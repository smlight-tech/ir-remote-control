// Canonical air-conditioner state.
//
// The internal representation is IRremoteESP8266's `stdAc::state_t` verbatim.
// That is deliberate: it is the type `IRac::sendAc()` consumes and the type
// `IRAcUtils::decodeToState()` produces, so state coming in from a physical
// remote and state going out to the AC share one representation with no lossy
// conversion in between.
//
// The *wire* vocabulary (JSON, MQTT, Telegram, UART) uses Home Assistant's
// climate strings — "cool", "fan_only", "medium_high" — so exposing the device
// to Home Assistant needs no translation table anywhere.
#pragma once

#include <ArduinoJson.h>
#include <IRac.h>
#include <IRremoteESP8266.h>

namespace ac {

using State = stdAc::state_t;

// A partial update. Every field is optional; only what the caller set is
// merged onto the current state. This is what every client adapter produces.
struct Delta {
  bool hasPower = false;      bool power = false;
  bool hasMode = false;       stdAc::opmode_t mode = stdAc::opmode_t::kAuto;
  bool hasDegrees = false;    float degrees = 24.0f;
  bool hasCelsius = false;    bool celsius = true;
  bool hasFan = false;        stdAc::fanspeed_t fan = stdAc::fanspeed_t::kAuto;
  bool hasSwingV = false;     stdAc::swingv_t swingv = stdAc::swingv_t::kOff;
  bool hasSwingH = false;     stdAc::swingh_t swingh = stdAc::swingh_t::kOff;
  bool hasQuiet = false;      bool quiet = false;
  bool hasTurbo = false;      bool turbo = false;
  bool hasEcono = false;      bool econo = false;
  bool hasLight = false;      bool light = false;
  bool hasFilter = false;     bool filter = false;
  bool hasClean = false;      bool clean = false;
  bool hasBeep = false;       bool beep = false;
  bool hasSleep = false;      int16_t sleep = -1;

  bool empty() const;
  // Merges onto `state`; returns true if anything actually changed.
  bool applyTo(State &state) const;
};

State defaultState();
bool equal(const State &a, const State &b);

// ---------------------------------------------------------------------------
// Wire vocabulary
// ---------------------------------------------------------------------------

// Home Assistant hvac_mode: "off" when powered down, otherwise the mode name.
const char *hvacMode(const State &state);

const char *modeName(stdAc::opmode_t mode);
const char *fanName(stdAc::fanspeed_t fan);
const char *swingVName(stdAc::swingv_t swing);
const char *swingHName(stdAc::swingh_t swing);

bool parseMode(const char *text, stdAc::opmode_t &out);
bool parseFan(const char *text, stdAc::fanspeed_t &out);
bool parseSwingV(const char *text, stdAc::swingv_t &out);
bool parseSwingH(const char *text, stdAc::swingh_t &out);

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

void toJson(const State &state, JsonObject out);

// Reads a partial JSON object into a Delta. Unknown keys are ignored; keys with
// unparseable values are reported through `error` and cause a false return.
bool deltaFromJson(JsonObjectConst in, Delta &out, String &error);

}  // namespace ac
