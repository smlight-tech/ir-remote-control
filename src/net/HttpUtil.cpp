#include "HttpUtil.h"

#include "../core/Log.h"
#include "../core/Settings.h"

namespace net {
namespace {
const char *kTag = "http";
const char *kJson = "application/json";

// Accumulates a request body across chunk callbacks.
//
// This lives in AsyncWebServerRequest::_tempObject, which the server releases
// with a bare free() when a request is aborted. That rules out anything with a
// destructor — hence one flat malloc with the payload inline, so an abandoned
// upload cannot leak.
struct BodyBuffer {
  uint32_t length;
  uint32_t capacity;
  bool overflowed;
  char data[1];   // actually `capacity + 1` bytes
};

void releaseBuffer(AsyncWebServerRequest *request);

BodyBuffer *bufferFor(AsyncWebServerRequest *request) {
  return static_cast<BodyBuffer *>(request->_tempObject);
}

BodyBuffer *allocateBuffer(AsyncWebServerRequest *request, size_t capacity) {
  releaseBuffer(request);
  BodyBuffer *buffer =
      static_cast<BodyBuffer *>(malloc(sizeof(BodyBuffer) + capacity));
  if (buffer == nullptr) return nullptr;
  buffer->length = 0;
  buffer->capacity = capacity;
  buffer->overflowed = false;
  buffer->data[0] = '\0';
  request->_tempObject = buffer;
  return buffer;
}

void releaseBuffer(AsyncWebServerRequest *request) {
  if (request->_tempObject == nullptr) return;
  free(request->_tempObject);
  request->_tempObject = nullptr;
}

bool tokenMatches(AsyncWebServerRequest *request, const String &token) {
  if (token.isEmpty()) return false;

  if (request->hasHeader("Authorization")) {
    const String header = request->getHeader("Authorization")->value();
    if (header.startsWith("Bearer ") && header.substring(7) == token) return true;
  }
  if (request->hasHeader("X-Api-Key")) {
    if (request->getHeader("X-Api-Key")->value() == token) return true;
  }
  // Query parameter, so that a token can be pasted into a browser or a curl
  // one-liner without header gymnastics.
  if (request->hasParam("token") &&
      request->getParam("token")->value() == token) {
    return true;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------

void onJson(AsyncWebServer &server, const char *path, WebRequestMethod method,
            JsonRouteHandler handler) {
  server.on(
      path, method,
      // Called once the body has fully arrived.
      [handler](AsyncWebServerRequest *request) {
        BodyBuffer *buffer = bufferFor(request);

        if (buffer != nullptr && buffer->overflowed) {
          releaseBuffer(request);
          sendError(request, 413, F("request body too large"));
          return;
        }

        JsonDocument doc;
        DeserializationError err;
        if (buffer != nullptr && buffer->length > 0) {
          err = deserializeJson(doc, buffer->data, buffer->length);
        }
        releaseBuffer(request);

        if (err) {
          sendError(request, 400, String(F("malformed JSON: ")) + err.c_str());
          return;
        }
        handler(request, doc.as<JsonVariantConst>());
      },
      nullptr,
      // Body chunks.
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
         size_t total) {
        BodyBuffer *buffer = bufferFor(request);

        if (index == 0) {
          if (total > kMaxBodyBytes) {
            // Still allocate, so the completion handler can say why.
            buffer = allocateBuffer(request, 0);
            if (buffer != nullptr) buffer->overflowed = true;
            return;
          }
          // `total` is zero for chunked uploads; fall back to the cap.
          buffer = allocateBuffer(request, total > 0 ? total : kMaxBodyBytes);
          if (buffer == nullptr) {
            LOG_E(kTag, "out of memory buffering a %u byte body",
                  (unsigned)total);
            return;
          }
        }

        if (buffer == nullptr || buffer->overflowed) return;

        if (buffer->length + len > buffer->capacity) {
          buffer->overflowed = true;
          buffer->length = 0;
          return;
        }
        memcpy(buffer->data + buffer->length, data, len);
        buffer->length += len;
        buffer->data[buffer->length] = '\0';
      });
}

// ---------------------------------------------------------------------------

// Below this, building a response is likely to fail partway through — and a
// failed allocation inside the TCP stack is not an error anybody can catch,
// it is a reboot. Refusing early costs the caller a retry and costs everyone
// else nothing.
static const uint32_t kMinBlockForResponse = 4096;

bool lowOnMemory() {
  return ESP.getMaxFreeBlockSize() < kMinBlockForResponse;
}

void sendBusy(AsyncWebServerRequest *request) {
  LOG_W(kTag, "refusing a request: largest free block is %u bytes",
        ESP.getMaxFreeBlockSize());
  AsyncWebServerResponse *response = request->beginResponse(
      503, "text/plain", F("busy, try again"));
  response->addHeader("Retry-After", "2");
  request->send(response);
}

void sendJson(AsyncWebServerRequest *request, int status,
              const JsonDocument &doc) {
  // A short reply is always worth attempting; a long one on a heap in pieces
  // is what takes the device down.
  if (status < 400 && lowOnMemory() && measureJson(doc) > 512) {
    sendBusy(request);
    return;
  }
  AsyncResponseStream *response = request->beginResponseStream(kJson);
  response->setCode(status);
  response->addHeader("Cache-Control", "no-store");
  serializeJson(doc, *response);
  request->send(response);
}

void sendOk(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(request, 200, doc);
}

void sendError(AsyncWebServerRequest *request, int status,
               const String &message) {
  sendFailure(request, status, "error", message);
}

void sendFailure(AsyncWebServerRequest *request, int status, const char *code,
                 const String &message) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["code"] = code;
  doc["error"] = message;
  sendJson(request, status, doc);
}

// ---------------------------------------------------------------------------

String param(AsyncWebServerRequest *request, const char *name,
             const String &fallback) {
  if (request->hasParam(name)) return request->getParam(name)->value();
  if (request->hasParam(name, /*post=*/true))
    return request->getParam(name, true)->value();
  return fallback;
}

bool boolParam(AsyncWebServerRequest *request, const char *name, bool fallback) {
  const String value = param(request, name);
  if (value.isEmpty()) return fallback;
  return value == "1" || value.equalsIgnoreCase("true") ||
         value.equalsIgnoreCase("yes");
}

bool authorised(AsyncWebServerRequest *request) {
  const cfg::AuthSettings &auth = cfg::settings.auth;
  if (!auth.enabled) return true;

  if (tokenMatches(request, auth.token)) return true;
  if (request->authenticate(auth.user.c_str(), auth.pass.c_str())) return true;

  LOG_W(kTag, "unauthorised %s %s", request->methodToString(),
        request->url().c_str());
  request->requestAuthentication("SLWF-12");
  return false;
}

}  // namespace net
