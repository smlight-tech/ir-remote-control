#include "MqttService.h"

#include <ESP8266WiFi.h>

#include "../app/Scenes.h"
#include "../app/Stats.h"
#include "../core/CommandBus.h"
#include "../core/Log.h"
#include "../core/Settings.h"
#include "generated/version.h"

namespace net {
namespace {
const char *kTag = "mqtt";

// Home Assistant discovery payloads run to about 1.2 kB.
const uint16_t kBufferSize = 1536;
const uint32_t kMaxRetryDelayMs = 60000;
// Republish periodically even when nothing changed, so a broker restart or a
// late-joining subscriber does not sit there with no state.
const uint32_t kHeartbeatMs = 60000;

const char *const kModes[] = {"off", "auto", "cool", "heat", "dry", "fan_only"};
const char *const kFans[] = {"auto", "min",  "low", "medium",
                             "medium_high", "high", "max"};
const char *const kSwings[] = {"off", "auto", "highest", "high",
                               "upper_middle", "middle", "low", "lowest"};
}  // namespace

MqttService mqtt;

// ---------------------------------------------------------------------------

void MqttService::begin() {
  client_.setBufferSize(kBufferSize);
  client_.setCallback([this](char *topic, uint8_t *payload, unsigned int length) {
    this->onMessage(topic, payload, length);
  });
  reconfigure();
}

void MqttService::reconfigure() {
  const cfg::MqttSettings &m = cfg::settings.mqtt;

  if (client_.connected()) {
    publishAvailability(false);
    client_.disconnect();
  }

  enabled_ = m.enabled && !m.host.isEmpty();
  discoveryPublished_ = false;
  lastAttemptAt_ = 0;
  retryDelayMs_ = 5000;

  if (!enabled_) {
    LOG_I(kTag, "disabled");
    return;
  }

  client_.setServer(m.host.c_str(), m.port);
  client_.setKeepAlive(m.keepalive);
  LOG_I(kTag, "broker %s:%u, base topic '%s'", m.host.c_str(), m.port,
        m.baseTopic.c_str());
}

bool MqttService::connected() const {
  return const_cast<PubSubClient &>(client_).connected();
}

String MqttService::topic(const char *leaf) const {
  String result = cfg::settings.mqtt.baseTopic;
  if (!result.endsWith("/")) result += '/';
  result += leaf;
  return result;
}

String MqttService::uniqueId() const {
  return String("slwf12_") + cfg::settings.chipId();
}

// homie/<device-id>/<leaf>
String MqttService::homieTopic(const char *leaf) const {
  String result = cfg::settings.mqtt.homieBaseTopic;
  if (result.isEmpty()) result = "homie";
  if (!result.endsWith("/")) result += '/';
  result += "slwf12-";
  result += cfg::settings.chipId();
  if (leaf != nullptr && leaf[0] != '\0') {
    result += '/';
    result += leaf;
  }
  return result;
}

// ---------------------------------------------------------------------------

void MqttService::loop() {
  if (!enabled_) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (!client_.connected()) {
    if (millis() - lastAttemptAt_ < retryDelayMs_) return;
    lastAttemptAt_ = millis();
    connect();
    return;
  }

  client_.loop();

  const bool changed = bus::commands.revision() != lastRevision_;
  const bool stale = millis() - lastPublishAt_ > kHeartbeatMs;
  if (changed || stale) publishState();
}

void MqttService::connect() {
  const cfg::MqttSettings &m = cfg::settings.mqtt;
  const String clientId = uniqueId();
  const String availability = topic("availability");

  LOG_I(kTag, "connecting to %s:%u", m.host.c_str(), m.port);

  const bool ok =
      m.user.isEmpty()
          ? client_.connect(clientId.c_str(), availability.c_str(), 0,
                            m.retain, "offline")
          : client_.connect(clientId.c_str(), m.user.c_str(), m.pass.c_str(),
                            availability.c_str(), 0, m.retain, "offline");

  if (!ok) {
    lastError_ = String(F("connection failed, state ")) + client_.state();
    LOG_W(kTag, "%s; retrying in %lus", lastError_.c_str(),
          (unsigned long)(retryDelayMs_ / 1000));
    retryDelayMs_ = min(retryDelayMs_ * 2, kMaxRetryDelayMs);
    return;
  }

  connectCount_++;
  retryDelayMs_ = 5000;
  lastError_ = "";
  LOG_I(kTag, "connected");

  subscribeAll();
  publishAvailability(true);
  if (cfg::settings.mqtt.discovery) publishDiscovery();
  if (cfg::settings.mqtt.homie) publishHomieDescription();
  publishState(/*force=*/true);
}

void MqttService::subscribeAll() {
  const char *leaves[] = {"set",      "power/set", "mode/set", "temperature/set",
                          "fan/set",  "swing/set", "resend",   "scene/set"};
  for (const char *leaf : leaves) {
    const String full = topic(leaf);
    client_.subscribe(full.c_str());
  }

  if (cfg::settings.mqtt.homie) {
    const String wildcard = homieTopic("ac/+/set");
    client_.subscribe(wildcard.c_str());
  }
  LOG_D(kTag, "subscribed to %s#", cfg::settings.mqtt.baseTopic.c_str());
}

// ---------------------------------------------------------------------------

void MqttService::onMessage(char *rawTopic, const uint8_t *payload,
                            unsigned int length) {
  String full(rawTopic);

  String body;
  body.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) body += static_cast<char>(payload[i]);

