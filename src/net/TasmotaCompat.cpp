// A small slice of Tasmota's HTTP command interface.
//
// Tasmota is the de-facto dialect for ESP-based devices: there is a large
// amount of tooling, and in particular a widely used Home Assistant
// integration, that already knows how to drive an air conditioner by sending
// `IRHVAC` at a Tasmota device. Answering to the same shape means all of it
// works here without anyone writing an adapter.
//
//   GET /cm?cmnd=Power%20On            -> {"POWER":"ON"}
//   GET /cm?cmnd=Power%20Toggle
//   GET /cm?cmnd=Status                -> a Tasmota-ish status document
//   GET /cm?cmnd=IRHVAC {"Power":"On","Mode":"Cool","FanSpeed":"Auto","Temp":23}
//
// This is a compatibility surface, not an emulation: it does not pretend to be
// Tasmota, and /api is the interface to build anything new against.
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include "../core/CommandBus.h"
#include "../core/Log.h"
#include "../core/Settings.h"
#include "HttpUtil.h"
#include "TasmotaCompat.h"
#include "generated/version.h"

namespace net {
namespace {
const char *kTag = "tasmota";

// Tasmota's IRHVAC spells things with capitals; ours are lower case.
bool modeFromTasmota(const String &text, stdAc::opmode_t &out) {
  String lower = text;
  lower.toLowerCase();
  if (lower == "fan" || lower == "fan_only") lower = "fan_only";
  return ac::parseMode(lower.c_str(), out);
}

bool fanFromTasmota(const String &text, stdAc::fanspeed_t &out) {
  String lower = text;
  lower.toLowerCase();
  if (lower == "mediumhigh" || lower == "medium_high") lower = "medium_high";
  return ac::parseFan(lower.c_str(), out);
}

void sendTasmotaState(AsyncWebServerRequest *request) {
  const ac::State &state = bus::commands.state();
  JsonDocument doc;
  doc["POWER"] = state.power ? "ON" : "OFF";
  sendJson(request, 200, doc);
}

void handleIrHvac(AsyncWebServerRequest *request, const String &json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    sendError(request, 400, F("IRHVAC needs a JSON object"));
    return;
  }
  JsonObjectConst object = doc.as<JsonObjectConst>();

  ac::Delta delta;

  const char *power = object["Power"];
  if (power != nullptr) {
    delta.hasPower = true;
    delta.power = String(power).equalsIgnoreCase("on") ||
                  String(power) == "1" || String(power).equalsIgnoreCase("true");
  }

  const char *mode = object["Mode"];
  if (mode != nullptr) {
    if (!modeFromTasmota(mode, delta.mode)) {
      sendError(request, 400, String(F("unknown Mode: ")) + mode);
      return;
    }
    delta.hasMode = true;
    if (power == nullptr) {
      delta.hasPower = true;
      delta.power = true;
    }
  }

  const char *fan = object["FanSpeed"];
  if (fan != nullptr) {
    if (!fanFromTasmota(fan, delta.fan)) {
      sendError(request, 400, String(F("unknown FanSpeed: ")) + fan);
      return;
    }
    delta.hasFan = true;
  }

  if (!object["Temp"].isNull()) {
    delta.hasDegrees = true;
    delta.degrees = object["Temp"].as<float>();
  }

  if (delta.empty()) {
    sendError(request, 400, F("nothing in that IRHVAC command to apply"));
    return;
  }

  const bus::Outcome outcome = bus::commands.apply(delta, src::Source::Api);
  if (!outcome.ok() && outcome.result != bus::Result::Deferred) {
    sendFailure(request, 409, outcome.code(), outcome.message);
    return;
  }

  const ac::State &state = bus::commands.state();
  JsonDocument reply;
  JsonObject hvac = reply["IRHVAC"].to<JsonObject>();
  hvac["Vendor"] = cfg::settings.ac.protocol;
  hvac["Power"] = state.power ? "On" : "Off";
  hvac["Mode"] = ac::modeName(state.mode);
  hvac["FanSpeed"] = ac::fanName(state.fanspeed);
  hvac["Temp"] = state.degrees;
  sendJson(request, 200, reply);
}

void handleCommand(AsyncWebServerRequest *request) {
  // Off unless somebody wants software that speaks Tasmota to drive this.
  if (!cfg::settings.compat.tasmota) {
    sendError(request, 404, F("Tasmota compatibility is switched off"));
    return;
  }
  if (!authorised(request)) return;

  String command = param(request, "cmnd");
  command.trim();
  if (command.isEmpty()) {
    sendError(request, 400, F("cmnd is required"));
    return;
  }

  String verb = command;
  String argument;
  const int space = command.indexOf(' ');
  if (space > 0) {
    verb = command.substring(0, space);
    argument = command.substring(space + 1);
    argument.trim();
  }
  verb.toLowerCase();

  LOG_D(kTag, "cmnd=%s", command.c_str());

  if (verb == "irhvac") {
    handleIrHvac(request, argument);
    return;
  }

  if (verb == "power") {
    ac::Delta delta;
    delta.hasPower = true;
    String lower = argument;
    lower.toLowerCase();

    if (lower.isEmpty()) {
      sendTasmotaState(request);   // a bare `Power` queries
      return;
    }
    if (lower == "toggle" || lower == "2") {
      delta.power = !bus::commands.state().power;
    } else {
      delta.power = lower == "on" || lower == "1" || lower == "true";
    }

    const bus::Outcome outcome = bus::commands.apply(delta, src::Source::Api);
    if (!outcome.ok() && outcome.result != bus::Result::Deferred) {
      sendFailure(request, 409, outcome.code(), outcome.message);
      return;
    }
    sendTasmotaState(request);
    return;
  }

  if (verb == "status" || verb == "state") {
    const ac::State &state = bus::commands.state();
    JsonDocument doc;
    JsonObject status = doc["Status"].to<JsonObject>();
    status["Module"] = "SLWF-12";
    status["FriendlyName"] = cfg::settings.device.name;
    status["Power"] = state.power ? 1 : 0;
    JsonObject firmware = doc["StatusFWR"].to<JsonObject>();
    firmware["Version"] = FW_VERSION;
    firmware["Hardware"] = "ESP8266";
    JsonObject hvac = doc["IRHVAC"].to<JsonObject>();
    hvac["Vendor"] = cfg::settings.ac.protocol;
    hvac["Power"] = state.power ? "On" : "Off";
    hvac["Mode"] = ac::modeName(state.mode);
    hvac["FanSpeed"] = ac::fanName(state.fanspeed);
    hvac["Temp"] = state.degrees;
    sendJson(request, 200, doc);
    return;
  }

  JsonDocument doc;
  doc["Command"] = "Unknown";
  sendJson(request, 400, doc);
}

}  // namespace

void registerTasmotaRoutes(AsyncWebServer &server) {
  server.on("/cm", HTTP_GET, handleCommand);
  LOG_I(kTag, "Tasmota-compatible /cm endpoint registered");
}

}  // namespace net
