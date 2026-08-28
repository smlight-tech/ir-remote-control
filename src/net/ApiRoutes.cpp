#include "ApiRoutes.h"

#include <IRutils.h>
#include <LittleFS.h>

#include "../app/Automations.h"
#include "../app/PeerClient.h"
#include "../app/Peers.h"
#include "../app/Scenes.h"
#include "../app/Scheduler.h"
#include "../app/Stats.h"
#include "../core/CommandBus.h"
#include "../core/Log.h"
#include "../core/Settings.h"
#include "../io/UartService.h"
#include "../ir/IrService.h"
#include "../ir/Learning.h"
#include "../ir/RemoteMap.h"
#include "HttpUtil.h"
#include "ModbusService.h"
#include "MqttService.h"
#include "OtaService.h"
#include "TasmotaCompat.h"
#include "TelegramService.h"
#include "WebhookService.h"
#include "WebService.h"
#include "WifiService.h"
#include "generated/version.h"

namespace net {
namespace {
const char *kTag = "api";

// Guard clause used at the top of every handler.
#define REQUIRE_AUTH(request)          \
  do {                                 \
    if (!authorised(request)) return;  \
  } while (0)

void sendOutcome(AsyncWebServerRequest *request, const bus::Outcome &outcome) {
  // A deferred command was accepted, it just has not run yet — so it reports
  // success, with 202 and the remaining hold, rather than an error the caller
  // would reasonably retry.
  const bool accepted = outcome.ok() || outcome.result == bus::Result::Deferred;

  if (accepted) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["result"] = outcome.code();
    if (!outcome.message.isEmpty()) doc["message"] = outcome.message;
    JsonObject state = doc["state"].to<JsonObject>();
    ac::toJson(bus::commands.state(), state);
    doc["revision"] = bus::commands.revision();
    doc["hold"] = bus::commands.restartHoldSeconds();
    sendJson(request, outcome.result == bus::Result::Deferred ? 202 : 200, doc);
    return;
  }

  int status = 400;
  switch (outcome.result) {
    case bus::Result::SourceDisabled: status = 403; break;
    case bus::Result::NotConfigured:  status = 409; break;
    case bus::Result::SendFailed:     status = 502; break;
    default:                          status = 400; break;
  }
  sendFailure(request, status, outcome.code(), outcome.message);
}

// The web UI speaks for a person; anything else hitting /api is an integration.
src::Source sourceOf(AsyncWebServerRequest *request) {
  const String declared = param(request, "source");
  src::Source source;
  if (!declared.isEmpty() && src::parse(declared.c_str(), source)) return source;

  // A browser sends its own origin; scripts and curl generally do not.
  if (request->hasHeader("X-Requested-With")) return src::Source::Web;
  return src::Source::Api;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

void handleState(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonObject state = doc["state"].to<JsonObject>();
  ac::toJson(bus::commands.state(), state);
  doc["revision"] = bus::commands.revision();
  doc["source"] = src::name(bus::commands.lastSource());
  doc["changedSecondsAgo"] =
      bus::commands.lastChangeMs() ? (millis() - bus::commands.lastChangeMs()) / 1000
                                   : -1;
  sendJson(request, 200, doc);
}

void handleSetState(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);

  JsonObjectConst object = body.as<JsonObjectConst>();
  if (object.isNull()) {
    sendError(request, 400, F("expected a JSON object"));
    return;
  }

  // Accepted here as well as on /api/resend so that "send it again" is
  // expressible in the same command shape as everything else — which is what
  // lets a paired device be driven through one endpoint.
  if (object["resend"] | false) {
    sendOutcome(request, bus::commands.resend(sourceOf(request)));
    return;
  }

  ac::Delta delta;
  String error;
  if (!ac::deltaFromJson(object, delta, error)) {
    sendError(request, 400, error);
    return;
  }

  sendOutcome(request, bus::commands.apply(delta, sourceOf(request)));
}

void handleResend(AsyncWebServerRequest *request, JsonVariantConst) {
  REQUIRE_AUTH(request);
  sendOutcome(request, bus::commands.resend(sourceOf(request)));
}

void handleConfigGet(AsyncWebServerRequest *request) {
  REQUIRE_AUTH(request);
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  cfg::settings.toJson(root, /*includeSecrets=*/false);
  sendJson(request, 200, doc);
}

void handleConfigSet(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);

  JsonObjectConst object = body.as<JsonObjectConst>();
  if (object.isNull()) {
    sendError(request, 400, F("expected a JSON object"));
    return;
  }

  const int8_t oldRx = cfg::settings.pins.irRx;
  const int8_t oldTx = cfg::settings.pins.irTx;
  const bool oldInverted = cfg::settings.pins.irTxInverted;
  const bool oldCelsius = cfg::settings.device.celsius;

  String error;
  if (!cfg::settings.fromJson(object, error)) {
    sendError(request, 400, error);
    return;
  }

  cfg::settings.save();
  log_::setLevel(static_cast<log_::Level>(cfg::settings.log.level));
  log_::setSerialEnabled(cfg::settings.log.serial && !cfg::settings.uart.enabled);

  if (cfg::settings.pins.irRx != oldRx || cfg::settings.pins.irTx != oldTx ||
      cfg::settings.pins.irTxInverted != oldInverted) {
    LOG_I(kTag, "IR pins changed, reopening the hardware");
    ir::irService.reconfigure();
  }

  // Every adapter re-reads its own section; each of these is cheap and
  // idempotent when nothing relevant changed.
  mqtt.reconfigure();
  telegram.reconfigure();
  webhooks.reconfigure();
  modbusService.reconfigure();
  io::uart.reconfigure();
  // Timezone and NTP server both take effect through configTime(), so a
  // changed clock setting applies now rather than at the next reboot.
  app::scheduler.reconfigure();

