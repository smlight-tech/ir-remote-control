#include "CommandBus.h"

#include <LittleFS.h>

#include "Log.h"
#include "Settings.h"

namespace bus {
namespace {
const char *kTag = "bus";
const char *kStatePath = "/state.json";
// How stale the state *file* may get. The RTC copy is written on every
// change, so this only bounds what a power cut can lose — a quarter of an
// hour of "the air conditioner is set to 22, not 24", against writing to
// flash every few seconds for years.
const uint32_t kStateFlushMs = 15UL * 60UL * 1000UL;

// RTC user memory is addressed in 4-byte blocks. Offset 32 leaves the first
// 128 bytes to the bootloader and anything else that stakes a claim there.
const uint32_t kRtcOffset = 32;
const uint32_t kRtcMagic = 0x534C5746;   // "SLWF"

float roundToStep(float value, float step) {
  if (step <= 0.0f) return value;
  return roundf(value / step) * step;
}
}  // namespace

CommandBus commands;

const char *Outcome::code() const {
  switch (result) {
    case Result::Ok:             return "ok";
    case Result::NoChange:       return "no_change";
    case Result::SourceDisabled: return "source_disabled";
    case Result::Invalid:        return "invalid";
    case Result::NotConfigured:  return "not_configured";
    case Result::SendFailed:     return "send_failed";
    case Result::Deferred:       return "deferred";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------

void CommandBus::begin(Transmitter *transmitter) {
  tx_ = transmitter;
  if (cfg::settings.ac.restoreOnBoot) {
    // RTC first: it is never older than the file, and after an OTA update or
    // a crash it is the only one that reflects what the unit is doing now.
    if (loadRtcState()) {
      LOG_I(kTag, "state restored from RTC memory");
    } else if (loadPersistedState()) {
      LOG_I(kTag, "state restored from %s", kStatePath);
    }
  }
  // The unit is a property of the installation, not of whatever state was
  // last saved — a device told it lives in Fahrenheit stays in Fahrenheit
  // across a restore, a factory-fresh boot, and a state learned from the
  // remote before the preference was set.
  state_.celsius = cfg::settings.device.celsius;
  clamp(state_);
}

void CommandBus::loop() {
  if (pendingStart_ && restartHoldSeconds() == 0) runPendingStart();

  if (statePending_ && millis() - statePendingAt_ >= kStateFlushMs) {
    persistState();
  }
}

// ---------------------------------------------------------------------------
// Compressor protection
// ---------------------------------------------------------------------------

uint16_t CommandBus::restartHoldSeconds() const {
  const uint16_t minimum = cfg::settings.ac.minOffSeconds;
  if (minimum == 0 || !sawPowerOff_ || state_.power) return 0;

  const uint32_t elapsed = (millis() - poweredOffAt_) / 1000UL;
  if (elapsed >= minimum) return 0;
  return static_cast<uint16_t>(minimum - elapsed);
}

bool CommandBus::blockedByCompressorGuard(const ac::State &next) const {
  // Only a transition from off to on is interesting. Switching off, or
  // adjusting a unit that is already running, is always allowed.
  if (state_.power || !next.power) return false;
  return restartHoldSeconds() > 0;
}

void CommandBus::runPendingStart() {
  pendingStart_ = false;

  ac::State next = pendingState_;
  clamp(next);

  if (tx_ == nullptr || !tx_->ready()) {
    LOG_W(kTag, "deferred start dropped: no transmitter");
    return;
  }

  state_ = next;
  const bool transmitted = tx_->sendState(state_, /*beep=*/false);
  if (!transmitted) {
    LOG_E(kTag, "deferred start failed to transmit");
    return;
  }

  lastSource_ = pendingSource_;
  lastChangeAt_ = millis();
  revision_++;
  saveRtcState();
  statePending_ = true;
  statePendingAt_ = millis();

  LOG_I(kTag, "compressor guard expired, applying the start requested by %s",
        src::name(pendingSource_));
  notify(pendingSource_, transmitted);
}

void CommandBus::subscribe(Listener listener) {
  if (listenerCount_ >= kMaxListeners) {
    LOG_E(kTag, "listener table full, dropping subscriber");
    return;
  }
  listeners_[listenerCount_++] = listener;
}

Outcome CommandBus::setUnit(bool celsius, src::Source source) {
  if (state_.celsius == celsius) {
    Outcome outcome;
    outcome.result = Result::NoChange;
    return outcome;
  }

  ac::Delta delta;
  delta.hasCelsius = true;
  delta.celsius = celsius;
  delta.hasDegrees = true;
  delta.degrees = celsius ? (state_.degrees - 32.0f) * 5.0f / 9.0f
                       : state_.degrees * 9.0f / 5.0f + 32.0f;
  return apply(delta, source);
}

void CommandBus::clamp(ac::State &state) const {
  const cfg::AcSettings &limits = cfg::settings.ac;

  if (state.celsius) {
    if (state.degrees < limits.minTemp) state.degrees = limits.minTemp;
    if (state.degrees > limits.maxTemp) state.degrees = limits.maxTemp;
    state.degrees = roundToStep(state.degrees, limits.tempStep);
  } else {
    // Fahrenheit bounds are derived rather than configured separately; the
    // user only ever sets the range once, in whichever unit they think in.
    const float lo = limits.minTemp * 9.0f / 5.0f + 32.0f;
    const float hi = limits.maxTemp * 9.0f / 5.0f + 32.0f;
    if (state.degrees < lo) state.degrees = lo;
    if (state.degrees > hi) state.degrees = hi;
    state.degrees = roundf(state.degrees);
  }

  if (!limits.protocol.isEmpty()) {
    state.protocol = strToDecodeType(limits.protocol.c_str());
    state.model = limits.model;
  }
}

// ---------------------------------------------------------------------------

Outcome CommandBus::apply(const ac::Delta &delta, src::Source source) {
  Outcome outcome;

  if (!cfg::settings.isSourceEnabled(source)) {
    outcome.result = Result::SourceDisabled;
    outcome.message = String(F("client '")) + src::name(source) +
                      F("' is disabled in the settings");
    LOG_W(kTag, "rejected command from disabled client '%s'", src::name(source));
    return outcome;
  }

  if (delta.empty()) {
    outcome.result = Result::Invalid;
    outcome.message = F("nothing to change");
    return outcome;
  }

  ac::State next = state_;
  const bool changed = delta.applyTo(next);
  clamp(next);

  if (blockedByCompressorGuard(next)) {
    // Hold the request rather than refusing it: a schedule or a thermostat
    // automation that fires during the guard should still take effect, just
    // late. loop() applies it when the timer runs out.
    pendingStart_ = true;
    pendingState_ = next;
    pendingSource_ = source;

    outcome.result = Result::Deferred;
    outcome.message = String(F("compressor protection: starting in ")) +
                      restartHoldSeconds() + F(" s");
    LOG_I(kTag, "%s asked to start the unit; holding for %u s", src::name(source),
          restartHoldSeconds());
    return outcome;
  }

  // Clamping can absorb the whole delta (asking for 35 °C on a 16-30 unit).
  const bool effective = changed && !ac::equal(next, state_);

  const bool wantsTransmit = src::shouldTransmit(source) && (effective || delta.hasBeep);

  if (wantsTransmit && (tx_ == nullptr || !tx_->ready())) {
    outcome.result = Result::NotConfigured;
    outcome.message = F("no air conditioner is configured yet — run the learning wizard");
    return outcome;
  }

  const bool wasOn = state_.power;
  state_ = next;
  if (wasOn && !state_.power) {
    sawPowerOff_ = true;
    poweredOffAt_ = millis();
    pendingStart_ = false;   // an explicit stop cancels any held start
  }

  bool transmitted = false;
  if (wantsTransmit) {
    transmitted = tx_->sendState(state_, delta.hasBeep && delta.beep);
    if (!transmitted) {
      outcome.result = Result::SendFailed;
      outcome.message = F("the infrared transmission failed");
      LOG_E(kTag, "IR send failed for command from '%s'", src::name(source));
      return outcome;
    }
  }

  if (!effective && !transmitted) {
    outcome.result = Result::NoChange;
    return outcome;
  }

  lastSource_ = source;
  lastChangeAt_ = millis();
  revision_++;

  // Only a *changed* state is worth writing. Re-sending the same settings —
  // a scene applied twice, a daily schedule asking for what is already set,
  // an automation firing on a condition that keeps coming back — transmits
  // infrared but leaves the file identical, and flash has a finite number of
  // erases in it.
  if (effective) {
    saveRtcState();
    statePending_ = true;
    statePendingAt_ = millis();
    // Switching off is the moment worth spending a flash write on: it is
    // infrequent, and it is the state most likely to be interrupted by
    // somebody pulling the plug on the way out.
    if (!state_.power) persistState();
  }

  LOG_I(kTag, "%s -> %s %s %.1f%c fan=%s (rev %lu)", src::name(source),
        ac::hvacMode(state_), ac::modeName(state_.mode), state_.degrees,
        state_.celsius ? 'C' : 'F', ac::fanName(state_.fanspeed),
        (unsigned long)revision_);

  notify(source, transmitted);
  return outcome;
}

Outcome CommandBus::observe(const ac::State &observed, src::Source source) {
  Outcome outcome;

  if (!cfg::settings.isSourceEnabled(source)) {
    outcome.result = Result::SourceDisabled;
    return outcome;
  }

  ac::State next = observed;
  // Keep the protocol identity we already trust rather than whatever a single
  // noisy capture claimed.
  next.protocol = state_.protocol;
  next.model = state_.model;
  clamp(next);

  if (ac::equal(next, state_)) {
    outcome.result = Result::NoChange;
    return outcome;
  }

  const bool wasOn = state_.power;
  state_ = next;
  // The physical remote bypasses the compressor guard by definition — the AC
  // has already acted — but the timer still has to track reality.
  if (wasOn && !state_.power) {
    sawPowerOff_ = true;
    poweredOffAt_ = millis();
    pendingStart_ = false;
  }

  lastSource_ = source;
  lastChangeAt_ = millis();
  revision_++;
  saveRtcState();
  statePending_ = true;
  statePendingAt_ = millis();

  LOG_I(kTag, "observed from %s -> %s %.1f%c fan=%s", src::name(source),
        ac::hvacMode(state_), state_.degrees, state_.celsius ? 'C' : 'F',
        ac::fanName(state_.fanspeed));

  notify(source, /*transmitted=*/false);
  return outcome;
}

Outcome CommandBus::resend(src::Source source) {
  Outcome outcome;

  if (!cfg::settings.isSourceEnabled(source)) {
    outcome.result = Result::SourceDisabled;
    return outcome;
  }
  if (tx_ == nullptr || !tx_->ready()) {
    outcome.result = Result::NotConfigured;
    outcome.message = F("no air conditioner is configured yet");
    return outcome;
  }
  if (!tx_->sendState(state_, /*beep=*/false)) {
    outcome.result = Result::SendFailed;
    outcome.message = F("the infrared transmission failed");
    return outcome;
  }

  LOG_I(kTag, "resend requested by %s", src::name(source));
  notify(source, /*transmitted=*/true);
  return outcome;
}

void CommandBus::notify(src::Source source, bool transmitted) {
  for (uint8_t i = 0; i < listenerCount_; i++) {
    if (listeners_[i]) listeners_[i](state_, source, transmitted);
  }
}

// ---------------------------------------------------------------------------

bool CommandBus::loadPersistedState() {
  File file = LittleFS.open(kStatePath, "r");
  if (!file) return false;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    LOG_W(kTag, "state parse error: %s", err.c_str());
    return false;
  }

  ac::Delta delta;
  String error;
  if (!ac::deltaFromJson(doc.as<JsonObjectConst>(), delta, error)) {
    LOG_W(kTag, "state rejected: %s", error.c_str());
    return false;
  }
  delta.applyTo(state_);
  LOG_I(kTag, "restored last known state");
  return true;
}

namespace {

// Everything in the record except the checksum itself.
struct RtcRecord {
  uint32_t magic;
  uint32_t crc;
  uint32_t size;        // sizeof(ac::State), so a firmware that changed the
                        // layout does not read the old one as its own
  ac::State state;
};

uint32_t checksum(const RtcRecord &record) {
  // Small, and it only has to catch uninitialised memory rather than an
  // adversary. Everything after the crc field takes part.
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record.size);
  const size_t length = sizeof(RtcRecord) - offsetof(RtcRecord, size);
  uint32_t sum = 2166136261UL;
  for (size_t i = 0; i < length; i++) {
    sum ^= bytes[i];
    sum *= 16777619UL;
  }
  return sum;
}

}  // namespace

void CommandBus::saveRtcState() const {
  RtcRecord record;
  record.magic = kRtcMagic;
  record.size = sizeof(ac::State);
  record.state = state_;
  record.crc = checksum(record);

  ESP.rtcUserMemoryWrite(kRtcOffset, reinterpret_cast<uint32_t *>(&record),
                         sizeof(record));
}

bool CommandBus::loadRtcState() {
  RtcRecord record;
  if (!ESP.rtcUserMemoryRead(kRtcOffset, reinterpret_cast<uint32_t *>(&record),
                             sizeof(record))) {
    return false;
  }
  if (record.magic != kRtcMagic) return false;
  if (record.size != sizeof(ac::State)) return false;
  if (record.crc != checksum(record)) return false;

  state_ = record.state;
  return true;
}

void CommandBus::flush() {
  if (statePending_) persistState();
}

bool CommandBus::persistState() {
  statePending_ = false;

  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  ac::toJson(state_, root);

  File file = LittleFS.open(kStatePath, "w");
  if (!file) return false;
  serializeJson(doc, file);
  file.close();
  return true;
}

}  // namespace bus
