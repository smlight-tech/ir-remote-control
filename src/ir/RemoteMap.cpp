#include "RemoteMap.h"

#include <IRutils.h>
#include <LittleFS.h>

#include "../app/Scenes.h"
#include "../core/CommandBus.h"
#include "../core/Log.h"
#include "../core/Settings.h"
#include "IrService.h"

namespace ir {
namespace {
const char *kTag = "remote";
const char *kPath = "/remotes.json";

// Long enough to swallow the repeat frames of a held button, short enough that
// two deliberate presses still register as two.
const uint32_t kDebounceMs = 400;

struct ActionName {
  RemoteAction action;
  const char *name;
};
const ActionName kActions[] = {
    {RemoteAction::PowerToggle, "power_toggle"},
    {RemoteAction::PowerOn, "power_on"},
    {RemoteAction::PowerOff, "power_off"},
    {RemoteAction::TempUp, "temp_up"},
    {RemoteAction::TempDown, "temp_down"},
    {RemoteAction::ModeNext, "mode_next"},
    {RemoteAction::FanNext, "fan_next"},
    {RemoteAction::SwingToggle, "swing_toggle"},
    {RemoteAction::Scene, "scene"},
    {RemoteAction::Resend, "resend"},
};

// Cycling orders. Deliberately short: a single button that walks every mode an
// air conditioner can name would take eight presses to get back where it was.
const stdAc::opmode_t kModeCycle[] = {
    stdAc::opmode_t::kCool, stdAc::opmode_t::kHeat, stdAc::opmode_t::kDry,
    stdAc::opmode_t::kFan, stdAc::opmode_t::kAuto};

const stdAc::fanspeed_t kFanCycle[] = {
    stdAc::fanspeed_t::kAuto, stdAc::fanspeed_t::kLow,
    stdAc::fanspeed_t::kMedium, stdAc::fanspeed_t::kHigh};

template <typename T, size_t N>
T nextIn(const T (&table)[N], T current) {
  for (size_t i = 0; i < N; i++)
    if (table[i] == current) return table[(i + 1) % N];
  return table[0];
}

}  // namespace

RemoteMap remotes;

// ---------------------------------------------------------------------------

const char *RemoteMap::actionName(RemoteAction action) {
  for (const ActionName &entry : kActions)
    if (entry.action == action) return entry.name;
  return "none";
}

bool RemoteMap::parseAction(const char *text, RemoteAction &out) {
  if (text == nullptr) return false;
  for (const ActionName &entry : kActions) {
    if (strcasecmp(entry.name, text) == 0) {
      out = entry.action;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------

void RemoteMap::begin() {
  load();
  if (!bindings_.empty()) {
    LOG_I(kTag, "%u bound button(s)", (unsigned)bindings_.size());
  }
}

const RemoteBinding *RemoteMap::find(const Capture &capture) const {
  for (const RemoteBinding &binding : bindings_) {
    if (binding.protocol == capture.protocol && binding.value == capture.value &&
        binding.bits == capture.bits) {
      return &binding;
    }
  }
  return nullptr;
}

bool RemoteMap::handle(const Capture &capture) {
  if (bindings_.empty()) return false;
  // A repeat frame carries no identity of its own; ignoring them is what stops
  // a held button running away.
  if (capture.repeat) return true;

  const RemoteBinding *binding = find(capture);
  if (binding == nullptr) return false;

  if (capture.value == lastValue_ && millis() - lastAt_ < kDebounceMs) {
    return true;   // recognised, but too soon to act on again
  }
  lastValue_ = capture.value;
  lastAt_ = millis();

  LOG_I(kTag, "'%s' -> %s", binding->label.c_str(), actionName(binding->action));
  return perform(*binding);
}

bool RemoteMap::perform(const RemoteBinding &binding) {
  const ac::State &state = bus::commands.state();
  ac::Delta delta;

  switch (binding.action) {
    case RemoteAction::PowerToggle:
      delta.hasPower = true;
      delta.power = !state.power;
      break;
    case RemoteAction::PowerOn:
      delta.hasPower = true;
      delta.power = true;
      break;
    case RemoteAction::PowerOff:
      delta.hasPower = true;
      delta.power = false;
      break;
    case RemoteAction::TempUp:
      delta.hasDegrees = true;
      delta.degrees = state.degrees + cfg::settings.ac.tempStep;
      break;
    case RemoteAction::TempDown:
      delta.hasDegrees = true;
      delta.degrees = state.degrees - cfg::settings.ac.tempStep;
      break;
    case RemoteAction::ModeNext:
      delta.hasMode = true;
      delta.mode = nextIn(kModeCycle, state.mode);
      delta.hasPower = true;
      delta.power = true;
      break;
    case RemoteAction::FanNext:
      delta.hasFan = true;
      delta.fan = nextIn(kFanCycle, state.fanspeed);
      break;
    case RemoteAction::SwingToggle:
      delta.hasSwingV = true;
      delta.swingv = state.swingv == stdAc::swingv_t::kOff
                         ? stdAc::swingv_t::kAuto
                         : stdAc::swingv_t::kOff;
      break;

    case RemoteAction::Scene: {
      String error;
      if (!app::scenes.apply(binding.argument, src::Source::Remote, error)) {
        LOG_W(kTag, "scene '%s' not applied: %s", binding.argument.c_str(),
              error.c_str());
        return false;
      }
      return true;
    }

    case RemoteAction::Resend:
      bus::commands.resend(src::Source::Remote);
      return true;

    default:
      return false;
  }

  const bus::Outcome outcome = bus::commands.apply(delta, src::Source::Remote);
  if (!outcome.ok() && outcome.result != bus::Result::Deferred) {
    LOG_W(kTag, "not applied: %s", outcome.message.c_str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------

bool RemoteMap::bind(const RemoteBinding &binding, String &error) {
  if (binding.action == RemoteAction::None) {
    error = F("no action given");
    return false;
  }
  if (binding.protocol == decode_type_t::UNKNOWN && binding.value == 0) {
    error = F("that frame carried nothing we can match on");
    return false;
  }

  // Re-teaching a button replaces its binding; otherwise the first one would
  // win forever and the user would think the second attempt did nothing.
  for (RemoteBinding &existing : bindings_) {
    if (existing.protocol == binding.protocol && existing.value == binding.value &&
        existing.bits == binding.bits) {
      const String label = existing.label;
      existing = binding;
      if (existing.label.isEmpty()) existing.label = label;
      save();
      LOG_I(kTag, "rebound '%s'", existing.label.c_str());
      return true;
    }
  }

  if (bindings_.size() >= kMaxBindings) {
    error = String(F("at most ")) + kMaxBindings + F(" buttons can be bound");
    return false;
  }

  bindings_.push_back(binding);
  save();
  LOG_I(kTag, "bound '%s' (%s) to %s", binding.label.c_str(),
        typeToString(binding.protocol).c_str(), actionName(binding.action));
  return true;
}

bool RemoteMap::remove(size_t index) {
  if (index >= bindings_.size()) return false;
  bindings_.erase(bindings_.begin() + index);
  save();
  return true;
}

void RemoteMap::clear() {
  bindings_.clear();
  save();
  LOG_W(kTag, "all bound buttons erased");
}

// ---------------------------------------------------------------------------

void RemoteMap::toJson(JsonArray out) const {
  for (const RemoteBinding &binding : bindings_) {
    JsonObject entry = out.add<JsonObject>();
    entry["label"] = binding.label;
    entry["protocol"] = typeToString(binding.protocol);
    // As text: a 64-bit value does not survive a round trip through
    // JavaScript's number type, and the browser has to show these.
    char hex[19];
    snprintf(hex, sizeof(hex), "0x%llX",
             static_cast<unsigned long long>(binding.value));
    entry["value"] = hex;
    entry["bits"] = binding.bits;
    entry["action"] = actionName(binding.action);
    if (!binding.argument.isEmpty()) entry["argument"] = binding.argument;
  }
}

bool RemoteMap::fromJson(JsonArrayConst in, String &error) {
  if (in.size() > kMaxBindings) {
    error = String(F("at most ")) + kMaxBindings + F(" buttons can be bound");
    return false;
  }

  std::vector<RemoteBinding> parsed;
  for (JsonObjectConst entry : in) {
    RemoteBinding binding;
    binding.label = entry["label"] | "";
    binding.protocol = strToDecodeType(entry["protocol"] | "UNKNOWN");
    binding.bits = entry["bits"] | 0;
    binding.argument = entry["argument"] | "";

    const char *value = entry["value"];
    binding.value = value != nullptr ? strtoull(value, nullptr, 0) : 0;

    if (!parseAction(entry["action"] | "", binding.action)) {
      error = String(F("unknown action: ")) + (entry["action"] | "?");
      return false;
    }
    parsed.push_back(binding);
  }

  bindings_ = parsed;
  return true;
}

// ---------------------------------------------------------------------------

bool RemoteMap::load() {
  File file = LittleFS.open(kPath, "r");
  if (!file) return false;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    LOG_W(kTag, "bindings parse error: %s", err.c_str());
    return false;
  }

  String error;
  if (!fromJson(doc.as<JsonArrayConst>(), error)) {
    LOG_W(kTag, "bindings rejected: %s", error.c_str());
    return false;
  }
  return true;
}

bool RemoteMap::save() {
  JsonDocument doc;
  JsonArray root = doc.to<JsonArray>();
  toJson(root);

  File file = LittleFS.open(kPath, "w");
  if (!file) {
    LOG_E(kTag, "cannot write %s", kPath);
    return false;
  }
  serializeJson(doc, file);
  file.close();
  return true;
}

}  // namespace ir
