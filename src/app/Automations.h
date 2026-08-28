// IF / THEN rules across this device and the ones paired with it.
//
//   IF   this air conditioner is on
//   AND  the bedroom lamp brightness is below 100
//   FOR  2 minutes
//   THEN set the lamp to preset 3
//
// Rules are evaluated here rather than in the browser because that is the only
// place they can run at three in the morning with every phone asleep.
//
// Two properties keep the engine honest and cheap:
//
//   * Rules fire on the *edge*. A rule whose conditions are true runs once
//     when they become true, not every second while they stay true. Otherwise
//     "if it is warm, turn the fan up" would walk the fan to maximum.
//
//   * A rule never triggers itself. Actions carry src::Source::Automation, and
//     evaluation ignores its own changes for a moment afterwards, so two rules
//     pointing at each other settle instead of oscillating.
//
// Which fields a device offers, and what an action looks like for it, comes
// from devicetypes.json. The engine itself knows nothing about air
// conditioners or lights — it compares values and posts commands.
#pragma once

#include <ArduinoJson.h>

#include <vector>

namespace app {

enum class Op : uint8_t { Equal, NotEqual, Greater, Less, GreaterEqual, LessEqual };

struct Condition {
  String deviceId;      // "self", or a peer id
  String field;         // "power", "temp", "bri", …
  Op op = Op::Equal;
  String value;         // compared as a number when both sides look numeric
};

struct Action {
  String deviceId;      // "self", or a peer id
  String command;       // JSON object, verbatim, in that device's own shape

  // A step with a wait is a pause, not a command. Sequences are the one thing
  // a flat rule genuinely cannot express without this — "turn it on, wait five
  // minutes, then set the lamp" — and it costs far less than a flow graph.
  uint16_t waitSeconds = 0;
  bool isWait() const { return waitSeconds > 0 && deviceId.isEmpty(); }
};

struct AutomationRule {
  String id;
  String name;
  bool enabled = true;
  bool matchAll = true;         // AND when true, OR when false
  uint16_t forSeconds = 0;      // conditions must hold this long first
  uint16_t cooldownSeconds = 0; // shortest gap between firings

  std::vector<Condition> conditions;
  std::vector<Action> actions;
  // Run when the conditions stop being true, having been true. "When the AC
  // comes on, light the lamp; when it goes off, put it out" is one rule.
  std::vector<Action> elseActions;

  // --- evaluation state, not persisted -------------------------------------

  uint32_t matchSince = 0;      // when the current match episode began, 0 if none
  bool firedThisEpisode = false;
  uint32_t lastFiredAt = 0;
  uint32_t fireCount = 0;

  // A sequence in progress. Steps run one at a time so a wait does not block
  // anything else on the device.
  bool running = false;
  bool runningElse = false;
  size_t step = 0;
  uint32_t resumeAt = 0;

  // Runaway detection. Two rules pointing at each other would otherwise fire
  // forever, each one's effect being the other's trigger.
  uint32_t windowStartedAt = 0;
  uint8_t firesInWindow = 0;
  uint32_t suppressedUntil = 0;
};

class Automations {
 public:
  void begin();
  void loop();

  bool load();
  bool save();

  const std::vector<AutomationRule> &all() const { return rules_; }
  size_t count() const { return rules_.size(); }

  void toJson(JsonArray out) const;
  bool fromJson(JsonArrayConst in, String &error);

  // Runs a rule's actions now, ignoring its conditions. The "test" button.
  bool fire(const String &id, String &error);

  static const char *opName(Op op);
  static bool parseOp(const char *text, Op &out);

 private:
  bool evaluate(const AutomationRule &rule) const;
  bool evaluateCondition(const Condition &condition) const;
  void start(AutomationRule &rule, bool elseBranch);
  void advance(AutomationRule &rule);
  bool perform(const AutomationRule &rule, const Action &action);
  bool guardTripped(AutomationRule &rule);
  bool readField(const String &deviceId, const String &field,
                 JsonDocument &out) const;

  static const uint8_t kMaxRules = 12;

  std::vector<AutomationRule> rules_;
  uint32_t lastTickAt_ = 0;
};

extern Automations automations;

}  // namespace app
