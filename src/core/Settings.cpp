#include "Settings.h"

#include <LittleFS.h>

#include "Log.h"

namespace cfg {

const char *const kRedacted = "••••••••";

namespace {
const char *kTag = "cfg";
const char *kPath = "/config.json";
const char *kTempPath = "/config.tmp";
const uint32_t kSaveDebounceMs = 1500;

// Reads a string, treating the redaction sentinel as "leave unchanged".
void readSecret(JsonVariantConst v, String &out) {
  if (v.isNull()) return;
  const char *s = v.as<const char *>();
  if (s == nullptr) return;
  if (strcmp(s, kRedacted) == 0) return;
  out = s;
}

void readString(JsonVariantConst v, String &out) {
  if (v.isNull()) return;
  const char *s = v.as<const char *>();
  if (s != nullptr) out = s;
}

template <typename T>
void readNumber(JsonVariantConst v, T &out, T lo, T hi) {
  if (v.isNull() || !v.is<float>()) return;
  const float value = v.as<float>();
  if (value < static_cast<float>(lo) || value > static_cast<float>(hi)) return;
  out = static_cast<T>(value);
}

void readBool(JsonVariantConst v, bool &out) {
  if (!v.isNull() && v.is<bool>()) out = v.as<bool>();
}

void readIp(JsonVariantConst v, IPAddress &out) {
  const char *s = v.as<const char *>();
  if (s == nullptr) return;
  IPAddress parsed;
  if (parsed.fromString(s)) out = parsed;
}

String ipToString(const IPAddress &ip) {
  return ip == IPAddress(0, 0, 0, 0) ? String() : ip.toString();
}

const char *secretOut(const String &value, bool includeSecrets) {
  if (includeSecrets) return value.c_str();
  return value.length() ? kRedacted : "";
}

}  // namespace

Settings settings;

// ---------------------------------------------------------------------------

bool TelegramSettings::isAllowed(int64_t chatId) const {
  for (uint8_t i = 0; i < allowedCount; i++)
    if (allowed[i] == chatId) return true;
  return false;
}

bool TelegramSettings::allow(int64_t chatId) {
  if (isAllowed(chatId)) return true;
  if (allowedCount >= kMaxTelegramUsers) return false;
  allowed[allowedCount++] = chatId;
  return true;
}

// ---------------------------------------------------------------------------

void Settings::applyDefaults() {
  device = DeviceSettings();
  wifi = WifiSettings();
  auth = AuthSettings();
  pins = PinSettings();
  mqtt = MqttSettings();
  webhook = WebhookSettings();
  modbus = ModbusSettings();
  telegram = TelegramSettings();
  uart = UartSettings();
  ac = AcSettings();
  schedule = ScheduleSettings();
  ota = OtaSettings();
  time = TimeSettings();
  log = LogSettings();
  cloud = CloudSettings();

  // Off unless it is this device doing its own job, or something already
  // knocking on the door it opens anyway.
  //
  // The API is on: it is not a separate listener but the same web server the
  // interface uses, behind the same access control, and configuration was
  // never gated by source in the first place — so switching it off bought no
  // protection, only a confusing half-working API. What stays off is every
  // *additional* protocol or endpoint: MQTT, Telegram, Modbus, UART, and the
  // compatibility endpoints. Those are surfaces the device would not
  // otherwise have.
  for (uint8_t i = 0; i < src::kCount; i++) sourceEnabled[i] = false;
  for (src::Source allowed : {src::Source::Web, src::Source::Api,
                              src::Source::IrRemote, src::Source::Remote,
                              src::Source::Schedule, src::Source::Automation,
                              src::Source::System}) {
    sourceEnabled[static_cast<uint8_t>(allowed)] = true;
  }

  device.hostname = String("slwf12-") + chipId_;
  mqtt.baseTopic = String("slwf12/") + chipId_;
}

bool Settings::begin() {
  char id[13];
  snprintf(id, sizeof(id), "%06x", ESP.getChipId());
  chipId_ = id;

  if (!LittleFS.begin()) {
    LOG_W(kTag, "LittleFS mount failed, formatting");
    LittleFS.format();
    if (!LittleFS.begin()) {
      LOG_E(kTag, "LittleFS unusable — running on defaults, nothing will persist");
      applyDefaults();
      return false;
    }
  }

  applyDefaults();
  return load();
}

bool Settings::load() {
  File file = LittleFS.open(kPath, "r");
  if (!file) {
    LOG_I(kTag, "no %s yet, first boot", kPath);
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    LOG_E(kTag, "config parse error: %s — keeping defaults", err.c_str());
    return false;
  }

  String error;
  if (!fromJson(doc.as<JsonObjectConst>(), error)) {
    LOG_E(kTag, "config rejected: %s", error.c_str());
    return false;
  }

  LOG_I(kTag, "loaded %s", kPath);
  return true;
}

bool Settings::save() {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  toJson(root, /*includeSecrets=*/true);

  File file = LittleFS.open(kTempPath, "w");
  if (!file) {
    LOG_E(kTag, "cannot open %s for writing", kTempPath);
    return false;
  }
  const size_t written = serializeJson(doc, file);
  file.close();

  if (written == 0) {
    LOG_E(kTag, "serialisation produced nothing, keeping old config");
    LittleFS.remove(kTempPath);
    return false;
  }

  // Rename over the live file so a power cut can never leave a half-written
  // config behind.
  LittleFS.remove(kPath);
  if (!LittleFS.rename(kTempPath, kPath)) {
    LOG_E(kTag, "rename %s -> %s failed", kTempPath, kPath);
    return false;
  }

  dirty_ = false;
  LOG_I(kTag, "saved %s (%u bytes)", kPath, (unsigned)written);
  return true;
}

void Settings::factoryReset() {
  LittleFS.remove(kPath);
  LittleFS.remove("/profile.json");
  LittleFS.remove("/schedules.json");
  if (LittleFS.exists("/codes")) {
    Dir dir = LittleFS.openDir("/codes");
    while (dir.next()) LittleFS.remove(String("/codes/") + dir.fileName());
  }
  applyDefaults();
  dirty_ = false;
  LOG_W(kTag, "factory reset performed");
}

void Settings::touch() {
  dirty_ = true;
  dirtyAt_ = millis();
}

void Settings::loop() {
  if (!dirty_) return;
  if (millis() - dirtyAt_ < kSaveDebounceMs) return;
  save();
}

bool Settings::isSourceEnabled(src::Source s) const {
  if (!src::isGateable(s)) return true;
  const uint8_t i = static_cast<uint8_t>(s);
  return i < src::kCount && sourceEnabled[i];
}

// ---------------------------------------------------------------------------

void Settings::toJson(JsonObject out, bool includeSecrets) const {
  out["version"] = kSchemaVersion;
  out["chipId"] = chipId_;

  JsonObject d = out["device"].to<JsonObject>();
  d["name"] = device.name;
  d["hostname"] = device.hostname;
  d["language"] = device.language;
  d["celsius"] = device.celsius;
  d["hour12"] = device.hour12;
  d["dateFormat"] = device.dateFormat;
  d["weekStart"] = device.weekStart;

  JsonObject w = out["wifi"].to<JsonObject>();
  w["ssid"] = wifi.ssid;
  w["pass"] = secretOut(wifi.pass, includeSecrets);
  w["useStatic"] = wifi.useStatic;
  w["ip"] = ipToString(wifi.ip);
  w["gateway"] = ipToString(wifi.gateway);
  w["mask"] = ipToString(wifi.mask);
  w["dns"] = ipToString(wifi.dns);
  w["apPassword"] = secretOut(wifi.apPassword, includeSecrets);

  JsonObject a = out["auth"].to<JsonObject>();
  a["enabled"] = auth.enabled;
  a["user"] = auth.user;
  a["pass"] = secretOut(auth.pass, includeSecrets);
  a["token"] = secretOut(auth.token, includeSecrets);

  JsonObject p = out["pins"].to<JsonObject>();
  p["irRx"] = pins.irRx;
  p["irTx"] = pins.irTx;
  p["button"] = pins.button;
  p["irTxInverted"] = pins.irTxInverted;
  p["irCarrierKhz"] = pins.irCarrierKhz;

  JsonObject s = out["sources"].to<JsonObject>();
  for (uint8_t i = 0; i < src::kCount; i++) {
    const src::Source source = static_cast<src::Source>(i);
    if (!src::isGateable(source)) continue;
    s[src::name(source)] = sourceEnabled[i];
  }

  JsonObject m = out["mqtt"].to<JsonObject>();
  m["enabled"] = mqtt.enabled;
  m["host"] = mqtt.host;
  m["port"] = mqtt.port;
  m["user"] = mqtt.user;
  m["pass"] = secretOut(mqtt.pass, includeSecrets);
  m["baseTopic"] = mqtt.baseTopic;
  m["retain"] = mqtt.retain;
  m["discovery"] = mqtt.discovery;
  m["discoveryPrefix"] = mqtt.discoveryPrefix;
  m["keepalive"] = mqtt.keepalive;
  m["homie"] = mqtt.homie;
  m["homieBaseTopic"] = mqtt.homieBaseTopic;

  JsonObject wh = out["webhook"].to<JsonObject>();
  wh["enabled"] = webhook.enabled;
  wh["url"] = webhook.url;
  wh["headerName"] = webhook.headerName;
  wh["headerValue"] = secretOut(webhook.headerValue, includeSecrets);
  wh["minIntervalSeconds"] = webhook.minIntervalSeconds;

  JsonObject mb = out["modbus"].to<JsonObject>();
  mb["enabled"] = modbus.enabled;
  mb["port"] = modbus.port;

  JsonObject t = out["telegram"].to<JsonObject>();
  t["enabled"] = telegram.enabled;
  t["token"] = secretOut(telegram.token, includeSecrets);
  t["pollSeconds"] = telegram.pollSeconds;
  t["notifyOnChange"] = telegram.notifyOnChange;
  t["tlsBufferBytes"] = telegram.tlsBufferBytes;
  t["openEnrolment"] = telegram.openEnrolment;
  JsonArray ids = t["allowed"].to<JsonArray>();
  for (uint8_t i = 0; i < telegram.allowedCount; i++) ids.add(telegram.allowed[i]);

  JsonObject u = out["uart"].to<JsonObject>();
  u["enabled"] = uart.enabled;
  u["baud"] = uart.baud;
  u["emitEvents"] = uart.emitEvents;

  JsonObject acj = out["ac"].to<JsonObject>();
  acj["protocol"] = ac.protocol;
  acj["model"] = ac.model;
  acj["brand"] = ac.brand;
  acj["modelName"] = ac.modelName;
  acj["profileId"] = ac.profileId;
  acj["minTemp"] = ac.minTemp;
  acj["maxTemp"] = ac.maxTemp;
  acj["tempStep"] = ac.tempStep;
  acj["sendRepeats"] = ac.sendRepeats;
  acj["useLearnedCodes"] = ac.useLearnedCodes;
  acj["restoreOnBoot"] = ac.restoreOnBoot;
  acj["trackRemote"] = ac.trackRemote;
  acj["minOffSeconds"] = ac.minOffSeconds;
  acj["ratedWatts"] = ac.ratedWatts;

  out["schedule"]["enabled"] = schedule.enabled;

  JsonObject o = out["ota"].to<JsonObject>();
  o["channel"] = ota.channel;
  o["manifestUrl"] = ota.manifestUrl;

  JsonObject tm = out["time"].to<JsonObject>();
  tm["ntpServer"] = time.ntpServer;
  tm["timezone"] = time.timezone;
  tm["latitude"] = time.latitude;
  tm["longitude"] = time.longitude;

  JsonObject lg = out["log"].to<JsonObject>();
  lg["level"] = log.level;
  lg["serial"] = log.serial;

  JsonObject cl = out["cloud"].to<JsonObject>();
  JsonObject cp = out["compat"].to<JsonObject>();
  cp["tasmota"] = compat.tasmota;
  cp["metrics"] = compat.metrics;

  cl["enabled"] = cloud.enabled;
  cl["dbUrl"] = cloud.dbUrl;
}

bool Settings::fromJson(JsonObjectConst in, String &error) {
  if (in.isNull()) {
    error = F("expected an object");
    return false;
  }

  JsonObjectConst d = in["device"];
  if (!d.isNull()) {
    readString(d["name"], device.name);
    readString(d["hostname"], device.hostname);
    readString(d["language"], device.language);
    readBool(d["celsius"], device.celsius);
    readBool(d["hour12"], device.hour12);
    readString(d["dateFormat"], device.dateFormat);
    if (device.dateFormat != "dmy" && device.dateFormat != "mdy") {
      device.dateFormat = "iso";
    }
    readNumber<uint8_t>(d["weekStart"], device.weekStart, 0, 1);
    device.hostname.replace(' ', '-');
  }

  JsonObjectConst w = in["wifi"];
  if (!w.isNull()) {
    readString(w["ssid"], wifi.ssid);
    readSecret(w["pass"], wifi.pass);
    readBool(w["useStatic"], wifi.useStatic);
    readIp(w["ip"], wifi.ip);
    readIp(w["gateway"], wifi.gateway);
    readIp(w["mask"], wifi.mask);
    readIp(w["dns"], wifi.dns);
    readSecret(w["apPassword"], wifi.apPassword);
  }

  JsonObjectConst a = in["auth"];
  if (!a.isNull()) {
    readBool(a["enabled"], auth.enabled);
    readString(a["user"], auth.user);
    readSecret(a["pass"], auth.pass);
    readSecret(a["token"], auth.token);
    if (auth.enabled && auth.pass.isEmpty()) {
      error = F("cannot enable authentication without a password");
      return false;
    }
  }

  JsonObjectConst p = in["pins"];
  if (!p.isNull()) {
    readNumber<int8_t>(p["irRx"], pins.irRx, -1, 16);
    readNumber<int8_t>(p["irTx"], pins.irTx, -1, 16);
    readNumber<int8_t>(p["button"], pins.button, -1, 16);
    readBool(p["irTxInverted"], pins.irTxInverted);
    readNumber<uint8_t>(p["irCarrierKhz"], pins.irCarrierKhz, 30, 60);
    if (pins.irRx >= 0 && pins.irRx == pins.irTx) {
      error = F("IR receive and transmit pins must differ");
      return false;
    }
  }

  JsonObjectConst s = in["sources"];
  if (!s.isNull()) {
    for (uint8_t i = 0; i < src::kCount; i++) {
      const src::Source source = static_cast<src::Source>(i);
      if (!src::isGateable(source)) continue;
      readBool(s[src::name(source)], sourceEnabled[i]);
    }
  }

  JsonObjectConst m = in["mqtt"];
  if (!m.isNull()) {
    readBool(m["enabled"], mqtt.enabled);
    readString(m["host"], mqtt.host);
    readNumber<uint16_t>(m["port"], mqtt.port, 1, 65535);
    readString(m["user"], mqtt.user);
    readSecret(m["pass"], mqtt.pass);
    readString(m["baseTopic"], mqtt.baseTopic);
    readBool(m["retain"], mqtt.retain);
    readBool(m["discovery"], mqtt.discovery);
    readString(m["discoveryPrefix"], mqtt.discoveryPrefix);
    readNumber<uint16_t>(m["keepalive"], mqtt.keepalive, 5, 600);
    readBool(m["homie"], mqtt.homie);
    readString(m["homieBaseTopic"], mqtt.homieBaseTopic);
    if (mqtt.enabled && mqtt.host.isEmpty()) {
      error = F("MQTT is enabled but no broker host is set");
      return false;
    }
  }

  JsonObjectConst wh = in["webhook"];
  if (!wh.isNull()) {
    readBool(wh["enabled"], webhook.enabled);
    readString(wh["url"], webhook.url);
    readString(wh["headerName"], webhook.headerName);
    readSecret(wh["headerValue"], webhook.headerValue);
    readNumber<uint16_t>(wh["minIntervalSeconds"], webhook.minIntervalSeconds, 0,
                         3600);
    if (webhook.enabled) {
      if (webhook.url.isEmpty()) {
        error = F("the webhook is enabled but no URL is set");
        return false;
      }
      if (!webhook.url.startsWith("http://")) {
        error = F("webhook URLs must start with http:// — this device cannot "
                  "afford a second TLS session");
        return false;
      }
    }
  }

  JsonObjectConst mb = in["modbus"];
  if (!mb.isNull()) {
    readBool(mb["enabled"], modbus.enabled);
    readNumber<uint16_t>(mb["port"], modbus.port, 1, 65535);
  }

  JsonObjectConst t = in["telegram"];
  if (!t.isNull()) {
    readBool(t["enabled"], telegram.enabled);
    readSecret(t["token"], telegram.token);
    readNumber<uint16_t>(t["pollSeconds"], telegram.pollSeconds, 1, 300);
    readBool(t["notifyOnChange"], telegram.notifyOnChange);
    if (!t["tlsBufferBytes"].isNull()) {
      const uint32_t bytes = t["tlsBufferBytes"] | 0U;
      if (bytes != 0 && (bytes < 512 || bytes > 16384)) {
        error = F("the TLS buffer must be 0 (automatic) or 512..16384 bytes");
        return false;
      }
      telegram.tlsBufferBytes = static_cast<uint16_t>(bytes);
    }
    readBool(t["openEnrolment"], telegram.openEnrolment);
    JsonArrayConst ids = t["allowed"];
    if (!ids.isNull()) {
      telegram.allowedCount = 0;
      for (JsonVariantConst v : ids) {
        if (telegram.allowedCount >= kMaxTelegramUsers) break;
        telegram.allowed[telegram.allowedCount++] = v.as<int64_t>();
      }
    }
    if (telegram.enabled && telegram.token.isEmpty()) {
      error = F("Telegram is enabled but no bot token is set");
      return false;
    }
  }

  JsonObjectConst u = in["uart"];
  if (!u.isNull()) {
    readBool(u["enabled"], uart.enabled);
    readNumber<uint32_t>(u["baud"], uart.baud, 1200, 921600);
    readBool(u["emitEvents"], uart.emitEvents);
  }

  JsonObjectConst acj = in["ac"];
  if (!acj.isNull()) {
    readString(acj["protocol"], ac.protocol);
    readNumber<int16_t>(acj["model"], ac.model, -1, 32767);
    readString(acj["brand"], ac.brand);
    readString(acj["modelName"], ac.modelName);
    readString(acj["profileId"], ac.profileId);
    readNumber<float>(acj["minTemp"], ac.minTemp, -20.0f, 60.0f);
    readNumber<float>(acj["maxTemp"], ac.maxTemp, -20.0f, 60.0f);
    readNumber<float>(acj["tempStep"], ac.tempStep, 0.5f, 5.0f);
    readNumber<uint8_t>(acj["sendRepeats"], ac.sendRepeats, 0, 10);
    readBool(acj["useLearnedCodes"], ac.useLearnedCodes);
    readBool(acj["restoreOnBoot"], ac.restoreOnBoot);
    readBool(acj["trackRemote"], ac.trackRemote);
    readNumber<uint16_t>(acj["minOffSeconds"], ac.minOffSeconds, 0, 3600);
    readNumber<uint16_t>(acj["ratedWatts"], ac.ratedWatts, 0, 20000);
    if (ac.maxTemp <= ac.minTemp) {
      error = F("maximum temperature must be above the minimum");
      return false;
    }
  }

  JsonObjectConst sc = in["schedule"];
  if (!sc.isNull()) readBool(sc["enabled"], schedule.enabled);

  JsonObjectConst o = in["ota"];
  if (!o.isNull()) {
    readString(o["channel"], ota.channel);
    readString(o["manifestUrl"], ota.manifestUrl);
  }

  JsonObjectConst tm = in["time"];
  if (!tm.isNull()) {
    readString(tm["ntpServer"], time.ntpServer);
    readString(tm["timezone"], time.timezone);
    readNumber<float>(tm["latitude"], time.latitude, -90.0f, 90.0f);
    readNumber<float>(tm["longitude"], time.longitude, -180.0f, 180.0f);
  }

  JsonObjectConst lg = in["log"];
  if (!lg.isNull()) {
    readNumber<uint8_t>(lg["level"], log.level, 0, 3);
    readBool(lg["serial"], log.serial);
  }

  JsonObjectConst cp = in["compat"];
  if (!cp.isNull()) {
    readBool(cp["tasmota"], compat.tasmota);
    readBool(cp["metrics"], compat.metrics);
  }

  JsonObjectConst cl = in["cloud"];
  if (!cl.isNull()) {
    readBool(cl["enabled"], cloud.enabled);
    readString(cl["dbUrl"], cloud.dbUrl);
  }

  return true;
}

}  // namespace cfg
