// Tasmota-compatible HTTP command endpoint. See TasmotaCompat.cpp for what is
// and is not supported.
#pragma once

#include <ESPAsyncWebServer.h>

namespace net {

void registerTasmotaRoutes(AsyncWebServer &server);

}  // namespace net