  // The unit is not a display preference: it goes out in the infrared frame,
  // so changing it means telling the air conditioner.
  if (cfg::settings.device.celsius != oldCelsius) {
    bus::commands.setUnit(cfg::settings.device.celsius, src::Source::Web);
  }

  JsonDocument doc;
  doc["ok"] = true;
  JsonObject saved = doc["config"].to<JsonObject>();
  cfg::settings.toJson(saved, /*includeSecrets=*/false);
  sendJson(request, 200, doc);
}

void handleFactoryReset(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);

  JsonObjectConst object = body.as<JsonObjectConst>();
  if (object.isNull() || object["confirm"].as<String>() != "factory-reset") {
    sendError(request, 400,
              F("send {\"confirm\":\"factory-reset\"} to erase every setting"));
    return;
  }

  sendOk(request);
  LOG_W(kTag, "factory reset requested over the API");
  cfg::settings.factoryReset();
  ota.scheduleRestart(1500);
}

void handleRestart(AsyncWebServerRequest *request, JsonVariantConst) {
  REQUIRE_AUTH(request);
  sendOk(request);
  ota.scheduleRestart(500);
}

// --- Wi-Fi -----------------------------------------------------------------

void handleWifiScan(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonArray networks = doc["networks"].to<JsonArray>();
  wifi.scanJson(networks);
  doc["scanning"] = wifi.scanInProgress();
  if (networks.size() == 0 && !wifi.scanInProgress()) wifi.requestScan();
  sendJson(request, 200, doc);
}

void handleWifiForget(AsyncWebServerRequest *request, JsonVariantConst) {
  wifi.forgetScan();
  sendOk(request);
}

void handleWifiConnect(AsyncWebServerRequest *request, JsonVariantConst body) {
  // Deliberately *not* behind auth: during first-run setup there is no
  // password yet, and the only way to reach this is to already be joined to
  // the device's own access point. Once credentials exist, changing them
  // requires auth like anything else.
  if (!cfg::settings.wifi.ssid.isEmpty()) REQUIRE_AUTH(request);

  JsonObjectConst object = body.as<JsonObjectConst>();
  const String ssid = object["ssid"] | "";
  if (ssid.isEmpty()) {
    sendError(request, 400, F("ssid is required"));
    return;
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = F("connecting");
  sendJson(request, 200, doc);

  wifi.connectTo(ssid, object["pass"] | "");
}

// --- diagnostics -----------------------------------------------------------

void handleLog(AsyncWebServerRequest *request) {
  REQUIRE_AUTH(request);
  const uint32_t since = param(request, "since", "0").toInt();

  String lines;
  const uint32_t last = log_::dump(lines, since);

  AsyncResponseStream *response = request->beginResponseStream("text/plain");
  response->addHeader("X-Log-Sequence", String(last));
  response->addHeader("Cache-Control", "no-store");
  response->print(lines);
  request->send(response);
}

void handleLogClear(AsyncWebServerRequest *request, JsonVariantConst) {
  REQUIRE_AUTH(request);
  log_::clear();
  sendOk(request);
}

void handleMetrics(AsyncWebServerRequest *request) {
  // Registered always, answering only when switched on: routes cannot be
  // removed from the server once added, so the gate lives here.
  if (!cfg::settings.compat.metrics) {
    sendError(request, 404, F("the metrics endpoint is switched off"));
    return;
  }
  REQUIRE_AUTH(request);

  const ac::State &state = bus::commands.state();
  AsyncResponseStream *response = request->beginResponseStream("text/plain; version=0.0.4");

  response->printf("# HELP slwf12_uptime_seconds Time since boot.\n"
                   "# TYPE slwf12_uptime_seconds counter\n"
                   "slwf12_uptime_seconds %lu\n",
                   millis() / 1000UL);
  response->printf("# HELP slwf12_free_heap_bytes Free heap.\n"
                   "# TYPE slwf12_free_heap_bytes gauge\n"
                   "slwf12_free_heap_bytes %u\n",
                   ESP.getFreeHeap());
  response->printf("# HELP slwf12_heap_fragmentation_percent Heap fragmentation.\n"
                   "# TYPE slwf12_heap_fragmentation_percent gauge\n"
                   "slwf12_heap_fragmentation_percent %u\n",
                   ESP.getHeapFragmentation());
  response->printf("# HELP slwf12_wifi_rssi_dbm Wi-Fi signal strength.\n"
                   "# TYPE slwf12_wifi_rssi_dbm gauge\n"
                   "slwf12_wifi_rssi_dbm %ld\n",
                   (long)WiFi.RSSI());
  response->printf("# HELP slwf12_ac_power Air conditioner power state.\n"
                   "# TYPE slwf12_ac_power gauge\n"
                   "slwf12_ac_power %d\n",
                   state.power ? 1 : 0);
  response->printf("# HELP slwf12_ac_target_temperature Target temperature.\n"
                   "# TYPE slwf12_ac_target_temperature gauge\n"
                   "slwf12_ac_target_temperature %.1f\n",
                   state.degrees);
  response->printf("# HELP slwf12_ir_captures_total IR frames received.\n"
                   "# TYPE slwf12_ir_captures_total counter\n"
                   "slwf12_ir_captures_total %lu\n",
                   (unsigned long)ir::irService.captureCount());
  response->printf("# HELP slwf12_ir_sends_total IR frames transmitted.\n"
                   "# TYPE slwf12_ir_sends_total counter\n"
                   "slwf12_ir_sends_total %lu\n",
                   (unsigned long)ir::irService.sendCount());

  request->send(response);
}

// --- infrared --------------------------------------------------------------

void handleIrLast(AsyncWebServerRequest *request) {
  REQUIRE_AUTH(request);
  const ir::Capture &capture = ir::irService.lastCapture();

  JsonDocument doc;
  doc["captures"] = ir::irService.captureCount();
  doc["secondsAgo"] = ir::irService.lastCaptureAt()
                          ? (millis() - ir::irService.lastCaptureAt()) / 1000
                          : -1;
  if (ir::irService.lastCaptureAt() != 0) {
    JsonObject last = doc["last"].to<JsonObject>();
    last["protocol"] = capture.protocolName();
    last["bits"] = capture.bits;
    last["marks"] = capture.rawLength;
    last["overflow"] = capture.overflow;
    last["decoded"] = capture.decodedState;
    last["synthesisable"] = capture.synthesisable();
    if (capture.decodedState) {
      JsonObject state = last["state"].to<JsonObject>();
      ac::toJson(capture.state, state);
    }
  }
  sendJson(request, 200, doc);
}

void handleIrSend(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);

  JsonObjectConst object = body.as<JsonObjectConst>();
  if (object.isNull()) {
    sendError(request, 400, F("expected a JSON object"));
    return;
  }

  const uint8_t repeats = object["repeats"] | 0;

  const char *key = object["key"];
  if (key != nullptr) {
    if (!ir::irService.sendStored(String(key), repeats)) {
      sendError(request, 404, F("no stored code with that key"));
      return;
    }
    sendOk(request);
    return;
  }

  JsonArrayConst timings = object["timings"];
  if (!timings.isNull()) {
    const size_t length = timings.size();
    if (length < 2 || length > ir::kMaxRawLength) {
      sendError(request, 400, F("timings must hold between 2 and 1024 values"));
      return;
    }
    uint16_t *buffer = static_cast<uint16_t *>(malloc(length * sizeof(uint16_t)));
    if (buffer == nullptr) {
      sendError(request, 507, F("not enough memory for that code"));
      return;
    }
    size_t i = 0;
    for (JsonVariantConst v : timings) buffer[i++] = v.as<uint16_t>();

    const bool ok = ir::irService.sendRaw(buffer, length, object["khz"] | 38,
                                          repeats);
    free(buffer);
    if (!ok) {
      sendError(request, 502, F("transmission failed"));
      return;
    }
    sendOk(request);
    return;
  }

  const char *protocol = object["protocol"];
  if (protocol != nullptr) {
    const decode_type_t type = strToDecodeType(protocol);
    if (type == decode_type_t::UNKNOWN) {
      sendError(request, 400, F("unknown protocol name"));
      return;
    }
    ac::State state = bus::commands.state();
    ac::Delta delta;
    String error;
    if (!ac::deltaFromJson(object, delta, error)) {
      sendError(request, 400, error);
      return;
    }
    delta.applyTo(state);
    if (!ir::irService.sendAs(type, object["model"] | -1, state)) {
      sendError(request, 502, F("that protocol could not encode this state"));
      return;
    }
    sendOk(request);
    return;
  }

  sendError(request, 400, F("provide one of: key, timings, protocol"));
}