  if (handleHomieCommand(full, body)) return;

  String base = cfg::settings.mqtt.baseTopic;
  if (!base.endsWith("/")) base += '/';
  if (!full.startsWith(base)) return;

  const String leaf = full.substring(base.length());
  LOG_D(kTag, "%s <- %s", leaf.c_str(), body.c_str());
  handleCommand(leaf, body);
}

// Homie puts commands on <node>/<property>/set. The property names are the
// same vocabulary the rest of the firmware uses, so the existing parser does
// the work once the topic has been unwrapped.
bool MqttService::handleHomieCommand(const String &fullTopic,
                                     const String &payload) {
  if (!cfg::settings.mqtt.homie) return false;

  const String prefix = homieTopic("ac/");
  if (!fullTopic.startsWith(prefix) || !fullTopic.endsWith("/set")) return false;

  const String property =
      fullTopic.substring(prefix.length(), fullTopic.length() - 4);

  JsonDocument doc;
  JsonObject object = doc.to<JsonObject>();
  if (property == "power") {
    object["power"] = payload;
  } else if (property == "mode") {
    object["hvac_mode"] = payload;
  } else if (property == "temperature") {
    object["temp"] = payload.toFloat();
  } else if (property == "fan") {
    object["fan"] = payload;
  } else {
    return true;   // ours, but not a property we expose
  }

  ac::Delta delta;
  String error;
  if (!ac::deltaFromJson(object, delta, error)) {
    LOG_W(kTag, "homie %s rejected: %s", property.c_str(), error.c_str());
    return true;
  }

  const bus::Outcome outcome = bus::commands.apply(delta, src::Source::Mqtt);
  if (!outcome.ok()) LOG_W(kTag, "homie: %s", outcome.message.c_str());
  publishState(/*force=*/true);
  return true;
}

void MqttService::handleCommand(const String &leaf, const String &payload) {
  if (leaf == "resend") {
    bus::commands.resend(src::Source::Mqtt);
    return;
  }

  if (leaf == "scene/set") {
    String error;
    if (!app::scenes.apply(payload, src::Source::Mqtt, error)) {
      LOG_W(kTag, "scene '%s' not applied: %s", payload.c_str(), error.c_str());
    }
    publishState(/*force=*/true);
    return;
  }

  ac::Delta delta;
  String error;

  if (leaf == "set") {
    JsonDocument doc;
    if (deserializeJson(doc, payload) != DeserializationError::Ok) {
      LOG_W(kTag, "ignoring malformed JSON on .../set");
      return;
    }
    if (!ac::deltaFromJson(doc.as<JsonObjectConst>(), delta, error)) {
      LOG_W(kTag, "rejected command: %s", error.c_str());
      return;
    }
  } else {
    // Home Assistant's individual command topics carry a bare value; wrap it
    // so the same parser handles every path.
    JsonDocument doc;
    JsonObject object = doc.to<JsonObject>();
    if (leaf == "power/set") {
      object["power"] = payload;
    } else if (leaf == "mode/set") {
      object["hvac_mode"] = payload;
    } else if (leaf == "temperature/set") {
      object["temp"] = payload.toFloat();
    } else if (leaf == "fan/set") {
      object["fan"] = payload;
    } else if (leaf == "swing/set") {
      object["swingv"] = payload;
    } else {
      return;
    }
    if (!ac::deltaFromJson(object, delta, error)) {
      LOG_W(kTag, "rejected %s: %s", leaf.c_str(), error.c_str());
      return;
    }
  }

  const bus::Outcome outcome = bus::commands.apply(delta, src::Source::Mqtt);
  if (!outcome.ok()) {
    LOG_W(kTag, "command not applied: %s", outcome.message.c_str());
    // Republish so Home Assistant's optimistic UI snaps back to the truth.
    publishState(/*force=*/true);
  }
}

