#include "OtaService.h"

#include <ArduinoOTA.h>
#include <ESP8266httpUpdate.h>
#include <LittleFS.h>
#include <Updater.h>

#include "../core/CommandBus.h"
#include "../core/Log.h"
#include "../core/Settings.h"
#include "HttpUtil.h"
#include "WifiService.h"
#include "generated/version.h"

extern "C" uint32_t _FS_start;
extern "C" uint32_t _FS_end;

namespace net {
namespace {
const char *kTag = "ota";

// Default place to look for releases. Only ever handed to the browser.
const char *kDefaultManifest =
    "https://api.github.com/repos/smlight-tech/ir-remote-control/releases/latest";

bool looksLikeFilesystemImage(const String &filename) {
  String lower = filename;
  lower.toLowerCase();
  return lower.indexOf("littlefs") >= 0 || lower.indexOf("spiffs") >= 0 ||
         lower.indexOf("filesystem") >= 0 || lower.indexOf("fs.bin") >= 0;
}
}  // namespace

OtaService ota;

// ---------------------------------------------------------------------------

void OtaService::begin() {
  ArduinoOTA.setHostname(wifi.hostname().c_str());
  if (!cfg::settings.auth.pass.isEmpty()) {
    ArduinoOTA.setPassword(cfg::settings.auth.pass.c_str());
  }

  ArduinoOTA.onStart([this]() {
    inProgress_ = true;
    isFilesystem_ = ArduinoOTA.getCommand() == U_FS;
    if (isFilesystem_) LittleFS.end();
    LOG_I(kTag, "network update started (%s)",
          isFilesystem_ ? "filesystem" : "firmware");
  });
  ArduinoOTA.onProgress([this](unsigned int done, unsigned int total) {
    const uint8_t percent = total ? (done * 100U) / total : 0;
    if (percent != progressPercent_ && percent % 10 == 0) {
      progressPercent_ = percent;
      LOG_I(kTag, "update %u%%", percent);
    }
  });
  ArduinoOTA.onEnd([this]() {
    inProgress_ = false;
    LOG_I(kTag, "network update complete, restarting");
  });
  ArduinoOTA.onError([this](ota_error_t error) {
    inProgress_ = false;
    lastError_ = String(F("network update failed, code ")) + String(error);
    LOG_E(kTag, "%s", lastError_.c_str());
  });

  ArduinoOTA.begin();
  LOG_I(kTag, "network updates enabled on port 8266");
}

void OtaService::loop() {
  ArduinoOTA.handle();

  if (restartAt_ != 0 && millis() >= restartAt_) {
    LOG_W(kTag, "restarting now");
    // The one shutdown the device sees coming: write the state file while
    // there is still time, so even a restart followed by a power cut comes
    // back knowing what the air conditioner was doing.
    bus::commands.flush();
    Serial.flush();
    ESP.restart();
  }
}

void OtaService::scheduleRestart(uint32_t delayMs) {
  restartAt_ = millis() + delayMs;
  if (restartAt_ == 0) restartAt_ = 1;
}

// ---------------------------------------------------------------------------

bool OtaService::beginUpdate(const String &filename, size_t totalSize) {
  isFilesystem_ = looksLikeFilesystemImage(filename);

  size_t space;
  int command;
  if (isFilesystem_) {
    space = reinterpret_cast<size_t>(&_FS_end) - reinterpret_cast<size_t>(&_FS_start);
    command = U_FS;
    LittleFS.end();
  } else {
    space = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    command = U_FLASH;
  }

  if (totalSize > 0 && totalSize > space) {
    lastError_ = String(F("image is ")) + totalSize + F(" bytes but only ") +
                 space + F(" bytes are free");
    LOG_E(kTag, "%s", lastError_.c_str());
    return false;
  }

  if (!Update.begin(space, command)) {
    lastError_ = Update.getErrorString();
    LOG_E(kTag, "cannot start update: %s", lastError_.c_str());
    return false;
  }

  inProgress_ = true;
  progressPercent_ = 0;
  lastError_ = "";
  LOG_I(kTag, "upload started: %s (%s)", filename.c_str(),
        isFilesystem_ ? "filesystem" : "firmware");
  return true;
}

void OtaService::registerRoutes(AsyncWebServer &server) {
  server.on(
      "/api/ota/upload", HTTP_POST,
      [this](AsyncWebServerRequest *request) {
        if (!authorised(request)) return;

        const bool ok = !Update.hasError() && lastError_.isEmpty();
        JsonDocument doc;
        doc["ok"] = ok;
        doc["target"] = isFilesystem_ ? "filesystem" : "firmware";
        if (!ok) doc["error"] = lastError_.isEmpty() ? String(Update.getErrorString())
                                                     : lastError_;
        sendJson(request, ok ? 200 : 500, doc);

        inProgress_ = false;
        if (ok) scheduleRestart(1200);
      },
      [this](AsyncWebServerRequest *request, const String &filename, size_t index,
             uint8_t *data, size_t len, bool final) {
        if (index == 0) {
          // Challenge once, on the first chunk. Doing it per chunk would send
          // a 401 into the middle of an upload the browser is still streaming.
          if (!authorised(request)) return;
          if (!beginUpdate(filename, request->contentLength())) return;
        }
        if (!inProgress_) return;

        if (Update.write(data, len) != len) {
          lastError_ = Update.getErrorString();
          LOG_E(kTag, "write failed: %s", lastError_.c_str());
          Update.end();
          inProgress_ = false;
          return;
        }

        if (final) {
          if (!Update.end(true)) {
            lastError_ = Update.getErrorString();
            LOG_E(kTag, "finalise failed: %s", lastError_.c_str());
            return;
          }
          LOG_I(kTag, "upload complete: %u bytes", (unsigned)(index + len));
        }
      });

  onJson(server, "/api/ota/url", HTTP_POST,
         [this](AsyncWebServerRequest *request, JsonVariantConst body) {
           if (!authorised(request)) return;

           const String url = body["url"] | "";
           if (url.isEmpty()) {
             sendError(request, 400, F("url is required"));
             return;
           }
           if (!url.startsWith("http://")) {
             sendError(request, 400,
                       F("only plain http:// URLs are supported here — a TLS "
                         "session does not fit alongside the rest of the "
                         "firmware. Download the file and upload it instead."));
             return;
           }

           sendOk(request);
           LOG_W(kTag, "updating from %s", url.c_str());

           WiFiClient client;
           ESPhttpUpdate.setLedPin(-1);
           ESPhttpUpdate.rebootOnUpdate(true);
           const t_httpUpdate_return result =
               ESPhttpUpdate.update(client, url, FW_VERSION);
           if (result == HTTP_UPDATE_FAILED) {
             lastError_ = ESPhttpUpdate.getLastErrorString();
             LOG_E(kTag, "update failed: %s", lastError_.c_str());
           }
         });
}

// ---------------------------------------------------------------------------

void OtaService::statusJson(JsonObject out) const {
  out["version"] = FW_VERSION;
  out["commit"] = FW_COMMIT;
  out["built"] = FW_BUILD_DATE;
  out["inProgress"] = inProgress_;
  out["progress"] = progressPercent_;
  out["sketchSize"] = ESP.getSketchSize();
  out["freeSketchSpace"] = ESP.getFreeSketchSpace();
  out["channel"] = cfg::settings.ota.channel;
  out["manifestUrl"] = cfg::settings.ota.manifestUrl.isEmpty()
                           ? kDefaultManifest
                           : cfg::settings.ota.manifestUrl.c_str();
  if (!lastError_.isEmpty()) out["lastError"] = lastError_;
}

}  // namespace net
