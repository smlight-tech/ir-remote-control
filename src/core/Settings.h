// Persistent configuration, stored as /config.json on LittleFS.
//
// Saves are debounced through loop() so that a burst of UI edits or a schedule
// import costs one flash write rather than twenty.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <IPAddress.h>

#include "Source.h"

namespace cfg {

// Sentinel the web UI sends back in place of a secret it did not change.
extern const char *const kRedacted;

// Format of /config.json. Reported in the status document so a backup taken
// from one firmware can be told apart from one this build would read
// differently. Bump it whenever a stored field changes meaning.
static const uint16_t kSchemaVersion = 1;

static const uint8_t kMaxTelegramUsers = 8;

struct DeviceSettings {
  String name = "SLWF-12";
  String hostname;          // filled from the chip id on first boot
  String language = "en";
  // How this device presents itself. The unit is the one that matters
  // beyond the interface: the protocol carries it, so the air conditioner's
  // own display follows it. The rest are how dates and times are written,
  // which every client reads and none of them should guess.
  bool celsius = true;
  bool hour12 = false;              // 3:05 pm rather than 15:05
  String dateFormat = "iso";        // "iso" | "dmy" | "mdy"
  uint8_t weekStart = 1;            // 0 = Sunday, 1 = Monday
};

struct WifiSettings {
  String ssid;
  String pass;
  bool useStatic = false;
  IPAddress ip{0, 0, 0, 0};
  IPAddress gateway{0, 0, 0, 0};
  IPAddress mask{255, 255, 255, 0};
  IPAddress dns{0, 0, 0, 0};
  String apPassword;        // empty = open setup AP
};

struct AuthSettings {
  bool enabled = false;
  String user = "admin";
  String pass;
  String token;             // bearer token for headless API clients
};

struct PinSettings {
  int8_t irRx = PIN_IR_RX;
  int8_t irTx = PIN_IR_TX;
  int8_t button = PIN_BUTTON;
  bool irTxInverted = false;
  uint8_t irCarrierKhz = 38;
};

struct MqttSettings {
  bool enabled = false;
  String host;
  uint16_t port = 1883;
  String user;
  String pass;
  String baseTopic = "slwf12";
  bool retain = true;
  bool discovery = true;
  String discoveryPrefix = "homeassistant";
  uint16_t keepalive = 30;

  // The Homie 4.0 convention, published alongside Home Assistant discovery.
  // openHAB, Domoticz and a few others find devices this way; Home Assistant
  // users have no reason to switch it on.
  bool homie = false;
  String homieBaseTopic = "homie";
};

// Outbound HTTP on every state change, so anything that can receive a request
// can follow the air conditioner without speaking MQTT — Node-RED, a logging
// endpoint, a relay into some other system.
struct WebhookSettings {
  bool enabled = false;
  String url;                 // http:// only; see the note in WebhookService
  String headerName;          // optional, e.g. "Authorization"
  String headerValue;
  uint16_t minIntervalSeconds = 1;   // coalesce bursts
};

struct TelegramSettings {
  bool enabled = false;
  String token;
  int64_t allowed[kMaxTelegramUsers] = {0};
  uint8_t allowedCount = 0;
  uint16_t pollSeconds = 3;
  bool notifyOnChange = true;
  // TLS receive buffer. 0 = decide automatically: 512 bytes when the server
  // agrees to fragment, otherwise the 16 kB a TLS 1.2 record can legally
  // reach. Lowering it by hand trades robustness for heap — see the memory
  // notes in the README before doing so.
  uint16_t tlsBufferBytes = 0;
  // Any user may claim the bot until the first user is authorised; after that
  // new chat ids are rejected. Prevents a leaked token from being useful.
  bool openEnrolment = true;