// ---------------------------------------------------------------------------

void MqttService::publishAvailability(bool online) {
  if (!client_.connected()) return;
  const String full = topic("availability");
  client_.publish(full.c_str(), online ? "online" : "offline",
                  cfg::settings.mqtt.retain);
}

void MqttService::publishState(bool force) {
  if (!client_.connected()) return;
  if (!force && bus::commands.revision() == lastRevision_ &&
      millis() - lastPublishAt_ < kHeartbeatMs) {
    return;
  }

  lastRevision_ = bus::commands.revision();
  lastPublishAt_ = millis();

  const ac::State &state = bus::commands.state();
  const bool retain = cfg::settings.mqtt.retain;

  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  ac::toJson(state, root);
  root["source"] = src::name(bus::commands.lastSource());
  root["rssi"] = WiFi.RSSI();
  root["uptime"] = millis() / 1000UL;
  root["runtime_hours"] =
      roundf(app::stats.runtimeSeconds() / 360.0f) / 10.0f;
  root["runtime_today_hours"] =
      roundf(app::stats.runtimeTodaySeconds() / 360.0f) / 10.0f;
  root["starts"] = app::stats.starts();
  const float kwh = app::stats.energyKwh();
  if (kwh >= 0.0f) root["energy_kwh"] = roundf(kwh * 100.0f) / 100.0f;

  const app::Scene *scene = app::scenes.matching(state);
  root["scene"] = scene != nullptr ? scene->name : String();

  String payload;
  serializeJson(doc, payload);

  const String stateTopic = topic("state");
  client_.publish(stateTopic.c_str(), payload.c_str(), retain);

  // Plain-text mirrors for subscribers that do not do templating.
  struct Simple {
    const char *leaf;
    String value;
  };
  const Simple simples[] = {
      {"mode/state", ac::hvacMode(state)},
      {"power/state", state.power ? "ON" : "OFF"},
      {"temperature/state", String(state.degrees, 1)},
      {"fan/state", ac::fanName(state.fanspeed)},
      {"swing/state", ac::swingVName(state.swingv)},
  };
  for (const Simple &simple : simples) {
    const String full = topic(simple.leaf);
    client_.publish(full.c_str(), simple.value.c_str(), retain);
  }

  if (cfg::settings.mqtt.homie) publishHomieState(state);
}

// ---------------------------------------------------------------------------
// Homie 4.0
// ---------------------------------------------------------------------------

void MqttService::publishHomieDescription() {
  struct Entry {
    const char *leaf;
    String value;
  };

  String modes, fans;
  {
    const char *const kAllModes[] = {"auto", "cool", "heat", "dry", "fan_only"};
    for (const char *mode : kAllModes) {
      if (!modes.isEmpty()) modes += ',';
      modes += mode;
    }
    for (const char *fan : kFans) {
      if (!fans.isEmpty()) fans += ',';
      fans += fan;
    }
  }

  const cfg::AcSettings &acCfg = cfg::settings.ac;
  const String range = String(acCfg.minTemp, 0) + ":" + String(acCfg.maxTemp, 0);

  const Entry entries[] = {
      {"$homie", "4.0.0"},
      {"$name", cfg::settings.device.name},
      {"$nodes", "ac"},
      {"$extensions", ""},
      {"ac/$name", "Air conditioner"},
      {"ac/$type", "climate"},
      {"ac/$properties", "power,mode,temperature,fan"},

      {"ac/power/$name", "Power"},
      {"ac/power/$datatype", "boolean"},
      {"ac/power/$settable", "true"},

      {"ac/mode/$name", "Mode"},
      {"ac/mode/$datatype", "enum"},
      {"ac/mode/$format", modes},
      {"ac/mode/$settable", "true"},

      {"ac/temperature/$name", "Target temperature"},
      {"ac/temperature/$datatype", "float"},
      {"ac/temperature/$format", range},
      {"ac/temperature/$unit", cfg::settings.device.celsius ? "°C" : "°F"},
      {"ac/temperature/$settable", "true"},

      {"ac/fan/$name", "Fan speed"},
      {"ac/fan/$datatype", "enum"},
      {"ac/fan/$format", fans},
      {"ac/fan/$settable", "true"},
  };

  for (const Entry &entry : entries) {
    const String full = homieTopic(entry.leaf);
    client_.publish(full.c_str(), entry.value.c_str(), true);
  }

  // Homie expects $state to become "lost" if the device drops off, but MQTT
  // allows one will message per connection and that is already spent on the
  // Home Assistant availability topic. $state is therefore accurate while the
  // device is running and stale if it dies; the availability topic is the one
  // to trust for liveness.
  const String stateTopic = homieTopic("$state");
  client_.publish(stateTopic.c_str(), "ready", true);

  LOG_I(kTag, "Homie description published under '%s'", homieTopic("").c_str());
}