// Written straight to the response rather than built as a document first.
//
// This is the largest thing the firmware ever sends — around sixty protocol
// names — and it used to cost twice over: a JsonDocument holding sixty objects
// with String names, and then a response buffer that had to grow past its
// first segment to hold the serialised form. On a device with nine kilobytes
// free and a heap in pieces, that allocation is the one that fails, and the
// symptom is an empty protocol list on the teaching page.
//
// Names only: the interface uses the name as both the value and the label, and
// the numeric ids were never read by anything. That halves the bytes and fits
// the whole reply in one buffer.
void handleProtocols(AsyncWebServerRequest *request) {
  AsyncResponseStream *response = request->beginResponseStream(
      "application/json; charset=utf-8");
  response->addHeader("Cache-Control", "no-store");

  response->print(F("{\"protocols\":["));
  uint16_t count = 0;
  for (uint16_t i = 1; i <= 255; i++) {
    const decode_type_t type = static_cast<decode_type_t>(i);
    if (!IRac::isProtocolSupported(type)) continue;
    if (count++) response->print(',');
    response->print('"');
    response->print(typeToString(type));
    response->print('"');
  }
  response->printf("],\"count\":%u}", count);
  request->send(response);
}

// --- learned codes ---------------------------------------------------------

void handleCodesList(AsyncWebServerRequest *request) {
  REQUIRE_AUTH(request);
  JsonDocument doc;
  JsonArray list = doc["codes"].to<JsonArray>();
  ir::codes.listJson(list);
  doc["count"] = ir::codes.count();
  doc["bytes"] = ir::codes.bytesUsed();
  sendJson(request, 200, doc);
}

void handleCodeGet(AsyncWebServerRequest *request) {
  REQUIRE_AUTH(request);
  const String key = param(request, "key");
  if (key.isEmpty()) {
    sendError(request, 400, F("key is required"));
    return;
  }

  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  if (!ir::codes.codeToJson(key, root)) {
    sendError(request, 404, F("no stored code with that key"));
    return;
  }
  sendJson(request, 200, doc);
}

void handleCodePut(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);

  JsonObjectConst object = body.as<JsonObjectConst>();
  String key = param(request, "key");
  if (object["key"].is<const char *>()) key = object["key"].as<const char *>();
  JsonArrayConst timings = object["timings"];

  if (key.isEmpty() || timings.isNull()) {
    sendError(request, 400, F("key and timings are required"));
    return;
  }
  const size_t length = timings.size();
  if (length < 2 || length > ir::kMaxRawLength) {
    sendError(request, 400, F("timings must hold between 2 and 1024 values"));
    return;
  }

  uint16_t *buffer = static_cast<uint16_t *>(malloc(length * sizeof(uint16_t)));
  if (buffer == nullptr) {
    sendError(request, 507, F("not enough memory for that code"));
    return;
  }
  size_t i = 0;
  for (JsonVariantConst v : timings) buffer[i++] = v.as<uint16_t>();

  const bool ok = ir::codes.store(key, buffer, length, object["khz"] | 38);
  free(buffer);

  if (!ok) {
    sendError(request, 400, F("the code could not be stored"));
    return;
  }
  if (!cfg::settings.ac.useLearnedCodes) {
    cfg::settings.ac.useLearnedCodes = true;
    cfg::settings.touch();
  }
  sendOk(request);
}

