#include "Automations.h"

#include <LittleFS.h>

#include "../core/CommandBus.h"
#include "../core/Log.h"
#include "../core/Settings.h"
#include "PeerClient.h"
#include "Peers.h"
#include "Scheduler.h"

namespace app {
namespace {
const char *kTag = "auto";
const char *kPath = "/automations.json";

// Once a second is the right granularity: conditions come from a peer poll
// that is itself seconds old, so evaluating faster would only burn cycles.
const uint32_t kTickMs = 1000;

// The device this rule refers to when it means "the air conditioner I am".
const char *kSelf = "self";

// Runaway detection. A rule that fires this often in this window is caught in
// a loop — most likely two rules whose effects are each other's triggers — and
// is put to sleep rather than left to hammer the air conditioner.
const uint8_t kMaxFiresPerWindow = 6;
const uint32_t kCycleWindowMs = 30000;
const uint32_t kSuppressionMs = 300000;

struct OpName {
  Op op;
  const char *name;
};
const OpName kOps[] = {
    {Op::Equal, "eq"},        {Op::NotEqual, "ne"},
    {Op::Greater, "gt"},      {Op::Less, "lt"},
    {Op::GreaterEqual, "gte"}, {Op::LessEqual, "lte"},
};

bool looksNumeric(const String &text) {
  if (text.isEmpty()) return false;
  char *end = nullptr;
  strtod(text.c_str(), &end);
  return end != nullptr && *end == '\0';
}

bool compare(double left, double right, Op op) {
  switch (op) {
    case Op::Equal:        return left == right;
    case Op::NotEqual:     return left != right;
    case Op::Greater:      return left > right;
    case Op::Less:         return left < right;
    case Op::GreaterEqual: return left >= right;
    case Op::LessEqual:    return left <= right;
  }
  return false;
}

// Booleans arrive as true/false from one device and as 1/0 from another, so
// everything is normalised before comparison.
String normalise(const String &text) {
  if (text.equalsIgnoreCase("true") || text == "1") return "1";
  if (text.equalsIgnoreCase("false") || text == "0") return "0";
  return text;
}

}  // namespace

Automations automations;

// ---------------------------------------------------------------------------

const char *Automations::opName(Op op) {
  for (const OpName &entry : kOps)
    if (entry.op == op) return entry.name;
  return "eq";
}

bool Automations::parseOp(const char *text, Op &out) {
  if (text == nullptr) return false;
  for (const OpName &entry : kOps) {
    if (strcasecmp(entry.name, text) == 0) {
      out = entry.op;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------

void Automations::begin() {
  load();
  if (!rules_.empty()) LOG_I(kTag, "%u rule(s)", (unsigned)rules_.size());
}

// Reads one field of one device, whichever device it is. This is the only
// place the engine touches anything device-shaped.
bool Automations::readField(const String &deviceId, const String &field,
                            JsonDocument &out) const {
  // The clock answers in the same shape a device does, so "if it is 8am" needs
  // no second kind of rule. Minutes past midnight rather than "08:00" because
  // that is what compares with < and >; the editor shows a clock face.
  //
  // An unsynchronised clock answers nothing at all, which is what stops a rule
  // firing at what the device briefly believes is 1 January 1970.
  if (deviceId == "clock") {
    if (!scheduler.timeSynced()) return false;

    struct tm local;
    const time_t value = scheduler.now();
    localtime_r(&value, &local);

    if (field == "minutes") {
      out.set(local.tm_hour * 60 + local.tm_min);
      return true;
    }
    if (field == "weekday") {
      static const char *kDays[] = {"sun", "mon", "tue", "wed",
                                    "thu", "fri", "sat"};
      out.set(kDays[local.tm_wday % 7]);
      return true;
    }

    // Sun times. "Half an hour before sunset" is a far better trigger for an
    // air conditioner than a fixed hour: sunset moves by two hours across a
    // year and 20:00 does not.
    // Up or down rather than true or false: a rule reads "Sun is down", which
    // is what somebody wants to write, and "Daylight is Off" is not.
    if (field == "sun") {
      out.set(scheduler.daylight() ? "up" : "down");
      return true;
    }
    if (field == "daylight") {
      out.set(scheduler.daylight());
      return true;
    }
    const int minutes = local.tm_hour * 60 + local.tm_min;
    if (field == "to_sunset" || field == "to_sunrise") {
      const int when = field == "to_sunset" ? scheduler.sunsetMinutes()
                                            : scheduler.sunriseMinutes();
      if (when < 0) return false;   // not located, or no sun times today
      out.set(when - minutes);      // negative once it has happened
      return true;
    }
    return false;
  }

  if (deviceId == kSelf) {
    const ac::State &state = bus::commands.state();
    if (field == "power")       { out.set(state.power); return true; }
    if (field == "mode")        { out.set(ac::modeName(state.mode)); return true; }
    if (field == "temp")        { out.set(state.degrees); return true; }
    if (field == "fan")         { out.set(ac::fanName(state.fanspeed)); return true; }
    if (field == "swingv")      { out.set(ac::swingVName(state.swingv)); return true; }
    return false;
  }
  return peerClient.readValue(deviceId, field, out);
}

bool Automations::evaluateCondition(const Condition &condition) const {
  JsonDocument actual;
  if (!readField(condition.deviceId, condition.field, actual)) {
    // Unknown, usually because a peer has not answered yet. An unknown value
    // is not a match — a rule must not fire on a device it cannot see.
    return false;
  }

  String left;
  if (actual.as<JsonVariantConst>().is<bool>()) {
    left = actual.as<bool>() ? "1" : "0";
  } else if (actual.as<JsonVariantConst>().is<const char *>()) {
    left = actual.as<const char *>();
  } else {
    left = String(actual.as<double>(), 3);
  }

  const String right = normalise(condition.value);
  left = normalise(left);

  if (looksNumeric(left) && looksNumeric(right)) {
    return compare(left.toDouble(), right.toDouble(), condition.op);
  }
  // Text compares only for equality; "cool" is not greater than "heat".
  const bool same = left.equalsIgnoreCase(right);
  return condition.op == Op::NotEqual ? !same : same;
}

bool Automations::evaluate(const AutomationRule &rule) const {
  if (rule.conditions.empty()) return false;

  for (const Condition &condition : rule.conditions) {
    const bool ok = evaluateCondition(condition);
    if (rule.matchAll && !ok) return false;
    if (!rule.matchAll && ok) return true;
  }
  return rule.matchAll;
}

// ---------------------------------------------------------------------------

void Automations::loop() {
  // Sequences advance on their own clock, independently of evaluation, so a
  // rule waiting five minutes does not hold anything else up.
  for (AutomationRule &rule : rules_) {
    if (rule.running && millis() >= rule.resumeAt) {
      rule.step++;
      advance(rule);
    }
  }

  if (millis() - lastTickAt_ < kTickMs) return;
  lastTickAt_ = millis();

  for (AutomationRule &rule : rules_) {
    if (!rule.enabled) continue;
    if (rule.running) continue;          // finish what it is doing first

    if (rule.suppressedUntil != 0) {
      if (millis() < rule.suppressedUntil) continue;
      rule.suppressedUntil = 0;
      rule.firesInWindow = 0;
      LOG_I(kTag, "'%s' is allowed to run again", rule.name.c_str());
    }

    const bool matches = evaluate(rule);

    if (!matches) {
      // Leaving a match episode is what triggers the else branch — and only
      // if the rule actually did something on the way in, so a rule that never
      // fired does not undo something it never did.
      if (rule.matchSince != 0 && rule.firedThisEpisode &&
          !rule.elseActions.empty()) {
        start(rule, /*elseBranch=*/true);
      }
      rule.matchSince = 0;
      rule.firedThisEpisode = false;
      continue;
    }

    if (rule.matchSince == 0) rule.matchSince = millis();
    if (rule.firedThisEpisode) continue;

    // "for N seconds" — the conditions have to hold, not merely occur.
    if (rule.forSeconds > 0 &&
        millis() - rule.matchSince < rule.forSeconds * 1000UL) {
      continue;
    }
    if (rule.lastFiredAt != 0 && rule.cooldownSeconds > 0 &&
        millis() - rule.lastFiredAt < rule.cooldownSeconds * 1000UL) {
      continue;
    }

    rule.firedThisEpisode = true;
    start(rule, /*elseBranch=*/false);
  }
}

// True when the rule has fired too often too quickly to be anything but a
// loop. Suppressing it is better than letting it keep going: the alternative
// is an air conditioner being told to switch on and off every second.
bool Automations::guardTripped(AutomationRule &rule) {
  const uint32_t now = millis();
  if (rule.windowStartedAt == 0 || now - rule.windowStartedAt > kCycleWindowMs) {
    rule.windowStartedAt = now;
    rule.firesInWindow = 0;
  }
  if (++rule.firesInWindow <= kMaxFiresPerWindow) return false;

  rule.suppressedUntil = now + kSuppressionMs;
  rule.running = false;
  LOG_E(kTag, "'%s' fired %u times in %lu s and looks like a loop — pausing it "
              "for %lu minutes. Check whether two rules are triggering each "
              "other.",
        rule.name.c_str(), rule.firesInWindow,
        (unsigned long)(kCycleWindowMs / 1000),
        (unsigned long)(kSuppressionMs / 60000));
  return true;
}

void Automations::start(AutomationRule &rule, bool elseBranch) {
  if (guardTripped(rule)) return;

  rule.lastFiredAt = millis();
  rule.fireCount++;
  rule.running = true;
  rule.runningElse = elseBranch;
  rule.step = 0;
  rule.resumeAt = 0;
  LOG_I(kTag, "'%s' %s", rule.name.c_str(), elseBranch ? "reversing" : "fired");
  advance(rule);
}

// Runs steps until a wait is reached or the list is exhausted.
void Automations::advance(AutomationRule &rule) {
  const std::vector<Action> &steps =
      rule.runningElse ? rule.elseActions : rule.actions;

  while (rule.step < steps.size()) {
    const Action &action = steps[rule.step];

    if (action.isWait()) {
      rule.resumeAt = millis() + action.waitSeconds * 1000UL;
      return;                     // loop() picks it up again when due
    }
    perform(rule, action);
    rule.step++;
  }

  rule.running = false;
  rule.runningElse = false;
}

bool Automations::perform(const AutomationRule &rule, const Action &action) {
  JsonDocument command;
  if (deserializeJson(command, action.command) != DeserializationError::Ok) {
    LOG_W(kTag, "'%s' has an unreadable step", rule.name.c_str());
    return false;
  }

  if (action.deviceId == kSelf) {
    ac::Delta delta;
    String error;
    if (!ac::deltaFromJson(command.as<JsonObjectConst>(), delta, error)) {
      LOG_W(kTag, "'%s': %s", rule.name.c_str(), error.c_str());
      return false;
    }
    const bus::Outcome outcome =
        bus::commands.apply(delta, src::Source::Automation);
    if (!outcome.ok() && outcome.result != bus::Result::Deferred) {
      LOG_W(kTag, "'%s': %s", rule.name.c_str(), outcome.message.c_str());
      return false;
    }
    return true;
  }

  String error;
  if (!peerClient.command(action.deviceId, command, error)) {
    LOG_W(kTag, "'%s': %s", rule.name.c_str(), error.c_str());
    return false;
  }
  return true;
}

bool Automations::fire(const String &id, String &error) {
  for (AutomationRule &rule : rules_) {
    if (rule.id != id) continue;
    if (rule.actions.empty()) {
      error = F("that rule has no actions");
      return false;
    }
    start(rule, /*elseBranch=*/false);
    return true;
  }
  error = F("no rule with that id");
  return false;
}

// ---------------------------------------------------------------------------

void Automations::toJson(JsonArray out) const {
  for (const AutomationRule &rule : rules_) {
    JsonObject entry = out.add<JsonObject>();
    entry["id"] = rule.id;
    entry["name"] = rule.name;
    entry["enabled"] = rule.enabled;
    entry["match"] = rule.matchAll ? "all" : "any";
    entry["for"] = rule.forSeconds;
    entry["cooldown"] = rule.cooldownSeconds;
    entry["fired"] = rule.fireCount;
    entry["lastFiredSecondsAgo"] =
        rule.lastFiredAt ? (int32_t)((millis() - rule.lastFiredAt) / 1000) : -1;

    JsonArray conditions = entry["conditions"].to<JsonArray>();
    for (const Condition &condition : rule.conditions) {
      JsonObject item = conditions.add<JsonObject>();
      item["device"] = condition.deviceId;
      item["field"] = condition.field;
      item["op"] = opName(condition.op);
      item["value"] = condition.value;
    }

    auto writeSteps = [](JsonArray out, const std::vector<Action> &steps) {
      for (const Action &action : steps) {
        JsonObject item = out.add<JsonObject>();
        if (action.isWait()) {
          item["wait"] = action.waitSeconds;
          continue;
        }
        item["device"] = action.deviceId;
        JsonDocument command;
        if (deserializeJson(command, action.command) == DeserializationError::Ok) {
          item["command"] = command;
        } else {
          item["command"].to<JsonObject>();
        }
      }
    };
    writeSteps(entry["actions"].to<JsonArray>(), rule.actions);
    if (!rule.elseActions.empty()) {
      writeSteps(entry["else"].to<JsonArray>(), rule.elseActions);
    }
    if (rule.suppressedUntil != 0) entry["suppressed"] = true;
  }
}

bool Automations::fromJson(JsonArrayConst in, String &error) {
  if (in.size() > kMaxRules) {
    error = String(F("at most ")) + kMaxRules + F(" automations are supported");
    return false;
  }

  std::vector<AutomationRule> parsed;
  uint16_t sequence = 1;

  for (JsonObjectConst entry : in) {
    AutomationRule rule;
    rule.name = entry["name"] | "";
    if (rule.name.isEmpty()) {
      error = F("every automation needs a name");
      return false;
    }
    rule.id = entry["id"] | "";
    if (rule.id.isEmpty()) rule.id = String(F("rule-")) + sequence;
    sequence++;

    rule.enabled = entry["enabled"] | true;
    rule.matchAll = String(entry["match"] | "all") != "any";
    rule.forSeconds = entry["for"] | 0;
    rule.cooldownSeconds = entry["cooldown"] | 0;

    for (JsonObjectConst item : entry["conditions"].as<JsonArrayConst>()) {
      Condition condition;
      condition.deviceId = item["device"] | kSelf;
      condition.field = item["field"] | "";
      if (!parseOp(item["op"] | "eq", condition.op)) {
        error = String(F("unknown comparison in '")) + rule.name + "'";
        return false;
      }
      // Stored as text so that true, 23.5 and "cool" all round-trip.
      JsonVariantConst value = item["value"];
      if (value.is<bool>())        condition.value = value.as<bool>() ? "true" : "false";
      else if (value.is<float>())  condition.value = String(value.as<float>(), 2);
      else                         condition.value = value.as<const char *>() ?: "";

      if (condition.field.isEmpty()) {
        error = String(F("a condition in '")) + rule.name + F("' has no field");
        return false;
      }
      rule.conditions.push_back(condition);
    }

    auto readSteps = [&](JsonArrayConst in, std::vector<Action> &out) -> bool {
      for (JsonObjectConst item : in) {
        Action action;
        if (item["wait"].is<unsigned int>()) {
          action.waitSeconds = item["wait"].as<unsigned int>();
          if (action.waitSeconds == 0) continue;   // a zero wait is not a step
          out.push_back(action);
          continue;
        }
        action.deviceId = item["device"] | kSelf;
        JsonObjectConst command = item["command"];
        if (command.isNull()) {
          error = String(F("a step in '")) + rule.name + F("' has no command");
          return false;
        }
        serializeJson(command, action.command);
        out.push_back(action);
      }
      return true;
    };
    if (!readSteps(entry["actions"].as<JsonArrayConst>(), rule.actions)) return false;
    if (!readSteps(entry["else"].as<JsonArrayConst>(), rule.elseActions)) return false;

    if (rule.conditions.empty()) {
      error = String(F("'")) + rule.name + F("' has no conditions");
      return false;
    }
    bool doesSomething = false;
    for (const Action &action : rule.actions) {
      if (!action.isWait()) { doesSomething = true; break; }
    }
    if (!doesSomething) {
      error = String(F("'")) + rule.name + F("' only waits, and never does anything");
      return false;
    }

    parsed.push_back(rule);
  }

  rules_ = parsed;
  return true;
}

// ---------------------------------------------------------------------------

bool Automations::load() {
  File file = LittleFS.open(kPath, "r");
  if (!file) return false;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    LOG_W(kTag, "automations parse error: %s", err.c_str());
    return false;
  }

  String error;
  if (!fromJson(doc.as<JsonArrayConst>(), error)) {
    LOG_W(kTag, "automations rejected: %s", error.c_str());
    return false;
  }
  return true;
}

bool Automations::save() {
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
  LOG_I(kTag, "%u automation(s) saved", (unsigned)rules_.size());
  return true;
}

}  // namespace app