void MqttService::publishHomieState(const ac::State &state) {
  struct Value {
    const char *leaf;
    String value;
  };
  const Value values[] = {
      {"ac/power", state.power ? "true" : "false"},
      {"ac/mode", ac::modeName(state.mode)},
      {"ac/temperature", String(state.degrees, 1)},
      {"ac/fan", ac::fanName(state.fanspeed)},
  };
  for (const Value &value : values) {
    const String full = homieTopic(value.leaf);
    client_.publish(full.c_str(), value.value.c_str(), true);
  }
}

// ---------------------------------------------------------------------------

void MqttService::addDeviceBlock(JsonObject parent) const {
  JsonObject device = parent["device"].to<JsonObject>();
  JsonArray ids = device["identifiers"].to<JsonArray>();
  ids.add(uniqueId());
  device["name"] = cfg::settings.device.name;
  device["manufacturer"] = "SMLIGHT";
  device["model"] = cfg::settings.ac.brand.isEmpty()
                        ? String("SLWF-12")
                        : String("SLWF-12 / ") + cfg::settings.ac.brand + " " +
                              cfg::settings.ac.modelName;
  device["sw_version"] = FW_VERSION;
  device["configuration_url"] = String("http://") + WiFi.localIP().toString();
}

void MqttService::publishDiscoveryFor(const char *component,
                                      const char *objectId, JsonDocument &doc) {
  String full = cfg::settings.mqtt.discoveryPrefix;
  full += '/';
  full += component;
  full += '/';
  full += uniqueId();
  full += '/';
  full += objectId;
  full += "/config";

  String payload;
  serializeJson(doc, payload);

  if (!client_.publish(full.c_str(), payload.c_str(), true)) {
    LOG_W(kTag, "discovery for %s did not fit in the MQTT buffer (%u bytes)",
          objectId, payload.length());
    return;
  }
  LOG_D(kTag, "discovery published: %s", full.c_str());
}

