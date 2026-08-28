#include "WebService.h"

#include <LittleFS.h>

#include "../core/CommandBus.h"
#include "../core/Log.h"
#include "../core/Settings.h"
#include "../ir/Learning.h"
#include "ApiRoutes.h"
#include "HttpUtil.h"
#include "WifiService.h"

namespace net {
namespace {
const char *kTag = "web";

// State pushes are coalesced: a client adapter can produce a burst of changes
// (a schedule firing, a learning sweep) and browsers only need the last one.
const uint32_t kStateCoalesceMs = 120;

// URLs the major mobile platforms probe to decide whether a network has a
// captive portal. Answering these with a redirect is what makes the setup
// sheet appear automatically.
bool isCaptiveProbe(const String &url) {
  return url.startsWith("/generate_204") ||         // Android
         url.startsWith("/gen_204") ||
         url.startsWith("/hotspot-detect.html") ||  // iOS / macOS
         url.startsWith("/library/test/success.html") ||
         url.startsWith("/connecttest.txt") ||      // Windows
         url.startsWith("/ncsi.txt") ||
         url.startsWith("/canonical.html") ||       // Ubuntu
         url.startsWith("/success.txt");
}
}  // namespace

WebService web;

// ---------------------------------------------------------------------------

void WebService::begin() {
  registerWebSocket();
  registerApiRoutes(server_);
  registerStaticRoutes();

  DefaultHeaders::Instance().addHeader("X-Content-Type-Options", "nosniff");
  DefaultHeaders::Instance().addHeader("Referrer-Policy", "same-origin");

  server_.begin();
  LOG_I(kTag, "web server listening on port 80");
}

void WebService::registerStaticRoutes() {
  // The UI is immutable per build, so it can be cached hard; index.html is
  // revalidated so a firmware update is picked up straight away.
  server_.serveStatic("/assets/", LittleFS, "/assets/")
      .setCacheControl("max-age=604800");
  server_.serveStatic("/lang/", LittleFS, "/lang/")
      .setCacheControl("max-age=3600");
  server_.serveStatic("/", LittleFS, "/")
      .setDefaultFile("index.html")
      .setCacheControl("no-cache");

  server_.onNotFound([this](AsyncWebServerRequest *request) {
    if (handleCaptivePortal(request)) return;

    if (request->url().startsWith("/api/")) {
      sendError(request, 404, F("no such endpoint"));
      return;
    }
    // Single-page app: unknown paths fall back to the shell so deep links work.
    if (LittleFS.exists("/index.html") || LittleFS.exists("/index.html.gz")) {
      request->send(LittleFS, "/index.html", "text/html");
      return;
    }
    request->send(404, "text/plain",
                  F("The web interface is not installed. Upload the LittleFS "
                    "image with: pio run -t uploadfs"));
  });
}

bool WebService::handleCaptivePortal(AsyncWebServerRequest *request) {
  if (!wifi.portalActive()) return false;

  const String host = request->host();
  const String self = WiFi.softAPIP().toString();
  if (host == self) return false;   // already talking to us by address

  if (!isCaptiveProbe(request->url()) && host.endsWith(".local")) return false;

  AsyncWebServerResponse *response = request->beginResponse(302);
  response->addHeader("Location", String("http://") + self + "/");
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
  return true;
}

// ---------------------------------------------------------------------------

void WebService::registerWebSocket() {
  ws_.onEvent([this](AsyncWebSocket *socket, AsyncWebSocketClient *client,
                     AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
      case WS_EVT_CONNECT: {
        LOG_D(kTag, "websocket client %u connected", client->id());
        // Send a full snapshot immediately so the UI can render without an
        // extra round trip.
        JsonDocument doc;
        doc["t"] = "hello";
        JsonObject state = doc["state"].to<JsonObject>();
        ac::toJson(bus::commands.state(), state);
        doc["revision"] = bus::commands.revision();
        doc["source"] = src::name(bus::commands.lastSource());
        JsonObject learning = doc["learning"].to<JsonObject>();
        learn::wizard.statusJson(learning);

        String payload;
        serializeJson(doc, payload);
        client->text(payload);
        break;
      }
      case WS_EVT_DISCONNECT:
        LOG_D(kTag, "websocket client %u disconnected", client->id());
        watchLog(client->id(), false);
        break;
      case WS_EVT_ERROR:
        LOG_W(kTag, "websocket error on client %u", client->id());
        break;
      case WS_EVT_DATA: {
        // The only thing a client says: whether it is looking at the log.
        AwsFrameInfo *info = static_cast<AwsFrameInfo *>(arg);
        if (info == nullptr || !info->final || info->index != 0 ||
            info->len != len || info->opcode != WS_TEXT || len > 64) {
          break;
        }
        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) break;
        if (String(doc["t"] | "") == "log") {
          watchLog(client->id(), doc["on"] | false);
        }
        break;
      }
      default:
        break;
    }
  });

  server_.addHandler(&ws_);
}

