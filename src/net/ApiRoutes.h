// Registration of the device's internal API.
//
// Everything the device can do is reachable here, and every other client
// (Telegram, MQTT, UART) is built on the same `bus::CommandBus` these routes
// call — so anything a browser can do, an integration can do too.
#pragma once

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

namespace net {

void registerApiRoutes(AsyncWebServer &server);

// Fills the object served by GET /api/status. Reused by the UART and Telegram
// adapters so every client reports the device identically.
void buildStatus(JsonObject out);

}  // namespace net
