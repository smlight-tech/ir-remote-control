// The internal API every client adapter talks to.
//
// Nothing — not MQTT, not Telegram, not the web UI, not the IR receiver —
// touches the AC or the state directly. They all build an `ac::Delta`, hand it
// to `apply()` with their `src::Source`, and get told what happened. Adding a
// new client means writing an adapter against this one class.
//
// The bus is responsible for: gating disabled sources, clamping values to what
// the AC supports, deciding whether the change needs to go out over IR, and
// fanning the result out to every subscriber.
#pragma once

#include <functional>

#include "AcState.h"
#include "Source.h"

namespace bus {

enum class Result : uint8_t {
  Ok,              // applied, and transmitted if it needed to be
  NoChange,        // valid, but the state was already what was asked for
  SourceDisabled,  // this client is switched off in the settings
  Invalid,         // the delta could not be applied
  NotConfigured,   // no AC protocol known yet — run the learning wizard
  SendFailed,      // the IR transmission itself failed
  Deferred,        // held back by compressor protection; will run shortly
};

struct Outcome {
  Result result = Result::Ok;
  String message;

  bool ok() const { return result == Result::Ok || result == Result::NoChange; }
  const char *code() const;
};

// Implemented by the IR service. Kept abstract so the bus has no dependency on
// IRremoteESP8266, which keeps unit reasoning (and future transports, such as
// an RF or serial-attached AC) simple.
class Transmitter {
 public:
  virtual ~Transmitter() {}
  virtual bool ready() const = 0;
  virtual bool sendState(const ac::State &state, bool beep) = 0;
};

// (state, source, transmitted) — `transmitted` is false when the change was
// merely observed (e.g. the user pressed their own remote).
using Listener = std::function<void(const ac::State &, src::Source, bool)>;

class CommandBus {
 public:
  void begin(Transmitter *transmitter);
  void loop();

  const ac::State &state() const { return state_; }
  src::Source lastSource() const { return lastSource_; }
  uint32_t lastChangeMs() const { return lastChangeAt_; }
  uint32_t revision() const { return revision_; }

  // Main entry point: merge a client's delta and act on it.
  Outcome apply(const ac::Delta &delta, src::Source source);

  // Adopt a complete state observed on the air from the AC's own remote.
  // Never retransmits — the AC has already seen the command.
  Outcome observe(const ac::State &observed, src::Source source);

  // Re-emit the current state without changing it. Useful after a power cut,
  // or when the user suspects the AC missed a command.
  Outcome resend(src::Source source);

  // Writes the state file now if it is stale. Called before a deliberate
  // restart, which is the one shutdown the device ever sees coming.
  void flush();

  // Switches the unit the air conditioner is spoken to in, converting the
  // current target so the room is asked for the same temperature rather than
  // for 24 °F. The protocol carries the unit, so this changes what the unit's
  // own display reads too.
  Outcome setUnit(bool celsius, src::Source source);

  void subscribe(Listener listener);

  // Persisted so the AC can be restored to its last known state after a reboot.
  bool loadPersistedState();

  // The ESP8266 keeps 512 bytes across a reset — anything short of losing
  // power — so the live state lives there and the file becomes a slow backup
  // for the power-cut case alone.
  void saveRtcState() const;
  bool loadRtcState();
  bool persistState();

  // Compressor protection: seconds still to wait before the unit may be
  // switched back on, or 0 when it may go on now.
  uint16_t restartHoldSeconds() const;
  bool hasPendingStart() const { return pendingStart_; }

 private:
  bool blockedByCompressorGuard(const ac::State &next) const;
  void runPendingStart();
  void clamp(ac::State &state) const;
  void notify(src::Source source, bool transmitted);

  static const uint8_t kMaxListeners = 10;

  ac::State state_ = ac::defaultState();
  Transmitter *tx_ = nullptr;
  Listener listeners_[kMaxListeners];
  uint8_t listenerCount_ = 0;

  src::Source lastSource_ = src::Source::System;
  uint32_t lastChangeAt_ = 0;
  uint32_t revision_ = 0;

  // "The file no longer matches the state." Not urgent: the RTC copy already
  // covers every restart that keeps the power on.
  bool statePending_ = false;
  uint32_t statePendingAt_ = 0;

  // Compressor protection.
  bool sawPowerOff_ = false;      // millis() is 0 at boot, so a flag is needed
  uint32_t poweredOffAt_ = 0;
  bool pendingStart_ = false;
  ac::State pendingState_;
  src::Source pendingSource_ = src::Source::System;
};

extern CommandBus commands;

}  // namespace bus