void handleCodeDelete(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);
  String key = param(request, "key");
  if (body["key"].is<const char *>()) key = body["key"].as<const char *>();
  if (!ir::codes.remove(key)) {
    sendError(request, 404, F("no stored code with that key"));
    return;
  }
  sendOk(request);
}

void handleCodesClear(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);
  if (body["confirm"].as<String>() != "clear-codes") {
    sendError(request, 400,
              F("send {\"confirm\":\"clear-codes\"} to erase every learned code"));
    return;
  }
  ir::codes.clear();
  sendOk(request);
}

// --- learning --------------------------------------------------------------

void handleLearnStatus(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  learn::wizard.statusJson(root);
  sendJson(request, 200, doc);
}

void handleLearnStart(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);

  JsonObjectConst object = body.as<JsonObjectConst>();
  const String mode = object["mode"] | "identify";
  String error;
  bool started = false;

  if (mode == "identify") {
    started = learn::wizard.startIdentify(error);
  } else if (mode == "sweep") {
    started = learn::wizard.startSweep(error);
  } else if (mode == "record") {
    learn::Plan plan = learn::Wizard::defaultPlan();
    JsonObjectConst planJson = object["plan"];
    if (!planJson.isNull() &&
        !learn::Wizard::planFromJson(planJson, plan, error)) {
      sendError(request, 400, error);
      return;
    }
    started = learn::wizard.startRecord(plan, error);
  } else if (mode == "bind") {
    started = learn::wizard.startBind(object["action"] | "",
                                      object["label"] | "",
                                      object["argument"] | "", error);
  } else if (mode == "keys") {
    std::vector<String> keys;
    for (JsonVariantConst v : object["keys"].as<JsonArrayConst>())
      keys.push_back(String(v.as<const char *>()));
    started = learn::wizard.startRecordKeys(keys, error);
  } else {
    sendError(request, 400,
              F("mode must be identify, sweep, record, keys or bind"));
    return;
  }

  if (!started) {
    sendError(request, 409, error);
    return;
  }

  web.broadcastLearning();
  handleLearnStatus(request);
}

void handleLearnConfirm(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);
  learn::wizard.confirm(body["ok"] | false);
  web.broadcastLearning();
  handleLearnStatus(request);
}

void handleLearnSkip(AsyncWebServerRequest *request, JsonVariantConst) {
  REQUIRE_AUTH(request);
  learn::wizard.skip();
  web.broadcastLearning();
  handleLearnStatus(request);
}

void handleLearnCancel(AsyncWebServerRequest *request, JsonVariantConst) {
  REQUIRE_AUTH(request);
  learn::wizard.cancel();
  web.broadcastLearning();
  handleLearnStatus(request);
}

// --- schedules -------------------------------------------------------------

void handleSchedulesGet(AsyncWebServerRequest *request) {
  REQUIRE_AUTH(request);
  JsonDocument doc;
  JsonArray rules = doc["schedules"].to<JsonArray>();
  app::scheduler.toJson(rules);
  doc["enabled"] = cfg::settings.schedule.enabled;
  doc["timeSynced"] = app::scheduler.timeSynced();
  doc["now"] = app::scheduler.localTimeString();
  sendJson(request, 200, doc);
}

void handleSchedulesSet(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);

  JsonArrayConst rules = body["schedules"].as<JsonArrayConst>();
  if (rules.isNull()) {
    sendError(request, 400, F("expected {\"schedules\":[...]}"));
    return;
  }
  String error;
  if (!app::scheduler.fromJson(rules, error)) {
    sendError(request, 400, error);
    return;
  }
  app::scheduler.save();
  sendOk(request);
}

// --- clock -----------------------------------------------------------------

// Sets the clock from whatever the caller believes the time to be — in
// practice the browser's, which is right far more often than not. For a device
// on a network with no route out, this is the only way it will ever know the
// time, and schedules are useless without it.
void handleTimeSet(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);

  const uint32_t epoch = body["epoch"] | 0U;
  if (epoch == 0) {
    sendError(request, 400, F("expected {\"epoch\":<seconds since 1970>}"));
    return;
  }
  if (!app::scheduler.setTime(static_cast<time_t>(epoch))) {
    sendError(request, 400, F("that is not a believable time"));
    return;
  }
  sendOk(request);
}

// --- scenes ----------------------------------------------------------------

void handleScenesGet(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonArray list = doc["scenes"].to<JsonArray>();
  app::scenes.toJson(list);
  const app::Scene *active = app::scenes.matching(bus::commands.state());
  if (active != nullptr) doc["active"] = active->id;
  sendJson(request, 200, doc);
}

void handleScenesSet(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);

  JsonArrayConst list = body["scenes"].as<JsonArrayConst>();
  if (list.isNull()) {
    sendError(request, 400, F("expected {\"scenes\":[...]}"));
    return;
  }
  String error;
  if (!app::scenes.fromJson(list, error)) {
    sendError(request, 400, error);
    return;
  }
  app::scenes.save();
  handleScenesGet(request);
}

void handleSceneApply(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);

  String id = param(request, "id");
  if (body["id"].is<const char *>()) id = body["id"].as<const char *>();
  if (id.isEmpty()) {
    sendError(request, 400, F("id is required"));
    return;
  }

  String error;
  if (!app::scenes.apply(id, sourceOf(request), error)) {
    sendError(request, 409, error);
    return;
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["scene"] = id;
  JsonObject state = doc["state"].to<JsonObject>();
  ac::toJson(bus::commands.state(), state);
  doc["hold"] = bus::commands.restartHoldSeconds();
  sendJson(request, 200, doc);
}

// --- bound buttons on other remotes ----------------------------------------

