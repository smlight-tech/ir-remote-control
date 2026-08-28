// How much the air conditioner actually runs.
//
// Useful on its own ("did I leave it on all week?"), and the reason the device
// can offer a Home Assistant energy sensor: multiply running time by the
// nameplate wattage and you get an estimate good enough to see trends, which
// is more than most IR bridges give you.
//
// It is an estimate and is presented as one. A real inverter unit modulates
// its draw constantly, so this is running-time × rated power, not metering —
// and when the rated power is unset, no energy figure is published at all
// rather than a made-up one.
#pragma once

#include <ArduinoJson.h>

#include "../core/AcState.h"
#include "../core/Source.h"

namespace app {

class Stats {
 public:
  void begin();
  void loop();

  // Subscribed to the command bus; the only thing that moves the counters.
  void onStateChanged(const ac::State &state, src::Source source);

  uint32_t runtimeSeconds() const;
  uint32_t runtimeTodaySeconds() const;
  uint32_t starts() const { return starts_; }
  // Negative when the rated power is unknown.
  float energyKwh() const;

  void toJson(JsonObject out) const;
  void reset();

 private:
  bool load();
  bool save();
  void rollDay();
  uint32_t currentDayStamp() const;

  uint32_t totalSeconds_ = 0;     // accumulated, excluding any run in progress
  uint32_t todaySeconds_ = 0;
  uint32_t starts_ = 0;
  uint32_t dayStamp_ = 0;         // yyyymmdd of what todaySeconds_ refers to

  bool running_ = false;
  uint32_t runningSince_ = 0;     // millis()

  uint32_t lastSaveAt_ = 0;
  bool dirty_ = false;
};

extern Stats stats;

}  // namespace app
