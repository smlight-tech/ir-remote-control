// Storage for raw IR captures, used when the AC's protocol is not one that
// IRremoteESP8266 can synthesise.
//
// Almost every air-conditioner remote transmits its *entire* state in every
// frame rather than a "temperature up" delta. A raw capture is therefore a
// snapshot of one complete state, not a button press — so codes are keyed by
// the state they produce:
//
//     off                       the unit switched off
//     cool_24_auto              cool mode, 24 degrees, fan auto
//     btn_turbo                 a free-standing extra button
//
// Lookup degrades gracefully: exact key, then any fan speed at that
// temperature, then the nearest temperature in the same mode. A user who only
// taught 18/22/26 still gets sensible behaviour across the whole dial.
//
// Codes are read and written one at a time. A full profile can run to tens of
// kilobytes of JSON, which an ESP8266 cannot hold in RAM — so assembling and
// splitting profiles is the browser's job, and this class only ever deals with
// a single code.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "../core/AcState.h"

namespace ir {

static const uint16_t kMaxRawLength = 1024;

struct RawCode {
  uint16_t *timings = nullptr;   // microseconds, mark/space alternating
  uint16_t length = 0;
  uint16_t carrierKhz = 38;

  bool valid() const { return timings != nullptr && length > 1; }
  void release();
};

class CodeStore {
 public:
  bool begin();

  // The canonical key for a state, e.g. "cool_24_auto" or "off".
  static String keyFor(const ac::State &state);
  static String buttonKey(const String &name);
  static bool validKey(const String &key);

  bool has(const String &key) const;
  uint16_t count() const { return count_; }

  // Caller owns the returned buffer and must call RawCode::release().
  bool load(const String &key, RawCode &out) const;

  // Best available match for a state; see the degradation rules above.
  bool resolve(const ac::State &state, RawCode &out, String &usedKey) const;

  bool store(const String &key, const uint16_t *timings, uint16_t length,
             uint16_t carrierKhz);
  bool remove(const String &key);
  void clear();

  // ["off", "cool_24_auto", ...] with per-code metadata, for the UI.
  void listJson(JsonArray out) const;

  // One code as {"key":..., "khz":38, "timings":[...]}.
  bool codeToJson(const String &key, JsonObject out) const;

  size_t bytesUsed() const;

 private:
  static String pathFor(const String &key);
  uint16_t recount();

  uint16_t count_ = 0;
};

extern CodeStore codes;

}  // namespace ir
