// Small helpers shared by every HTTP route.
//
// ESPAsyncWebServer hands request bodies over in chunks and has no JSON body
// support that is safe to rely on across forks, so bodies are accumulated here
// and parsed with ArduinoJson directly.
#pragma once

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <functional>

namespace net {

// Bodies larger than this are rejected outright. The biggest legitimate body
// is a single raw IR code (~1024 timings), which fits comfortably.
static const size_t kMaxBodyBytes = 12288;

using JsonRouteHandler =
    std::function<void(AsyncWebServerRequest *, JsonVariantConst)>;

// Registers a route that parses its request body as JSON. An empty body is
// passed through as a null variant, which handlers may accept or reject.
void onJson(AsyncWebServer &server, const char *path, WebRequestMethod method,
            JsonRouteHandler handler);

// Whether there is a contiguous block big enough to build a reply in. The
// number that matters on this chip is not how much heap is free but whether
// any of it is in one piece.
bool lowOnMemory();
void sendBusy(AsyncWebServerRequest *request);

// --- responses -------------------------------------------------------------

void sendJson(AsyncWebServerRequest *request, int status, const JsonDocument &doc);
void sendOk(AsyncWebServerRequest *request);
void sendError(AsyncWebServerRequest *request, int status, const String &message);

// Builds {"ok":false,"error":...} for a bus::Outcome-style code/message pair.
void sendFailure(AsyncWebServerRequest *request, int status, const char *code,
                 const String &message);

// --- request helpers -------------------------------------------------------

String param(AsyncWebServerRequest *request, const char *name,
             const String &fallback = String());
bool boolParam(AsyncWebServerRequest *request, const char *name, bool fallback);

// True when the request may proceed. Sends the 401 itself when it may not.
bool authorised(AsyncWebServerRequest *request);

}  // namespace net