void handleRemotesGet(AsyncWebServerRequest *request) {
  REQUIRE_AUTH(request);
  JsonDocument doc;
  JsonArray list = doc["buttons"].to<JsonArray>();
  ir::remotes.toJson(list);

  JsonArray actions = doc["actions"].to<JsonArray>();
  for (const char *name :
       {"power_toggle", "power_on", "power_off", "temp_up", "temp_down",
        "mode_next", "fan_next", "swing_toggle", "scene", "resend"}) {
    actions.add(name);
  }
  sendJson(request, 200, doc);
}

void handleRemoteDelete(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);
  if (!body["index"].is<int>()) {
    sendError(request, 400, F("index is required"));
    return;
  }
  if (!ir::remotes.remove(body["index"].as<int>())) {
    sendError(request, 404, F("no bound button at that position"));
    return;
  }
  handleRemotesGet(request);
}

void handleRemotesClear(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);
  if (body["confirm"].as<String>() != "clear-buttons") {
    sendError(request, 400,
              F("send {\"confirm\":\"clear-buttons\"} to forget every button"));
    return;
  }
  ir::remotes.clear();
  sendOk(request);
}

// --- paired devices --------------------------------------------------------

void handlePeersGet(AsyncWebServerRequest *request) {
  REQUIRE_AUTH(request);
  JsonDocument doc;
  JsonArray list = doc["peers"].to<JsonArray>();
  app::peers.toJson(list);
  JsonArray live = doc["status"].to<JsonArray>();
  app::peerClient.statusJson(live);
  JsonArray found = doc["candidates"].to<JsonArray>();
  app::peers.candidatesJson(found);
  doc["discovering"] = app::peers.discovering();
  // This device's own identity, so a peer list can show "this one" alongside
  // the rest without the browser having to fetch it separately.
  doc["self"]["id"] = cfg::settings.chipId();
  doc["self"]["type"] = "slwf12";
  doc["self"]["name"] = cfg::settings.device.name;
  sendJson(request, 200, doc);
}

void handlePeersSet(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);

  JsonArrayConst list = body["peers"].as<JsonArrayConst>();
  if (list.isNull()) {
    sendError(request, 400, F("expected {\"peers\":[...]}"));
    return;
  }
  String error;
  if (!app::peers.fromJson(list, error)) {
    sendError(request, 400, error);
    return;
  }
  app::peers.save();
  app::peerClient.reconfigure();
  handlePeersGet(request);
}

void handlePeersDiscover(AsyncWebServerRequest *request, JsonVariantConst) {
  REQUIRE_AUTH(request);
  // Blocks for a couple of seconds; see the note in Peers::discover().
  app::peers.discover();
  handlePeersGet(request);
}


void handlePeerCommand(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);

  const String id = body["id"] | "";
  JsonObjectConst command = body["command"].as<JsonObjectConst>();
  if (id.isEmpty() || command.isNull()) {
    sendError(request, 400, F("expected {\"id\":..., \"command\":{...}}"));
    return;
  }

  // The command body is that device's own shape, built by whoever knows the
  // type — the automation editor or the interface — from devicetypes.json.
  JsonDocument payload;
  payload.set(command);

  String error;
  if (!app::peerClient.command(id, payload, error)) {
    sendError(request, 409, error);
    return;
  }
  // Queued, not confirmed: the next poll reports what actually happened.
  JsonDocument doc;
  doc["ok"] = true;
  doc["result"] = "queued";
  sendJson(request, 202, doc);
}


// --- automations -----------------------------------------------------------

void handleAutomationsGet(AsyncWebServerRequest *request) {
  REQUIRE_AUTH(request);
  JsonDocument doc;
  JsonArray list = doc["automations"].to<JsonArray>();
  app::automations.toJson(list);
  sendJson(request, 200, doc);
}

void handleAutomationsSet(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);

  JsonArrayConst list = body["automations"].as<JsonArrayConst>();
  if (list.isNull()) {
    sendError(request, 400, F("expected {\"automations\":[...]}"));
    return;
  }
  String error;
  if (!app::automations.fromJson(list, error)) {
    sendError(request, 400, error);
    return;
  }
  app::automations.save();
  handleAutomationsGet(request);
}

void handleAutomationRun(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);
  String error;
  if (!app::automations.fire(body["id"] | "", error)) {
    sendError(request, 409, error);
    return;
  }
  sendOk(request);
}

// --- statistics ------------------------------------------------------------

void handleStatsReset(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);
  if (body["confirm"].as<String>() != "reset-stats") {
    sendError(request, 400,
              F("send {\"confirm\":\"reset-stats\"} to clear the counters"));
    return;
  }
  app::stats.reset();
  sendOk(request);
}

// --- profile export --------------------------------------------------------

// Everything needed to describe this AC to the shared database, minus the raw
// codes themselves — the browser fetches those one at a time and assembles the
// submission file, because a full profile does not fit in the device's RAM.
void handleProfile(AsyncWebServerRequest *request) {
  REQUIRE_AUTH(request);
  const cfg::AcSettings &acCfg = cfg::settings.ac;

  JsonDocument doc;
  doc["schema"] = 1;
  doc["brand"] = acCfg.brand;
  doc["model"] = acCfg.modelName;
  doc["profileId"] = acCfg.profileId;
  doc["protocol"] = acCfg.protocol;
  doc["protocolModel"] = acCfg.model;
  doc["useLearnedCodes"] = acCfg.useLearnedCodes;
  doc["minTemp"] = acCfg.minTemp;
  doc["maxTemp"] = acCfg.maxTemp;
  doc["tempStep"] = acCfg.tempStep;
  doc["carrierKhz"] = cfg::settings.pins.irCarrierKhz;
  doc["firmware"] = FW_VERSION;

  JsonArray codes = doc["codes"].to<JsonArray>();
  ir::codes.listJson(codes);
  sendJson(request, 200, doc);
}

