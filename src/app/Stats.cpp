#include "Stats.h"

#include <LittleFS.h>
#include <time.h>

#include "../core/Log.h"
#include "../core/Settings.h"

namespace app {
namespace {
const char *kTag = "stats";
const char *kPath = "/stats.json";

// Flash has a finite number of erase cycles, and this is a counter that only
// ever creeps upward — a quarter of an hour is often enough to survive a power
// cut without wearing the filesystem out.
const uint32_t kSaveIntervalMs = 15UL * 60UL * 1000UL;

const time_t kSaneEpoch = 1609459200;   // anything earlier means no NTP yet
}  // namespace

Stats stats;

// ---------------------------------------------------------------------------

void Stats::begin() {
  load();
  LOG_I(kTag, "%lu h of runtime recorded over %lu start(s)",
        (unsigned long)(totalSeconds_ / 3600), (unsigned long)starts_);
}

uint32_t Stats::currentDayStamp() const {
  const time_t value = ::time(nullptr);
  if (value < kSaneEpoch) return 0;      // no clock yet
  struct tm local;
  localtime_r(&value, &local);
  return (local.tm_year + 1900) * 10000UL + (local.tm_mon + 1) * 100UL +
         local.tm_mday;
}

void Stats::rollDay() {
  const uint32_t today = currentDayStamp();
  if (today == 0 || today == dayStamp_) return;
  if (dayStamp_ != 0) {
    LOG_I(kTag, "new day: %lu min yesterday",
          (unsigned long)(todaySeconds_ / 60));
  }
  dayStamp_ = today;
  todaySeconds_ = 0;
  dirty_ = true;
}

// ---------------------------------------------------------------------------

void Stats::onStateChanged(const ac::State &state, src::Source source) {
  (void)source;

  if (state.power && !running_) {
    running_ = true;
    runningSince_ = millis();
    starts_++;
    dirty_ = true;
    LOG_D(kTag, "run started (%lu total)", (unsigned long)starts_);
    return;
  }

  if (!state.power && running_) {
    const uint32_t ran = (millis() - runningSince_) / 1000UL;
    running_ = false;
    totalSeconds_ += ran;
    todaySeconds_ += ran;
    dirty_ = true;
    LOG_I(kTag, "ran for %lu min", (unsigned long)(ran / 60));
    save();
  }
}

void Stats::loop() {
  rollDay();
  if (dirty_ && millis() - lastSaveAt_ > kSaveIntervalMs) save();
}

// ---------------------------------------------------------------------------

uint32_t Stats::runtimeSeconds() const {
  uint32_t total = totalSeconds_;
  if (running_) total += (millis() - runningSince_) / 1000UL;
  return total;
}

uint32_t Stats::runtimeTodaySeconds() const {
  uint32_t total = todaySeconds_;
  if (running_) total += (millis() - runningSince_) / 1000UL;
  return total;
}

float Stats::energyKwh() const {
  const uint16_t watts = cfg::settings.ac.ratedWatts;
  if (watts == 0) return -1.0f;
  return (runtimeSeconds() / 3600.0f) * (watts / 1000.0f);
}

void Stats::toJson(JsonObject out) const {
  out["runtimeSeconds"] = runtimeSeconds();
  out["runtimeTodaySeconds"] = runtimeTodaySeconds();
  out["starts"] = starts_;
  out["running"] = running_;
  out["ratedWatts"] = cfg::settings.ac.ratedWatts;

  const float kwh = energyKwh();
  if (kwh >= 0.0f) {
    out["energyKwh"] = roundf(kwh * 100.0f) / 100.0f;
    out["energyEstimated"] = true;
  }
}

void Stats::reset() {
  totalSeconds_ = 0;
  todaySeconds_ = 0;
  starts_ = 0;
  runningSince_ = millis();
  dirty_ = true;
  save();
  LOG_W(kTag, "statistics reset");
}

// ---------------------------------------------------------------------------

bool Stats::load() {
  File file = LittleFS.open(kPath, "r");
  if (!file) return false;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) return false;

  totalSeconds_ = doc["totalSeconds"] | 0U;
  todaySeconds_ = doc["todaySeconds"] | 0U;
  starts_ = doc["starts"] | 0U;
  dayStamp_ = doc["dayStamp"] | 0U;
  return true;
}

bool Stats::save() {
  lastSaveAt_ = millis();
  dirty_ = false;

  // Fold any run in progress into the stored total, so a power cut loses at
  // most the time since the last write rather than the whole run.
  JsonDocument doc;
  doc["totalSeconds"] = runtimeSeconds();
  doc["todaySeconds"] = runtimeTodaySeconds();
  doc["starts"] = starts_;
  doc["dayStamp"] = dayStamp_;

  File file = LittleFS.open(kPath, "w");
  if (!file) return false;
  serializeJson(doc, file);
  file.close();

  // The stored total now includes the current run, so restart the clock to
  // avoid counting those seconds twice.
  if (running_) {
    totalSeconds_ = doc["totalSeconds"].as<uint32_t>();
    todaySeconds_ = doc["todaySeconds"].as<uint32_t>();
    runningSince_ = millis();
  }
  return true;
}

}  // namespace app
