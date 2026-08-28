// Standalone timers and schedules, so the device stays useful when the network
// (or Home Assistant, or the internet) is not there.
//
// Two rule shapes:
//   * daily   — fires at hh:mm on a set of weekdays
//   * timer   — fires once at an absolute time ("switch off in 45 minutes")
//
// Also owns NTP, because a scheduler without a clock is a decoration.
#pragma once

#include <ArduinoJson.h>

#include <vector>

#include "../core/AcState.h"

namespace app {

enum class RuleKind : uint8_t { Daily, Timer };

struct Rule {
  String id;
  String name;
  bool enabled = true;
  RuleKind kind = RuleKind::Daily;

  uint8_t days = 0b1111111;   // bit 0 = Sunday .. bit 6 = Saturday
  uint8_t hour = 8;
  uint8_t minute = 0;

  time_t fireAt = 0;          // Timer rules only, absolute epoch seconds

  ac::Delta action;
  String actionJson;          // kept verbatim so round-tripping is lossless

  uint32_t lastFiredDay = 0;  // yyyymmdd, so a rule fires at most once a day
};

class Scheduler {
 public:
  void begin();
  void loop();

  bool load();
  bool save();

  void toJson(JsonArray out) const;
  bool fromJson(JsonArrayConst in, String &error);

  // Convenience used by Telegram and the UI: "switch off in N minutes".
  bool addTimer(uint16_t minutes, const ac::Delta &action, const String &name,
                String &error);

  bool timeSynced() const { return synced_; }
  String localTimeString() const;
  time_t now() const;

  // Seconds east of UTC at this instant, so a browser can render the device's
  // local time without knowing anything about its timezone rules.
  long utcOffset() const;

  // Sun times for today, as minutes past local midnight. -1 when the device
  // has not been told where it is, or when the sun does not rise or set at
  // that latitude on that date. Recomputed once a day, not per query.
  int sunriseMinutes() const;
  int sunsetMinutes() const;
  bool daylight() const;

  // Sets the clock by hand, for a device with no route to an NTP server. The
  // board has no battery-backed clock, so this survives until the next reset
  // and no further — and NTP, if it ever answers, wins.
  bool setTime(time_t epoch);
  bool manuallySet() const { return manual_; }

  void reconfigure() { applyTimezone(); }

  size_t count() const { return rules_.size(); }

 private:
  void applyTimezone();
  void fire(Rule &rule);
  void computeSun() const;

  static const uint8_t kMaxRules = 16;

  std::vector<Rule> rules_;
  bool synced_ = false;
  bool manual_ = false;

  // Sun times are pure arithmetic on the date and the location, so they are
  // worked out once per local day and kept.
  mutable uint32_t sunDay_ = 0;
  mutable int sunrise_ = -1;
  mutable int sunset_ = -1;
  uint32_t lastTickAt_ = 0;
  uint32_t nextId_ = 1;
};

extern Scheduler scheduler;

}  // namespace app