void MqttService::publishDiscovery() {
  const cfg::AcSettings &acCfg = cfg::settings.ac;
  const String stateTopic = topic("state");
  const String availability = topic("availability");

  {
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    root["name"] = nullptr;                 // use the device name
    root["unique_id"] = uniqueId() + "_climate";
    root["availability_topic"] = availability;
    root["temperature_unit"] = cfg::settings.device.celsius ? "C" : "F";
    root["min_temp"] = acCfg.minTemp;
    root["max_temp"] = acCfg.maxTemp;
    root["temp_step"] = acCfg.tempStep;
    root["initial"] = 24;
    root["optimistic"] = false;

    root["mode_command_topic"] = topic("mode/set");
    root["mode_state_topic"] = stateTopic;
    root["mode_state_template"] = "{{ value_json.hvac_mode }}";
    JsonArray modes = root["modes"].to<JsonArray>();
    for (const char *mode : kModes) modes.add(mode);

    root["temperature_command_topic"] = topic("temperature/set");
    root["temperature_state_topic"] = stateTopic;
    root["temperature_state_template"] = "{{ value_json.temp }}";

    root["fan_mode_command_topic"] = topic("fan/set");
    root["fan_mode_state_topic"] = stateTopic;
    root["fan_mode_state_template"] = "{{ value_json.fan }}";
    JsonArray fans = root["fan_modes"].to<JsonArray>();
    for (const char *fan : kFans) fans.add(fan);

    root["swing_mode_command_topic"] = topic("swing/set");
    root["swing_mode_state_topic"] = stateTopic;
    root["swing_mode_state_template"] = "{{ value_json.swingv }}";
    JsonArray swings = root["swing_modes"].to<JsonArray>();
    for (const char *swing : kSwings) swings.add(swing);

    root["power_command_topic"] = topic("power/set");
    root["payload_on"] = "ON";
    root["payload_off"] = "OFF";

    addDeviceBlock(root);
    publishDiscoveryFor("climate", "climate", doc);
  }

  {
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    root["name"] = "Signal strength";
    root["unique_id"] = uniqueId() + "_rssi";
    root["availability_topic"] = availability;
    root["state_topic"] = stateTopic;
    root["value_template"] = "{{ value_json.rssi }}";
    root["unit_of_measurement"] = "dBm";
    root["device_class"] = "signal_strength";
    root["entity_category"] = "diagnostic";
    addDeviceBlock(root);
    publishDiscoveryFor("sensor", "rssi", doc);
  }

  // Scenes as a dropdown. Home Assistant gets a `select` it can put on a
  // dashboard or drive from an automation, and the option list is whatever the
  // user actually configured.
  if (app::scenes.count() > 0) {
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    root["name"] = "Scene";
    root["unique_id"] = uniqueId() + "_scene";
    root["availability_topic"] = availability;
    root["command_topic"] = topic("scene/set");
    root["state_topic"] = stateTopic;
    root["value_template"] = "{{ value_json.scene }}";
    root["icon"] = "mdi:palette";
    JsonArray options = root["options"].to<JsonArray>();
    for (const app::Scene &scene : app::scenes.all()) options.add(scene.name);
    addDeviceBlock(root);
    publishDiscoveryFor("select", "scene", doc);
  }

  {
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    root["name"] = "Runtime";
    root["unique_id"] = uniqueId() + "_runtime";
    root["availability_topic"] = availability;
    root["state_topic"] = stateTopic;
    root["value_template"] = "{{ value_json.runtime_hours }}";
    root["unit_of_measurement"] = "h";
    root["device_class"] = "duration";
    root["state_class"] = "total_increasing";
    root["icon"] = "mdi:timer-outline";
    addDeviceBlock(root);
    publishDiscoveryFor("sensor", "runtime", doc);
  }

  // Only offered once the nameplate wattage is known — an energy sensor fed by
  // a guess would quietly pollute the Home Assistant energy dashboard.
  if (cfg::settings.ac.ratedWatts > 0) {
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    root["name"] = "Energy (estimated)";
    root["unique_id"] = uniqueId() + "_energy";
    root["availability_topic"] = availability;
    root["state_topic"] = stateTopic;
    root["value_template"] = "{{ value_json.energy_kwh }}";
    root["unit_of_measurement"] = "kWh";
    root["device_class"] = "energy";
    root["state_class"] = "total_increasing";
    addDeviceBlock(root);
    publishDiscoveryFor("sensor", "energy", doc);
  }

  {
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    root["name"] = "Resend state";
    root["unique_id"] = uniqueId() + "_resend";
    root["availability_topic"] = availability;
    root["command_topic"] = topic("resend");
    root["payload_press"] = "PRESS";
    root["entity_category"] = "config";
    root["icon"] = "mdi:remote";
    addDeviceBlock(root);
    publishDiscoveryFor("button", "resend", doc);
  }

  discoveryPublished_ = true;
  LOG_I(kTag, "Home Assistant discovery published under '%s'",
        cfg::settings.mqtt.discoveryPrefix.c_str());
}

// ---------------------------------------------------------------------------

void MqttService::statusJson(JsonObject out) const {
  out["enabled"] = enabled_;
  out["connected"] = connected();
  out["broker"] = cfg::settings.mqtt.host;
  out["baseTopic"] = cfg::settings.mqtt.baseTopic;
  out["discovery"] = discoveryPublished_;
  out["connects"] = connectCount_;
  if (!lastError_.isEmpty()) out["lastError"] = lastError_;
}

}  // namespace net