// --- languages -------------------------------------------------------------
//
// English is in the filesystem image; everything else is installed on demand.
// Not to save space — the filesystem is 2 MB and a pack is twenty kilobytes —
// but so a translation can be added or corrected without anybody reflashing a
// filesystem, which is the difference between a language being maintained and
// being frozen at whatever shipped.
//
// The *browser* fetches the pack and posts it here. That keeps a TLS session,
// which costs most of this chip's free heap, off the device entirely, and it
// works when the device has no route out — which is most of them, and all of
// them during first-run setup.

// A code names a file, so it is checked as one: letters and one optional
// dash, nothing that could climb out of /lang.
bool validLanguageCode(const String &code) {
  if (code.length() < 2 || code.length() > 5) return false;
  for (size_t i = 0; i < code.length(); i++) {
    const char c = code[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c == '-' && i > 0);
    if (!ok) return false;
  }
  return true;
}

bool languageInstalled(const String &code) {
  return LittleFS.exists("/lang/" + code + ".json") ||
         LittleFS.exists("/lang/" + code + ".json.gz");
}

void handleLanguages(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonArray list = doc["languages"].to<JsonArray>();
  Dir dir = LittleFS.openDir("/lang");
  while (dir.next()) {
    String name = dir.fileName();
    if (name.endsWith(".gz")) name.remove(name.length() - 3);
    if (!name.endsWith(".json")) continue;
    name.remove(name.length() - 5);
    if (name == "index") continue;   // the catalogue, not a language
    list.add(name);
  }
  doc["current"] = cfg::settings.device.language;

  // The catalogue of what exists, whether or not it is here yet, so the
  // interface can offer a language it has to go and fetch first.
  File catalogue = LittleFS.open("/lang/index.json", "r");
  if (catalogue) {
    JsonDocument known;
    if (deserializeJson(known, catalogue) == DeserializationError::Ok) {
      doc["catalogue"] = known;
    }
    catalogue.close();
  }
  sendJson(request, 200, doc);
}

// A pack is twenty to thirty kilobytes — more than the JSON body limit and
// more than the free heap — so it is written to the filesystem as it arrives
// and never assembled in memory. It lands in a temporary file and is renamed
// only once the whole thing is there, so an interrupted upload leaves the
// language that was working still working.
File langUpload;
String langUploadCode;
bool langUploadFailed = false;

const size_t kMaxLanguageBytes = 65536;

void handleLanguageBody(AsyncWebServerRequest *request, uint8_t *data,
                        size_t len, size_t index, size_t total) {
  if (index == 0) {
    langUploadFailed = true;   // until proven otherwise
    if (!authorised(request)) return;

    const String code = param(request, "code");
    if (!validLanguageCode(code)) {
      sendError(request, 400, F("that is not a language code"));
      return;
    }
    if (total == 0 || total > kMaxLanguageBytes) {
      sendError(request, 413, F("that language pack is too large"));
      return;
    }

    FSInfo info;
    LittleFS.info(info);
    if (info.totalBytes - info.usedBytes < total + 4096) {
      sendError(request, 507, F("not enough room left on the device"));
      return;
    }

    langUploadCode = code;
    langUpload = LittleFS.open("/lang/upload.tmp", "w");
    if (!langUpload) {
      sendError(request, 500, F("could not open the file for writing"));
      return;
    }
    langUploadFailed = false;
  }

  if (langUploadFailed || !langUpload) return;

  if (langUpload.write(data, len) != len) {
    langUploadFailed = true;
    langUpload.close();
    LittleFS.remove("/lang/upload.tmp");
    sendError(request, 507, F("the device ran out of room"));
  }
}

void handleLanguageInstall(AsyncWebServerRequest *request) {
  if (langUploadFailed) return;      // the body handler already answered
  if (!langUpload) {
    sendError(request, 400, F("no language pack was sent"));
    return;
  }
  langUpload.close();

  const String path = "/lang/" + langUploadCode + ".json";
  // A pack that shipped in the image is compressed, and the static handler
  // prefers the compressed copy — so the old one goes, or the new one would
  // never be seen.
  LittleFS.remove(path + ".gz");
  LittleFS.remove(path);
  if (!LittleFS.rename("/lang/upload.tmp", path)) {
    LittleFS.remove("/lang/upload.tmp");
    sendError(request, 500, F("the language pack could not be saved"));
    return;
  }

  LOG_I(kTag, "language pack '%s' installed", langUploadCode.c_str());
  sendOk(request);
}

void handleLanguageRemove(AsyncWebServerRequest *request, JsonVariantConst body) {
  REQUIRE_AUTH(request);

  const String code = body["code"] | "";
  if (!validLanguageCode(code)) {
    sendError(request, 400, F("that is not a language code"));
    return;
  }
  // English is the one that cannot be removed: it is the fallback every other
  // language falls back *to*, and a device with no language at all would need
  // a reflash to recover.
  if (code == "en") {
    sendError(request, 400, F("English cannot be removed"));
    return;
  }
  if (!languageInstalled(code)) {
    sendError(request, 404, F("that language is not installed"));
    return;
  }

  LittleFS.remove("/lang/" + code + ".json");
  LittleFS.remove("/lang/" + code + ".json.gz");
  if (cfg::settings.device.language == code) {
    cfg::settings.device.language = "en";
    cfg::settings.save();
  }
  LOG_I(kTag, "language pack '%s' removed", code.c_str());
  sendOk(request);
}

// tools/build_web.py stamps the filesystem image with the digest of the
// sources it was built from. Read once — it cannot change without a reboot,
// since replacing the image restarts the device.
struct WebManifest {
  String version;
  String built;
};

const WebManifest &manifest() {
  static WebManifest cached;
  static bool read = false;
  if (read) return cached;
  read = true;

  File file = LittleFS.open("/manifest.json", "r");
  if (!file) return cached;
  JsonDocument doc;
  if (deserializeJson(doc, file) == DeserializationError::Ok) {
    cached.version = doc["version"] | "";
    cached.built = doc["built"] | "";
  }
  file.close();
  return cached;
}

}  // namespace