uint32_t WebService::clientCount() const {
  return const_cast<AsyncWebSocket &>(ws_).count();
}

// ---------------------------------------------------------------------------

void WebService::loop() {
  const uint32_t now = millis();

  if (now - lastCleanupAt_ > 1000) {
    lastCleanupAt_ = now;
    ws_.cleanupClients();
  }

  if (statePending_ && now - statePendingAt_ >= kStateCoalesceMs) {
    statePending_ = false;
    if (ws_.count() > 0) {
      JsonDocument doc;
      doc["t"] = "state";
      JsonObject state = doc["state"].to<JsonObject>();
      ac::toJson(bus::commands.state(), state);
      doc["revision"] = bus::commands.revision();
      doc["source"] = src::name(bus::commands.lastSource());

      String payload;
      serializeJson(doc, payload);
      ws_.textAll(payload);
    }
  }

  // Stream new log lines — but only to the clients that asked, and only when
  // there are any. A page sitting on the control screen pays nothing.
  if (anyoneWatchingLog() && log_::sequence() != lastLogSeqSent_) {
    String lines;
    lastLogSeqSent_ = log_::dump(lines, lastLogSeqSent_);
    if (lines.length() > 0 && lines.length() < 2048) {
      JsonDocument doc;
      doc["t"] = "log";
      doc["lines"] = lines;
      String payload;
      serializeJson(doc, payload);
      for (uint32_t id : logWatchers_) {
        if (id != 0) ws_.text(id, payload);
      }
    }
  } else if (!anyoneWatchingLog()) {
    // Stay level with the log while nobody is reading, so opening the page
    // does not dump everything that happened since boot.
    lastLogSeqSent_ = log_::sequence();
  }
}

void WebService::watchLog(uint32_t clientId, bool wanted) {
  for (uint32_t &id : logWatchers_) {
    if (id == clientId) {
      if (!wanted) id = 0;
      return;
    }
  }
  if (!wanted) return;
  for (uint32_t &id : logWatchers_) {
    if (id == 0) {
      id = clientId;
      // Start from where the log is now rather than replaying its history.
      lastLogSeqSent_ = log_::sequence();
      return;
    }
  }
  LOG_W(kTag, "too many log watchers; client %u will not get a live log",
        clientId);
}

bool WebService::anyoneWatchingLog() const {
  for (uint32_t id : logWatchers_) {
    if (id != 0) return true;
  }
  return false;
}

void WebService::broadcastState(src::Source source) {
  (void)source;
  statePending_ = true;
  statePendingAt_ = millis();
}

void WebService::broadcastLearning() {
  if (ws_.count() == 0) return;
  JsonDocument doc;
  doc["t"] = "learning";
  JsonObject learning = doc["learning"].to<JsonObject>();
  learn::wizard.statusJson(learning);

  String payload;
  serializeJson(doc, payload);
  ws_.textAll(payload);
}

void WebService::broadcastLog() {
  // Rewinding the marker makes loop() re-emit the tail on its next pass, which
  // is all a caller asking for "flush the log now" actually wants.
  if (lastLogSeqSent_ > 0) lastLogSeqSent_--;
}

void WebService::broadcastNotice(const char *level, const String &messageKey) {
  if (ws_.count() == 0) return;
  JsonDocument doc;
  doc["t"] = "notice";
  doc["level"] = level;
  doc["message"] = messageKey;

  String payload;
  serializeJson(doc, payload);
  ws_.textAll(payload);
}

}  // namespace net
