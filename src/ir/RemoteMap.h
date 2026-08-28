// Buttons on *any* remote, bound to actions.
//
// The receiver hears every infrared handset in the room, not only the air
// conditioner's. That makes an obvious feature possible: pick a button you
// never use on the television remote — the coloured ones, teletext, a spare
// input key — and make it turn the air conditioner on.
//
// This is a different thing from tracking the AC's own remote. That path
// decodes a *complete air-conditioner state* and adopts it silently, without
// transmitting, because the unit has already obeyed. A bound button carries no
// state at all: it is a single code that means "do this", and the bridge has
// to actually send the resulting command over infrared.
//
// Matching is on protocol + value + bit count. Remotes the library cannot name
// still work, because the build enables DECODE_HASH: an unrecognised frame
// yields a stable hash that is perfectly good as an identity even though it
// tells us nothing about the manufacturer.
#pragma once

#include <ArduinoJson.h>
#include <IRremoteESP8266.h>

#include <vector>

#include "../core/AcState.h"
#include "../core/Source.h"

namespace ir {

struct Capture;

enum class RemoteAction : uint8_t {
  None = 0,
  PowerToggle,
  PowerOn,
  PowerOff,
  TempUp,
  TempDown,
  ModeNext,
  FanNext,
  SwingToggle,
  Scene,        // argument holds the scene id
  Resend,
};

struct RemoteBinding {
  String label;                                  // "TV red button"
  decode_type_t protocol = decode_type_t::UNKNOWN;
  uint64_t value = 0;
  uint16_t bits = 0;
  RemoteAction action = RemoteAction::None;
  String argument;
};

class RemoteMap {
 public:
  void begin();

  bool load();
  bool save();

  size_t count() const { return bindings_.size(); }
  const std::vector<RemoteBinding> &all() const { return bindings_; }

  // Looks the capture up and performs the bound action. Returns true when the
  // frame was recognised and handled.
  bool handle(const Capture &capture);

  // Replaces an existing binding for the same code rather than stacking a
  // second one on it — re-teaching a button should re-teach it.
  bool bind(const RemoteBinding &binding, String &error);
  bool remove(size_t index);
  void clear();

  void toJson(JsonArray out) const;
  bool fromJson(JsonArrayConst in, String &error);

  static const char *actionName(RemoteAction action);
  static bool parseAction(const char *text, RemoteAction &out);

 private:
  const RemoteBinding *find(const Capture &capture) const;
  bool perform(const RemoteBinding &binding);

  static const uint8_t kMaxBindings = 16;

  std::vector<RemoteBinding> bindings_;

  // A held button repeats every ~100 ms. Without this, resting a thumb on
  // "temperature up" would run the setpoint to the end of its range.
  uint64_t lastValue_ = 0;
  uint32_t lastAt_ = 0;
};

extern RemoteMap remotes;

}  // namespace ir
