#include "ModbusService.h"

#include <ModbusIP_ESP8266.h>

#include "../app/Stats.h"
#include "../core/CommandBus.h"
#include "../core/Log.h"
#include "../core/Settings.h"

namespace net {
namespace {
const char *kTag = "modbus";

ModbusIP mb;

// See the register map in the header.
const uint16_t kRegPower = 0;
const uint16_t kRegMode = 1;
const uint16_t kRegTemp = 2;
const uint16_t kRegFan = 3;
const uint16_t kRegSwing = 4;
const uint16_t kRegCommand = 5;
const uint16_t kRegRevision = 10;
const uint16_t kRegRuntime = 11;
const uint16_t kRegStarts = 12;

// Deliberately a small, stable table rather than the library's enum order:
// a register map is a published interface and must not shift when an upstream
// enum gains a member.
const stdAc::opmode_t kModes[] = {
    stdAc::opmode_t::kAuto, stdAc::opmode_t::kCool, stdAc::opmode_t::kHeat,
    stdAc::opmode_t::kDry,  stdAc::opmode_t::kFan};
const uint16_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

const stdAc::fanspeed_t kFans[] = {
    stdAc::fanspeed_t::kAuto,   stdAc::fanspeed_t::kMin,
    stdAc::fanspeed_t::kLow,    stdAc::fanspeed_t::kMedium,
    stdAc::fanspeed_t::kMediumHigh, stdAc::fanspeed_t::kHigh,
    stdAc::fanspeed_t::kMax};
const uint16_t kFanCount = sizeof(kFans) / sizeof(kFans[0]);

uint16_t indexOfMode(stdAc::opmode_t mode) {
  for (uint16_t i = 0; i < kModeCount; i++)
    if (kModes[i] == mode) return i;
  return 0;
}

uint16_t indexOfFan(stdAc::fanspeed_t fan) {
  for (uint16_t i = 0; i < kFanCount; i++)
    if (kFans[i] == fan) return i;
  return 0;
}

void applyFromPlc(const ac::Delta &delta) {
  const bus::Outcome outcome = bus::commands.apply(delta, src::Source::Modbus);
  if (!outcome.ok() && outcome.result != bus::Result::Deferred) {
    LOG_W(kTag, "PLC write not applied: %s", outcome.message.c_str());
  }
}

}  // namespace

ModbusService modbusService;

// ---------------------------------------------------------------------------

void ModbusService::begin() { reconfigure(); }

void ModbusService::reconfigure() {
  if (!cfg::settings.modbus.enabled) {
    LOG_I(kTag, "disabled");
    return;
  }
  if (started_) return;

  mb.server(cfg::settings.modbus.port);

  const ac::State &state = bus::commands.state();
  mb.addHreg(kRegPower, state.power ? 1 : 0);
  mb.addHreg(kRegMode, indexOfMode(state.mode));
  mb.addHreg(kRegTemp, static_cast<uint16_t>(lroundf(state.degrees * 10.0f)));
  mb.addHreg(kRegFan, indexOfFan(state.fanspeed));
  mb.addHreg(kRegSwing, state.swingv == stdAc::swingv_t::kOff ? 0 : 1);
  mb.addHreg(kRegCommand, 0);
  mb.addHreg(kRegRevision, 0);
  mb.addHreg(kRegRuntime, 0);
  mb.addHreg(kRegStarts, 0);

  // The callback returns the value actually stored. Returning the *current*
  // state rather than what was written would fight the PLC; instead the write
  // is accepted, applied through the bus, and publish() corrects the register
  // afterwards if the bus clamped or refused it.
  mb.onSetHreg(kRegPower, [this](TRegister *reg, uint16_t value) -> uint16_t {
    if (publishing_) return value;
    writes_++;
    ac::Delta delta;
    delta.hasPower = true;
    delta.power = value != 0;
    applyFromPlc(delta);
    return reg->value;
  });

  mb.onSetHreg(kRegMode, [this](TRegister *reg, uint16_t value) -> uint16_t {
    if (publishing_) return value;
    if (value >= kModeCount) return reg->value;
    writes_++;
    ac::Delta delta;
    delta.hasMode = true;
    delta.mode = kModes[value];
    delta.hasPower = true;
    delta.power = true;
    applyFromPlc(delta);
    return reg->value;
  });

  mb.onSetHreg(kRegTemp, [this](TRegister *reg, uint16_t value) -> uint16_t {
    if (publishing_) return value;
    writes_++;
    ac::Delta delta;
    delta.hasDegrees = true;
    delta.degrees = value / 10.0f;
    applyFromPlc(delta);
    return reg->value;
  });

  mb.onSetHreg(kRegFan, [this](TRegister *reg, uint16_t value) -> uint16_t {
    if (publishing_) return value;
    if (value >= kFanCount) return reg->value;
    writes_++;
    ac::Delta delta;
    delta.hasFan = true;
    delta.fan = kFans[value];
    applyFromPlc(delta);
    return reg->value;
  });

  mb.onSetHreg(kRegSwing, [this](TRegister *reg, uint16_t value) -> uint16_t {
    if (publishing_) return value;
    writes_++;
    ac::Delta delta;
    delta.hasSwingV = true;
    delta.swingv = value != 0 ? stdAc::swingv_t::kAuto : stdAc::swingv_t::kOff;
    applyFromPlc(delta);
    return reg->value;
  });

  mb.onSetHreg(kRegCommand, [this](TRegister *reg, uint16_t value) -> uint16_t {
    (void)reg;
    if (publishing_) return value;
    if (value == 1) {
      writes_++;
      bus::commands.resend(src::Source::Modbus);
    }
    return 0;   // self-clearing, so a PLC can write 1 again next time
  });

  started_ = true;
  publish(bus::commands.state());
  LOG_I(kTag, "Modbus TCP server listening on port %u",
        cfg::settings.modbus.port);
}

void ModbusService::loop() {
  if (!started_) return;
  mb.task();
}

// ---------------------------------------------------------------------------

void ModbusService::onStateChanged(const ac::State &state, src::Source source) {
  (void)source;
  if (!started_) return;
  publish(state);
}

void ModbusService::publish(const ac::State &state) {
  publishing_ = true;

  mb.Hreg(kRegPower, state.power ? 1 : 0);
  mb.Hreg(kRegMode, indexOfMode(state.mode));
  mb.Hreg(kRegTemp, static_cast<uint16_t>(lroundf(state.degrees * 10.0f)));
  mb.Hreg(kRegFan, indexOfFan(state.fanspeed));
  mb.Hreg(kRegSwing, state.swingv == stdAc::swingv_t::kOff ? 0 : 1);
  mb.Hreg(kRegRevision,
          static_cast<uint16_t>(bus::commands.revision() & 0xFFFF));
  mb.Hreg(kRegRuntime,
          static_cast<uint16_t>(app::stats.runtimeSeconds() / 3600UL));
  mb.Hreg(kRegStarts, static_cast<uint16_t>(app::stats.starts() & 0xFFFF));

  publishing_ = false;
}

// ---------------------------------------------------------------------------

void ModbusService::statusJson(JsonObject out) const {
  out["enabled"] = cfg::settings.modbus.enabled;
  out["running"] = started_;
  out["port"] = cfg::settings.modbus.port;
  out["writes"] = writes_;
}

}  // namespace net