// ---------------------------------------------------------------------------

// The parts that actually change while the page is open: how the device is
// coping, what the air conditioner is set to, and what time it thinks it is.
// Around three hundred bytes against two and a half thousand.
void buildBriefStatus(JsonObject out) {
  JsonObject device = out["device"].to<JsonObject>();
  device["uptime"] = millis() / 1000UL;
  device["freeHeap"] = ESP.getFreeHeap();
  device["fragmentation"] = ESP.getHeapFragmentation();
  device["maxFreeBlock"] = ESP.getMaxFreeBlockSize();

  JsonObject network = out["network"].to<JsonObject>();
  network["online"] = wifi.isOnline();
  network["rssi"] = WiFi.RSSI();

  JsonObject clock = out["clock"].to<JsonObject>();
  clock["epoch"] = static_cast<uint32_t>(app::scheduler.now());
  clock["offset"] = app::scheduler.utcOffset();
  clock["synced"] = app::scheduler.timeSynced();

  JsonObject state = out["state"].to<JsonObject>();
  ac::toJson(bus::commands.state(), state);
  out["revision"] = bus::commands.revision();
  out["lastSource"] = src::name(bus::commands.lastSource());

  JsonObject ir = out["ir"].to<JsonObject>();
  ir["captures"] = ir::irService.captureCount();
  ir["sends"] = ir::irService.sendCount();
}

void buildStatus(JsonObject out) {
  JsonObject device = out["device"].to<JsonObject>();
  device["name"] = cfg::settings.device.name;
  device["id"] = cfg::settings.chipId();
  // How another device identifies this one. `type` matches an id in
  // devicetypes.json; `model` is for people to read.
  device["type"] = "slwf12";
  device["model"] = "SLWF-12";
  device["firmware"] = FW_VERSION;
  device["commit"] = FW_COMMIT;
  device["built"] = FW_BUILD_DATE;
  // The interface lives in a second image that is flashed — and updated — on
  // its own, so its version is a separate fact from the firmware's.
  device["web"] = manifest().version;
  device["webBuilt"] = manifest().built;
  device["schema"] = cfg::kSchemaVersion;
  device["uptime"] = millis() / 1000UL;
  device["freeHeap"] = ESP.getFreeHeap();
  device["fragmentation"] = ESP.getHeapFragmentation();
  device["maxFreeBlock"] = ESP.getMaxFreeBlockSize();
  device["resetReason"] = ESP.getResetReason();
  device["fsUsed"] = ir::codes.bytesUsed();
  device["language"] = cfg::settings.device.language;

  // The clock, so the interface can show the *device's* local time rather
  // than the browser's. An offset rather than a timezone name: it already
  // accounts for summer time, and the browser needs no rule tables to use it.
  JsonObject clock = out["clock"].to<JsonObject>();
  clock["epoch"] = static_cast<uint32_t>(app::scheduler.now());
  clock["offset"] = app::scheduler.utcOffset();
  clock["synced"] = app::scheduler.timeSynced();
  clock["manual"] = app::scheduler.manuallySet();
  clock["tz"] = cfg::settings.time.timezone;
  clock["ntp"] = cfg::settings.time.ntpServer;
  clock["sunrise"] = app::scheduler.sunriseMinutes();
  clock["sunset"] = app::scheduler.sunsetMinutes();
  clock["daylight"] = app::scheduler.daylight();
  clock["lat"] = cfg::settings.time.latitude;
  clock["lon"] = cfg::settings.time.longitude;

  JsonObject network = out["network"].to<JsonObject>();
  wifi.statusJson(network);

  JsonObject state = out["state"].to<JsonObject>();
  ac::toJson(bus::commands.state(), state);
  out["revision"] = bus::commands.revision();
  out["lastSource"] = src::name(bus::commands.lastSource());

  JsonObject acInfo = out["ac"].to<JsonObject>();
  acInfo["configured"] = ir::irService.ready();
  acInfo["protocol"] = cfg::settings.ac.protocol;
  acInfo["brand"] = cfg::settings.ac.brand;
  acInfo["model"] = cfg::settings.ac.modelName;
  acInfo["useLearnedCodes"] = cfg::settings.ac.useLearnedCodes;
  acInfo["learnedCodes"] = ir::codes.count();
  acInfo["minTemp"] = cfg::settings.ac.minTemp;
  acInfo["maxTemp"] = cfg::settings.ac.maxTemp;
  acInfo["tempStep"] = cfg::settings.ac.tempStep;
  acInfo["restartHold"] = bus::commands.restartHoldSeconds();
  acInfo["pendingStart"] = bus::commands.hasPendingStart();
  acInfo["minOffSeconds"] = cfg::settings.ac.minOffSeconds;

  JsonObject statistics = out["stats"].to<JsonObject>();
  app::stats.toJson(statistics);

  const app::Scene *active = app::scenes.matching(bus::commands.state());
  if (active != nullptr) out["scene"] = active->id;

  JsonObject irInfo = out["ir"].to<JsonObject>();
  irInfo["rxPin"] = cfg::settings.pins.irRx;
  irInfo["txPin"] = cfg::settings.pins.irTx;
  irInfo["receiverActive"] = ir::irService.receiverActive();
  irInfo["captures"] = ir::irService.captureCount();
  irInfo["sends"] = ir::irService.sendCount();

  JsonObject sources = out["sources"].to<JsonObject>();
  for (uint8_t i = 0; i < src::kCount; i++) {
    const src::Source source = static_cast<src::Source>(i);
    if (!src::isGateable(source)) continue;
    sources[src::name(source)] = cfg::settings.isSourceEnabled(source);
  }

  JsonObject learning = out["learning"].to<JsonObject>();
  learn::wizard.statusJson(learning);

  JsonObject otaInfo = out["ota"].to<JsonObject>();
  ota.statusJson(otaInfo);

  // Every integration reports itself the same way, so one status document
  // answers "is it connected, and if not why not" for all of them.
  JsonObject integrations = out["integrations"].to<JsonObject>();
  mqtt.statusJson(integrations["mqtt"].to<JsonObject>());
  telegram.statusJson(integrations["telegram"].to<JsonObject>());
  webhooks.statusJson(integrations["webhook"].to<JsonObject>());
  modbusService.statusJson(integrations["modbus"].to<JsonObject>());
}

