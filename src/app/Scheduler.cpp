#include "Scheduler.h"

#include <LittleFS.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>

#include "../core/CommandBus.h"
#include "../core/Log.h"
#include "../core/Settings.h"

namespace app {
namespace {
const char *kTag = "sched";
const char *kPath = "/schedules.json";

// Anything before 2021 means NTP has not answered yet.
const time_t kSaneEpoch = 1609459200;

// Degrees, because every published form of this calculation is in degrees and
// transcribing it into radians is how sign errors get in.
//
// Double, though single precision would do for the angles: the float trig
// functions turned out to cost the same flash and half a kilobyte more RAM,
// and RAM is the scarcer of the two here. Day numbers need the precision in
// any case — sub-minute accuracy on a Julian day is ten significant digits.
double sinDeg(double degrees) { return sin(degrees * M_PI / 180.0); }
double cosDeg(double degrees) { return cos(degrees * M_PI / 180.0); }
double asinDeg(double value) { return asin(value) * 180.0 / M_PI; }
double acosDeg(double value) { return acos(value) * 180.0 / M_PI; }

// Reduces to [0, 360) without pulling in double fmod.
double wrap360(double degrees) {
  return degrees - 360.0 * floor(degrees / 360.0);
}

uint32_t dayStamp(const struct tm &t) {
  return (t.tm_year + 1900) * 10000UL + (t.tm_mon + 1) * 100UL + t.tm_mday;
}
}  // namespace

Scheduler scheduler;

// ---------------------------------------------------------------------------

void Scheduler::begin() {
  applyTimezone();
  load();
}

void Scheduler::applyTimezone() {
  sunDay_ = 0;   // location or timezone may have moved; recompute

  const cfg::TimeSettings &t = cfg::settings.time;

  if (t.ntpServer.isEmpty()) {
    // No server, no traffic: the timezone still has to be applied, so it is
    // set directly rather than through configTime(), which would start SNTP.
    // (A server removed mid-session stops being asked at the next restart.)
    setenv("TZ", t.timezone.c_str(), 1);
    tzset();
    LOG_I(kTag, "clock: no time server, timezone %s", t.timezone.c_str());
    return;
  }

  // Only the server that was asked for. The usual practice of listing two
  // more as fallbacks means contacting strangers nobody chose.
  configTime(t.timezone.c_str(), t.ntpServer.c_str(), nullptr, nullptr);
  LOG_I(kTag, "clock: NTP %s, timezone %s", t.ntpServer.c_str(),
        t.timezone.c_str());
}

time_t Scheduler::now() const {
  time_t value = ::time(nullptr);
  return value;
}

long Scheduler::utcOffset() const {
  const time_t value = now();
  if (value < kSaneEpoch) return 0;

  // Taken as the difference between the same instant rendered two ways, which
  // needs no timezone rules of its own and is already correct for whether
  // summer time is in force. (esp8266 newlib's struct tm has no tm_gmtoff.)
  struct tm local, utc;
  localtime_r(&value, &local);
  gmtime_r(&value, &utc);

  long offset = (local.tm_hour - utc.tm_hour) * 3600L +
                (local.tm_min - utc.tm_min) * 60L +
                (local.tm_sec - utc.tm_sec);

  // The two renderings can land either side of midnight; tm_yday wraps at new
  // year, hence comparing the difference rather than the days themselves.
  const int days = local.tm_yday - utc.tm_yday;
  if (days == 1 || days < -1) offset += 86400L;
  else if (days == -1 || days > 1) offset -= 86400L;

  return offset;
}

bool Scheduler::setTime(time_t epoch) {
  if (epoch < kSaneEpoch) return false;

  struct timeval tv = {epoch, 0};
  if (settimeofday(&tv, nullptr) != 0) return false;

  manual_ = true;
  synced_ = true;
  LOG_I(kTag, "clock set by hand: %s", localTimeString().c_str());
  return true;
}

// Sunrise and sunset by the usual low-precision solar position algorithm —
// good to about a minute, which is far better than anything downstream needs
// and costs no network at all. Worth having on a device that may never reach
// the internet: sunset moves by two hours across a year and "20:00" does not.
//
// Everything is worked out in UTC and converted at the end, so the timezone
// and summer time are handled by the same code that handles them everywhere
// else rather than by a second rule set here.
void Scheduler::computeSun() const {
  const time_t value = now();
  if (value < kSaneEpoch) return;

  struct tm local;
  localtime_r(&value, &local);
  const uint32_t today = dayStamp(local);
  if (today == sunDay_) return;      // already done for this local day

  sunDay_ = today;
  sunrise_ = -1;
  sunset_ = -1;

  const cfg::TimeSettings &settings = cfg::settings.time;
  if (!settings.located()) return;

  const double latitude = settings.latitude;
  const double longitude = settings.longitude;

  // Days since 2000-01-01 12:00 UTC, for local noon — which is the instant the
  // whole calculation is anchored on. Checked against NOAA's own algorithm at
  // five latitudes across four seasons: never more than a minute apart.
  const double julianDay =
      floor((value + utcOffset()) / 86400.0) + 2440587.5 + 0.5;
  const double n = floor(julianDay - 2451545.0 + 0.0008);

  const double meanSolarNoon = n - longitude / 360.0;
  const double meanAnomaly = wrap360(357.5291 + 0.98560028 * meanSolarNoon);
  const double centre = 1.9148 * sinDeg(meanAnomaly) +
                        0.0200 * sinDeg(2 * meanAnomaly) +
                        0.0003 * sinDeg(3 * meanAnomaly);
  const double eclipticLongitude =
      wrap360(meanAnomaly + centre + 180.0 + 102.9372);
  const double transit = 2451545.0 + meanSolarNoon +
                         0.0053 * sinDeg(meanAnomaly) -
                         0.0069 * sinDeg(2 * eclipticLongitude);

  const double declination = asinDeg(sinDeg(eclipticLongitude) * sinDeg(23.44));

  // -0.833° puts the *upper limb* of the sun on the horizon and allows for
  // refraction, which is what people mean by sunrise.
  const double numerator = sinDeg(-0.833) -
                           sinDeg(latitude) * sinDeg(declination);
  const double denominator = cosDeg(latitude) * cosDeg(declination);
  const double cosHourAngle = numerator / denominator;

  if (cosHourAngle > 1.0 || cosHourAngle < -1.0) {
    // Polar night or midnight sun: no rise, no set. Left at -1, and daylight()
    // answers from the sun's declination against the latitude instead.
    return;
  }

  const double hourAngle = acosDeg(cosHourAngle);
  const double setJulian = transit + hourAngle / 360.0;
  const double riseJulian = transit - hourAngle / 360.0;

  const long offset = utcOffset();
  const time_t riseEpoch = static_cast<time_t>((riseJulian - 2440587.5) * 86400.0);
  const time_t setEpoch = static_cast<time_t>((setJulian - 2440587.5) * 86400.0);

  struct tm riseLocal, setLocal;
  const time_t riseLocalEpoch = riseEpoch + offset;
  const time_t setLocalEpoch = setEpoch + offset;
  gmtime_r(&riseLocalEpoch, &riseLocal);
  gmtime_r(&setLocalEpoch, &setLocal);

  sunrise_ = riseLocal.tm_hour * 60 + riseLocal.tm_min;
  sunset_ = setLocal.tm_hour * 60 + setLocal.tm_min;
}

int Scheduler::sunriseMinutes() const {
  computeSun();
  return sunrise_;
}

int Scheduler::sunsetMinutes() const {
  computeSun();
  return sunset_;
}

bool Scheduler::daylight() const {
  const int rise = sunriseMinutes();
  const int set = sunsetMinutes();
  if (rise < 0 || set < 0) return true;   // unknown, or the sun never sets

  const time_t value = now();
  struct tm local;
  localtime_r(&value, &local);
  const int minutes = local.tm_hour * 60 + local.tm_min;

  // Above the arctic circle in summer the two can be the wrong way round.
  return rise <= set ? (minutes >= rise && minutes < set)
                     : (minutes >= rise || minutes < set);
}

String Scheduler::localTimeString() const {
  const time_t value = now();
  if (value < kSaneEpoch) return String(F("not synchronised"));

  struct tm local;
  localtime_r(&value, &local);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
  return String(buf);
}

// ---------------------------------------------------------------------------

void Scheduler::loop() {
  // Once a second is plenty; rules have minute resolution.
  if (millis() - lastTickAt_ < 1000) return;
  lastTickAt_ = millis();

  const time_t value = now();
  if (value < kSaneEpoch) {
    synced_ = false;
    return;
  }
  if (!synced_) {
    synced_ = true;
    LOG_I(kTag, "clock synchronised: %s", localTimeString().c_str());
  }

  if (!cfg::settings.schedule.enabled) return;

  struct tm local;
  localtime_r(&value, &local);
  const uint32_t today = dayStamp(local);

  for (Rule &rule : rules_) {
    if (!rule.enabled) continue;

    if (rule.kind == RuleKind::Timer) {
      if (rule.fireAt != 0 && value >= rule.fireAt) {
        fire(rule);
        rule.enabled = false;
        rule.fireAt = 0;
        save();
      }
      continue;
    }

    if (!(rule.days & (1 << local.tm_wday))) continue;
    if (local.tm_hour != rule.hour || local.tm_min != rule.minute) continue;
    if (rule.lastFiredDay == today) continue;

    rule.lastFiredDay = today;
    fire(rule);
  }
}

void Scheduler::fire(Rule &rule) {
  LOG_I(kTag, "rule '%s' firing", rule.name.c_str());
  const bus::Outcome outcome =
      bus::commands.apply(rule.action, src::Source::Schedule);
  if (!outcome.ok()) {
    LOG_W(kTag, "rule '%s' rejected: %s", rule.name.c_str(),
          outcome.message.c_str());
  }
}

// ---------------------------------------------------------------------------

bool Scheduler::addTimer(uint16_t minutes, const ac::Delta &action,
                         const String &name, String &error) {
  if (rules_.size() >= kMaxRules) {
    error = F("no free schedule slots");
    return false;
  }
  const time_t value = now();
  if (value < kSaneEpoch) {
    error = F("the clock is not synchronised yet");
    return false;
  }

  Rule rule;
  rule.id = String(F("timer-")) + String(nextId_++);
  rule.name = name.isEmpty() ? String(F("Timer")) : name;
  rule.kind = RuleKind::Timer;
  rule.fireAt = value + static_cast<time_t>(minutes) * 60;
  rule.action = action;
  rule.enabled = true;

  rules_.push_back(rule);
  save();
  LOG_I(kTag, "timer '%s' set for %u minute(s) from now", rule.name.c_str(),
        minutes);
  return true;
}

// ---------------------------------------------------------------------------

void Scheduler::toJson(JsonArray out) const {
  for (const Rule &rule : rules_) {
    JsonObject entry = out.add<JsonObject>();
    entry["id"] = rule.id;
    entry["name"] = rule.name;
    entry["enabled"] = rule.enabled;
    entry["kind"] = rule.kind == RuleKind::Timer ? "timer" : "daily";
    if (rule.kind == RuleKind::Daily) {
      entry["days"] = rule.days;
      entry["hour"] = rule.hour;
      entry["minute"] = rule.minute;
    } else {
      entry["fireAt"] = static_cast<uint32_t>(rule.fireAt);
      const time_t value = now();
      entry["inSeconds"] =
          rule.fireAt > value ? static_cast<int32_t>(rule.fireAt - value) : 0;
    }

    // The action is stored as the caller wrote it, so nothing is lost in a
    // round trip through the UI.
    JsonDocument action;
    if (!rule.actionJson.isEmpty() &&
        deserializeJson(action, rule.actionJson) == DeserializationError::Ok) {
      entry["action"] = action;
    } else {
      entry["action"].to<JsonObject>();
    }
  }
}

bool Scheduler::fromJson(JsonArrayConst in, String &error) {
  if (in.size() > kMaxRules) {
    error = String(F("at most ")) + kMaxRules + F(" schedules are supported");
    return false;
  }

  std::vector<Rule> parsed;
  for (JsonObjectConst entry : in) {
    Rule rule;
    if (entry["id"].is<const char *>()) {
      rule.id = entry["id"].as<const char *>();
    } else {
      rule.id = String(F("rule-")) + String(nextId_++);
    }
    rule.name = entry["name"] | "";
    rule.enabled = entry["enabled"] | true;

    const String kind = entry["kind"] | "daily";
    rule.kind = kind == "timer" ? RuleKind::Timer : RuleKind::Daily;

    if (rule.kind == RuleKind::Daily) {
      rule.days = entry["days"] | 0b1111111;
      rule.hour = entry["hour"] | 0;
      rule.minute = entry["minute"] | 0;
      if (rule.hour > 23 || rule.minute > 59) {
        error = String(F("rule '")) + rule.name + F("' has an impossible time");
        return false;
      }
      if (rule.days == 0) {
        error = String(F("rule '")) + rule.name + F("' runs on no days");
        return false;
      }
    } else {
      rule.fireAt = static_cast<time_t>(entry["fireAt"] | 0U);
    }

    JsonObjectConst action = entry["action"];
    if (action.isNull()) {
      error = String(F("rule '")) + rule.name + F("' has no action");
      return false;
    }
    if (!ac::deltaFromJson(action, rule.action, error)) return false;
    serializeJson(action, rule.actionJson);

    parsed.push_back(rule);
  }

  rules_ = parsed;
  LOG_I(kTag, "%u schedule(s) loaded", (unsigned)rules_.size());
  return true;
}

// ---------------------------------------------------------------------------

bool Scheduler::load() {
  File file = LittleFS.open(kPath, "r");
  if (!file) return false;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    LOG_W(kTag, "schedules parse error: %s", err.c_str());
    return false;
  }

  String error;
  if (!fromJson(doc.as<JsonArrayConst>(), error)) {
    LOG_W(kTag, "schedules rejected: %s", error.c_str());
    return false;
  }
  return true;
}

bool Scheduler::save() {
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

}  // namespace app