  bool isAllowed(int64_t chatId) const;
  bool allow(int64_t chatId);
};

// Modbus TCP. A flat holding-register map, because that is all a PLC or a
// building-management head-end wants to see.
struct ModbusSettings {
  bool enabled = false;
  uint16_t port = 502;
};

struct UartSettings {
  bool enabled = false;
  uint32_t baud = 115200;
  bool emitEvents = true;   // push state changes as JSON lines
};

// What the AC actually supports. Populated by the learning wizard or by a
// profile pulled from the shared database, and used to shape every UI.
struct AcSettings {
  String protocol;              // IRremoteESP8266 name, e.g. "DAIKIN216"
  int16_t model = -1;
  String brand;
  String modelName;
  String profileId;             // id in the shared database, if imported
  float minTemp = 16.0f;
  float maxTemp = 30.0f;
  // Half a degree: what most air conditioner remotes offer, and every
  // protocol IRremoteESP8266 sends can carry it. A unit that only accepts
  // whole degrees rounds it off itself.
  float tempStep = 0.5f;
  uint8_t sendRepeats = 0;
  bool useLearnedCodes = false; // true when the protocol is not decodable
  bool restoreOnBoot = true;
  bool trackRemote = true;      // adopt state seen from the physical remote

  // Compressor protection. Restarting a scroll or reciprocating compressor
  // against residual head pressure is how they die young, which is why every
  // manufacturer specifies a minimum off period. Most units enforce this
  // themselves, but not all do, and a schedule or a bad automation can
  // otherwise power-cycle one every few seconds. 0 disables the guard.
  uint16_t minOffSeconds = 180;

  // Nameplate power draw, used for the energy estimate. 0 means unknown, and
  // the estimate is then not published at all rather than published as a lie.
  uint16_t ratedWatts = 0;
};

struct ScheduleSettings {
  bool enabled = true;
};

struct OtaSettings {
  String channel = "stable";
  String manifestUrl;           // defaults to the project's GitHub releases
};

// Endpoints that exist for somebody else's software to talk to. They cost
// nothing while off — a handler that answers 404 — but each one is a way in,
// and a way in nobody asked for is not worth having.
struct CompatSettings {
  bool tasmota = false;         // /cm?cmnd=…
  bool metrics = false;         // /api/metrics, Prometheus
};

struct TimeSettings {
  // Empty on purpose. A time server is an outbound connection to somebody
  // else's machine, and the browser looking at this page already knows what
  // time it is — it sets the clock the moment it finds one that does not.
  // Name a server here and the device will use it instead.
  String ntpServer;
  String timezone = "UTC0";     // POSIX TZ string

  // Where the device is, for sunrise and sunset. Zero means "not told", which
  // is treated as "no sun times" rather than as the Gulf of Guinea.
  float latitude = 0.0f;
  float longitude = 0.0f;
  bool located() const { return latitude != 0.0f || longitude != 0.0f; }
};

struct LogSettings {
  uint8_t level = 2;            // log_::Level
  bool serial = true;
};

struct CloudSettings {
  // Off until asked. The shared database is fetched by the browser, not the
  // device — but "the interface quietly went to GitHub" is the same surprise
  // either way, and language packs are already held to this rule.
  bool enabled = false;
  String dbUrl;                 // defaults to the project's GitHub Pages copy
};

class Settings {
 public:
  DeviceSettings device;
  WifiSettings wifi;
  AuthSettings auth;
  PinSettings pins;
  MqttSettings mqtt;
  WebhookSettings webhook;
  ModbusSettings modbus;
  TelegramSettings telegram;
  UartSettings uart;
  AcSettings ac;
  ScheduleSettings schedule;
  OtaSettings ota;
  CompatSettings compat;
  TimeSettings time;
  LogSettings log;
  CloudSettings cloud;

  bool sourceEnabled[src::kCount];

  // Mounts LittleFS (formatting it if it has never been used) and loads
  // /config.json. Returns false when defaults had to be used.
  bool begin();

  bool load();
  bool save();
  void factoryReset();

  // Queues a save for the next loop(); coalesces rapid changes.
  void touch();
  void loop();
  bool dirty() const { return dirty_; }

  void toJson(JsonObject out, bool includeSecrets) const;
  bool fromJson(JsonObjectConst in, String &error);

  bool isSourceEnabled(src::Source s) const;

  // Stable per-device id derived from the MAC, e.g. "a1b2c3".
  const String &chipId() const { return chipId_; }

 private:
  void applyDefaults();

  String chipId_;
  bool dirty_ = false;
  uint32_t dirtyAt_ = 0;
};

extern Settings settings;

}  // namespace cfg