// ---------------------------------------------------------------------------

// The routing table. Static, so it lives in flash: the paths and the function
// pointers cost no RAM, where the equivalent server.on() calls cost about 185
// bytes each in handler objects, captured std::functions and URI Strings.
struct GetRoute {
  const char *path;
  void (*handler)(AsyncWebServerRequest *);
};

struct JsonRoute {
  const char *path;
  void (*handler)(AsyncWebServerRequest *, JsonVariantConst);
};

void handleStatus(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();

  // The full document is two and a half kilobytes, and the interface used to
  // ask for all of it every fifteen seconds. Almost none of it changes: the
  // name, the pins, the protocol and the network are settled by the time the
  // page has loaded once. `?brief=1` returns only what moves.
  if (param(request, "brief") == "1") {
    buildBriefStatus(root);
  } else {
    buildStatus(root);
  }
  sendJson(request, 200, doc);
}

void handleWebhookTest(AsyncWebServerRequest *request, JsonVariantConst) {
  if (!authorised(request)) return;
  String error;
  if (!webhooks.sendNow(error)) {
    sendError(request, 409, error);
    return;
  }
  sendOk(request);
}

const GetRoute kGetRoutes[] = {
    {"/api/status", handleStatus},
    {"/api/state", handleState},
    {"/api/config", handleConfigGet},
    {"/api/wifi/scan", handleWifiScan},
    {"/api/log", handleLog},
    {"/api/metrics", handleMetrics},
    {"/api/ir/last", handleIrLast},
    {"/api/protocols", handleProtocols},
    {"/api/codes", handleCodesList},
    {"/api/code", handleCodeGet},
    {"/api/learn", handleLearnStatus},
    {"/api/schedules", handleSchedulesGet},
    {"/api/scenes", handleScenesGet},
    {"/api/peers", handlePeersGet},
    {"/api/automations", handleAutomationsGet},
    {"/api/remotes", handleRemotesGet},
    {"/api/profile", handleProfile},
    {"/api/languages", handleLanguages},
};

const JsonRoute kPostRoutes[] = {
    {"/api/state", handleSetState},
    {"/api/resend", handleResend},
    {"/api/config", handleConfigSet},
    {"/api/factory-reset", handleFactoryReset},
    {"/api/restart", handleRestart},
    {"/api/wifi/connect", handleWifiConnect},
    {"/api/wifi/forget-scan", handleWifiForget},
    {"/api/log/clear", handleLogClear},
    {"/api/ir/send", handleIrSend},
    {"/api/code", handleCodePut},
    {"/api/code/delete", handleCodeDelete},
    {"/api/codes/clear", handleCodesClear},
    {"/api/learn/start", handleLearnStart},
    {"/api/learn/confirm", handleLearnConfirm},
    {"/api/learn/skip", handleLearnSkip},
    {"/api/learn/cancel", handleLearnCancel},
    {"/api/schedules", handleSchedulesSet},
    {"/api/time", handleTimeSet},
    {"/api/scenes", handleScenesSet},
    {"/api/scenes/apply", handleSceneApply},
    {"/api/peers", handlePeersSet},
    {"/api/peers/discover", handlePeersDiscover},
    {"/api/peers/command", handlePeerCommand},
    {"/api/automations", handleAutomationsSet},
    {"/api/automations/run", handleAutomationRun},
    {"/api/remotes/delete", handleRemoteDelete},
    {"/api/remotes/clear", handleRemotesClear},
    {"/api/stats/reset", handleStatsReset},
    {"/api/lang/remove", handleLanguageRemove},
    {"/api/webhook/test", handleWebhookTest},
};

// One handler per method, looking the path up rather than having the server
// hold an object for each.
void dispatchGet(AsyncWebServerRequest *request) {
  const String &url = request->url();
  for (const GetRoute &route : kGetRoutes) {
    if (url == route.path) {
      route.handler(request);
      return;
    }
  }
  sendError(request, 404, F("no such endpoint"));
}

void dispatchPost(AsyncWebServerRequest *request, JsonVariantConst body) {
  const String &url = request->url();
  for (const JsonRoute &route : kPostRoutes) {
    if (url == route.path) {
      route.handler(request, body);
      return;
    }
  }
  sendError(request, 404, F("no such endpoint"));
}

// ---------------------------------------------------------------------------

void registerApiRoutes(AsyncWebServer &server) {
  // Registered before the catch-all: the server takes the first handler that
  // says it can cope, and this one streams its body to the filesystem rather
  // than having it parsed.
  // Order matters twice over. The server takes the first handler that says it
  // can cope, and a registered path matches its own prefix as well as itself:
  // "/api/lang" would also claim "/api/lang/remove". So the routes that stream
  // their own bodies are registered first and named so they cannot shadow
  // anything, and the catch-alls go last.
  server.on("/api/lang/install", HTTP_POST, handleLanguageInstall, nullptr,
            handleLanguageBody);
  ota.registerRoutes(server);          // /api/ota/upload, which takes a file
  registerTasmotaRoutes(server);       // /cm, outside /api entirely

  server.on("/api/*", HTTP_GET, dispatchGet);
  onJson(server, "/api/*", HTTP_POST, dispatchPost);

  LOG_I(kTag, "%u API routes",
        (unsigned)(sizeof(kGetRoutes) / sizeof(kGetRoutes[0]) +
                   sizeof(kPostRoutes) / sizeof(kPostRoutes[0])));
}

}  // namespace net
