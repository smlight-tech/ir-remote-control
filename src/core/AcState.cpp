#include "AcState.h"

#include <string.h>

namespace ac {
namespace {

struct NameMap {
  const char *name;
  int8_t value;
};

const NameMap kModes[] = {
    {"auto", static_cast<int8_t>(stdAc::opmode_t::kAuto)},
    {"cool", static_cast<int8_t>(stdAc::opmode_t::kCool)},
    {"heat", static_cast<int8_t>(stdAc::opmode_t::kHeat)},
    {"dry", static_cast<int8_t>(stdAc::opmode_t::kDry)},
    {"fan_only", static_cast<int8_t>(stdAc::opmode_t::kFan)},
};

const NameMap kFans[] = {
    {"auto", static_cast<int8_t>(stdAc::fanspeed_t::kAuto)},
    {"min", static_cast<int8_t>(stdAc::fanspeed_t::kMin)},
    {"low", static_cast<int8_t>(stdAc::fanspeed_t::kLow)},
    {"medium", static_cast<int8_t>(stdAc::fanspeed_t::kMedium)},
    {"medium_high", static_cast<int8_t>(stdAc::fanspeed_t::kMediumHigh)},
    {"high", static_cast<int8_t>(stdAc::fanspeed_t::kHigh)},
    {"max", static_cast<int8_t>(stdAc::fanspeed_t::kMax)},
};

const NameMap kSwingV[] = {
    {"off", static_cast<int8_t>(stdAc::swingv_t::kOff)},
    {"auto", static_cast<int8_t>(stdAc::swingv_t::kAuto)},
    {"highest", static_cast<int8_t>(stdAc::swingv_t::kHighest)},
    {"high", static_cast<int8_t>(stdAc::swingv_t::kHigh)},
    {"upper_middle", static_cast<int8_t>(stdAc::swingv_t::kUpperMiddle)},
    {"middle", static_cast<int8_t>(stdAc::swingv_t::kMiddle)},
    {"low", static_cast<int8_t>(stdAc::swingv_t::kLow)},
    {"lowest", static_cast<int8_t>(stdAc::swingv_t::kLowest)},
};

const NameMap kSwingH[] = {
    {"off", static_cast<int8_t>(stdAc::swingh_t::kOff)},
    {"auto", static_cast<int8_t>(stdAc::swingh_t::kAuto)},
    {"left_max", static_cast<int8_t>(stdAc::swingh_t::kLeftMax)},
    {"left", static_cast<int8_t>(stdAc::swingh_t::kLeft)},
    {"middle", static_cast<int8_t>(stdAc::swingh_t::kMiddle)},
    {"right", static_cast<int8_t>(stdAc::swingh_t::kRight)},
    {"right_max", static_cast<int8_t>(stdAc::swingh_t::kRightMax)},
    {"wide", static_cast<int8_t>(stdAc::swingh_t::kWide)},
};

template <size_t N>
const char *nameOf(const NameMap (&table)[N], int8_t value, const char *fallback) {
  for (size_t i = 0; i < N; i++)
    if (table[i].value == value) return table[i].name;
  return fallback;
}

template <size_t N>
bool valueOf(const NameMap (&table)[N], const char *text, int8_t &out) {
  if (text == nullptr) return false;
  for (size_t i = 0; i < N; i++)
    if (strcasecmp(table[i].name, text) == 0) {
      out = table[i].value;
      return true;
    }
  return false;
}

// Accepts true/false, 1/0, "on"/"off", "yes"/"no" — clients are diverse.
bool readBool(JsonVariantConst v, bool &out) {
  if (v.is<bool>()) {
    out = v.as<bool>();
    return true;
  }
  if (v.is<int>()) {
    out = v.as<int>() != 0;
    return true;
  }
  const char *s = v.as<const char *>();
  if (s == nullptr) return false;
  if (!strcasecmp(s, "on") || !strcasecmp(s, "true") || !strcasecmp(s, "yes") ||
      !strcasecmp(s, "1")) {
    out = true;
    return true;
  }
  if (!strcasecmp(s, "off") || !strcasecmp(s, "false") || !strcasecmp(s, "no") ||
      !strcasecmp(s, "0")) {
    out = false;
    return true;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------

State defaultState() {
  State s;
  s.protocol = decode_type_t::UNKNOWN;
  s.model = -1;
  s.power = false;
  s.mode = stdAc::opmode_t::kCool;
  s.degrees = 24.0f;
  s.celsius = true;
  s.fanspeed = stdAc::fanspeed_t::kAuto;
  s.swingv = stdAc::swingv_t::kOff;
  s.swingh = stdAc::swingh_t::kOff;
  s.quiet = false;
  s.turbo = false;
  s.econo = false;
  s.light = true;
  s.filter = false;
  s.clean = false;
  s.beep = false;
  s.sleep = -1;
  s.clock = -1;
  return s;
}

bool equal(const State &a, const State &b) {
  return a.power == b.power && a.mode == b.mode && a.degrees == b.degrees &&
         a.celsius == b.celsius && a.fanspeed == b.fanspeed &&
         a.swingv == b.swingv && a.swingh == b.swingh && a.quiet == b.quiet &&
         a.turbo == b.turbo && a.econo == b.econo && a.light == b.light &&
         a.filter == b.filter && a.clean == b.clean && a.sleep == b.sleep;
}

// ---------------------------------------------------------------------------

bool Delta::empty() const {
  return !(hasPower || hasMode || hasDegrees || hasCelsius || hasFan ||
           hasSwingV || hasSwingH || hasQuiet || hasTurbo || hasEcono ||
           hasLight || hasFilter || hasClean || hasBeep || hasSleep);
}

bool Delta::applyTo(State &s) const {
  const State before = s;

  if (hasPower) s.power = power;
  if (hasMode) s.mode = mode;
  if (hasDegrees) s.degrees = degrees;
  if (hasCelsius) s.celsius = celsius;
  if (hasFan) s.fanspeed = fan;
  if (hasSwingV) s.swingv = swingv;
  if (hasSwingH) s.swingh = swingh;
  if (hasQuiet) s.quiet = quiet;
  if (hasTurbo) s.turbo = turbo;
  if (hasEcono) s.econo = econo;
  if (hasLight) s.light = light;
  if (hasFilter) s.filter = filter;
  if (hasClean) s.clean = clean;
  if (hasSleep) s.sleep = sleep;
  // `beep` is a one-shot request, not part of the persistent state, so it is
  // carried on the Delta and consumed by the sender rather than merged here.

  return !equal(before, s);
}

// ---------------------------------------------------------------------------

const char *modeName(stdAc::opmode_t mode) {
  return nameOf(kModes, static_cast<int8_t>(mode), "auto");
}
const char *fanName(stdAc::fanspeed_t fan) {
  return nameOf(kFans, static_cast<int8_t>(fan), "auto");
}
const char *swingVName(stdAc::swingv_t s) {
  return nameOf(kSwingV, static_cast<int8_t>(s), "off");
}
const char *swingHName(stdAc::swingh_t s) {
  return nameOf(kSwingH, static_cast<int8_t>(s), "off");
}

const char *hvacMode(const State &state) {
  return state.power ? modeName(state.mode) : "off";
}

bool parseMode(const char *text, stdAc::opmode_t &out) {
  int8_t v;
  if (!valueOf(kModes, text, v)) return false;
  out = static_cast<stdAc::opmode_t>(v);
  return true;
}
bool parseFan(const char *text, stdAc::fanspeed_t &out) {
  int8_t v;
  if (!valueOf(kFans, text, v)) return false;
  out = static_cast<stdAc::fanspeed_t>(v);
  return true;
}
bool parseSwingV(const char *text, stdAc::swingv_t &out) {
  int8_t v;
  if (!valueOf(kSwingV, text, v)) return false;
  out = static_cast<stdAc::swingv_t>(v);
  return true;
}
bool parseSwingH(const char *text, stdAc::swingh_t &out) {
  int8_t v;
  if (!valueOf(kSwingH, text, v)) return false;
  out = static_cast<stdAc::swingh_t>(v);
  return true;
}

// ---------------------------------------------------------------------------

void toJson(const State &s, JsonObject out) {
  out["power"] = s.power;
  out["mode"] = modeName(s.mode);
  out["hvac_mode"] = hvacMode(s);
  out["temp"] = s.degrees;
  out["unit"] = s.celsius ? "C" : "F";
  out["fan"] = fanName(s.fanspeed);
  out["swingv"] = swingVName(s.swingv);
  out["swingh"] = swingHName(s.swingh);
  out["quiet"] = s.quiet;
  out["turbo"] = s.turbo;
  out["econo"] = s.econo;
  out["light"] = s.light;
  out["filter"] = s.filter;
  out["clean"] = s.clean;
  out["sleep"] = s.sleep;
  out["protocol"] = typeToString(s.protocol);
  out["model"] = s.model;
}

bool deltaFromJson(JsonObjectConst in, Delta &d, String &error) {
  // "hvac_mode" is the Home Assistant spelling and folds "off" into power.
  JsonVariantConst hvac = in["hvac_mode"];
  if (hvac.isNull()) hvac = in["mode"];
  if (!hvac.isNull()) {
    const char *text = hvac.as<const char *>();
    if (text == nullptr) {
      error = F("mode must be a string");
      return false;
    }
    if (!strcasecmp(text, "off")) {
      d.hasPower = true;
      d.power = false;
    } else if (parseMode(text, d.mode)) {
      d.hasMode = true;
      // Naming a mode implies turning on, unless power was stated explicitly.
      if (in["power"].isNull()) {
        d.hasPower = true;
        d.power = true;
      }
    } else {
      error = String(F("unknown mode: ")) + text;
      return false;
    }
  }

  if (!in["power"].isNull()) {
    if (!readBool(in["power"], d.power)) {
      error = F("power must be a boolean");
      return false;
    }
    d.hasPower = true;
  }

  JsonVariantConst temp = in["temp"];
  if (temp.isNull()) temp = in["temperature"];
  if (!temp.isNull()) {
    if (!temp.is<float>() && !temp.is<int>()) {
      error = F("temp must be a number");
      return false;
    }
    d.degrees = temp.as<float>();
    d.hasDegrees = true;
  }

  if (!in["unit"].isNull()) {
    const char *u = in["unit"].as<const char *>();
    if (u == nullptr || (u[0] != 'C' && u[0] != 'c' && u[0] != 'F' && u[0] != 'f')) {
      error = F("unit must be C or F");
      return false;
    }
    d.celsius = (u[0] == 'C' || u[0] == 'c');
    d.hasCelsius = true;
  }

  JsonVariantConst fan = in["fan"];
  if (fan.isNull()) fan = in["fan_mode"];
  if (!fan.isNull()) {
    if (!parseFan(fan.as<const char *>(), d.fan)) {
      error = String(F("unknown fan speed: ")) + (fan.as<const char *>() ?: "?");
      return false;
    }
    d.hasFan = true;
  }

  JsonVariantConst swing = in["swingv"];
  if (swing.isNull()) swing = in["swing_mode"];
  if (!swing.isNull()) {
    if (!parseSwingV(swing.as<const char *>(), d.swingv)) {
      error = String(F("unknown swing mode: ")) + (swing.as<const char *>() ?: "?");
      return false;
    }
    d.hasSwingV = true;
  }

  if (!in["swingh"].isNull()) {
    if (!parseSwingH(in["swingh"].as<const char *>(), d.swingh)) {
      error = F("unknown horizontal swing mode");
      return false;
    }
    d.hasSwingH = true;
  }

  struct Flag {
    const char *key;
    bool *has;
    bool *value;
  };
  const Flag flags[] = {
      {"quiet", &d.hasQuiet, &d.quiet},   {"turbo", &d.hasTurbo, &d.turbo},
      {"econo", &d.hasEcono, &d.econo},   {"light", &d.hasLight, &d.light},
      {"filter", &d.hasFilter, &d.filter}, {"clean", &d.hasClean, &d.clean},
      {"beep", &d.hasBeep, &d.beep},
  };
  for (const Flag &f : flags) {
    JsonVariantConst v = in[f.key];
    if (v.isNull()) continue;
    if (!readBool(v, *f.value)) {
      error = String(f.key) + F(" must be a boolean");
      return false;
    }
    *f.has = true;
  }

  if (!in["sleep"].isNull()) {
    if (!in["sleep"].is<int>()) {
      error = F("sleep must be an integer");
      return false;
    }
    d.sleep = in["sleep"].as<int16_t>();
    d.hasSleep = true;
  }

  return true;
}

}  // namespace ac
