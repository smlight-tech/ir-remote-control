// SLWF-12 AC Bridge — firmware entry point.
//
// Copyright (C) 2026 SMLIGHT and contributors.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version. It is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details: <https://www.gnu.org/licenses/>.
//
// The licence covers the whole repository — the firmware, the web interface
// and the tools — not only this file.
//
// The shape of this file is the architecture: everything below is either a
// service that runs in the cooperative loop, or a client adapter subscribed to
// the one command bus. No adapter talks to another; they all talk to the bus.
//
//   IR receiver ─┐                              ┌─► IR transmitter
//   MQTT ────────┤                              ├─► MQTT state
//   Telegram ────┼──►  bus::CommandBus  ────────┼─► WebSocket push
//   Web / API ───┤     (gates, clamps,          ├─► Telegram notice
//   UART ────────┤      transmits, notifies)    └─► UART event
//   Button ──────┤
//   Schedules ───┘
//
// Adding a new client means writing an adapter that calls `commands.apply()`
// and, if it needs to react to changes, subscribing here.

#include <Arduino.h>

#include "app/Automations.h"
#include "app/DeviceTypes.h"
#include "app/PeerClient.h"
#include "app/Peers.h"
#include "app/Scenes.h"
#include "app/Scheduler.h"
#include "app/Stats.h"
#include "core/CommandBus.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "io/ButtonService.h"
#include "io/UartService.h"
#include "ir/IrService.h"
#include "ir/Learning.h"
#include "ir/RemoteMap.h"
#include "net/ModbusService.h"
#include "net/MqttService.h"
#include "net/OtaService.h"
#include "net/TelegramService.h"
#include "net/WebhookService.h"
#include "net/WebService.h"
#include "net/WifiService.h"
#include "generated/version.h"

namespace {
const char *kTag = "main";

// Heap below this is a warning sign on an ESP8266 running TLS.
// Free heap is not the number that matters — the async web server needs a
// *contiguous* block to build a response in, and a fragmented heap can have
// plenty free and none of it usable. So the warning watches both, and the
// thresholds are set where trouble actually starts rather than where a full
// device happens to sit.
const uint32_t kLowHeapBytes = 6000;
const uint32_t kLowBlockBytes = 3000;
uint32_t lastHealthAt = 0;
learn::Phase lastLearnPhase = learn::Phase::Idle;

void banner() {
  Serial.println();
  Serial.println(F("SLWF-12 AC Bridge"));
  Serial.printf("  firmware  %s (%s, %s)\r\n", FW_VERSION, FW_COMMIT,
                FW_BUILD_DATE);
  Serial.printf("  chip      %06x, %u KB flash, %u KB free heap\r\n",
                ESP.getChipId(), ESP.getFlashChipRealSize() / 1024,
                ESP.getFreeHeap() / 1024);
  Serial.printf("  reset     %s\r\n", ESP.getResetReason().c_str());
  Serial.println();
}

// Every client that needs to know about a change is wired up here, in one
// place, so the fan-out is obvious rather than scattered across constructors.
void wireSubscribers() {
  bus::commands.subscribe(
      [](const ac::State &state, src::Source source, bool transmitted) {
        (void)transmitted;
        // Statistics first: the runtime counters must see the transition
        // before anything publishes a figure derived from them.
        app::stats.onStateChanged(state, source);
        net::web.broadcastState(source);
        net::mqtt.publishState(/*force=*/true);
        net::telegram.onStateChanged(state, source);
        net::webhooks.onStateChanged(state, source);
        net::modbusService.onStateChanged(state, source);
        io::uart.onStateChanged(state, source);
      });
}

void healthCheck() {
  if (millis() - lastHealthAt < 30000) return;
  lastHealthAt = millis();

  const uint32_t heap = ESP.getFreeHeap();
  const uint32_t largest = ESP.getMaxFreeBlockSize();
  if (heap < kLowHeapBytes || largest < kLowBlockBytes) {
    LOG_W(kTag, "free heap down to %u bytes (largest block %u, %u%% fragmented)",
          heap, ESP.getMaxFreeBlockSize(), ESP.getHeapFragmentation());
  } else {
    LOG_D(kTag, "heap %u, clients %lu, uptime %lus", heap,
          (unsigned long)net::web.clientCount(), millis() / 1000UL);
  }
}

// The learning wizard has no way to reach the clients itself; poll its phase
// and push an update whenever it moves.
void pumpLearningNotifications() {
  const learn::Phase phase = learn::wizard.phase();
  if (phase == lastLearnPhase) return;
  lastLearnPhase = phase;
  net::web.broadcastLearning();
  net::telegram.notifyLearning();
}

// Heap accounting for start-up. Free RAM is the scarcest thing on this chip
// and the easiest to spend without noticing, so every subsystem reports what
// it cost. Answering "where did the heap go" by reading code is guesswork;
// this makes the boot log say it.
uint32_t heapMark = 0;

void heapAfter(const char *what) {
  const uint32_t now = ESP.getFreeHeap();
  const int32_t spent = static_cast<int32_t>(heapMark) - static_cast<int32_t>(now);
  LOG_I(kTag, "heap: %-14s %+6ld -> %u free", what, (long)-spent, now);
  heapMark = now;
}

}  // namespace

// ---------------------------------------------------------------------------

void setup() {
  log_::begin(115200);
  banner();

  heapMark = ESP.getFreeHeap();
  LOG_I(kTag, "heap: at boot        %u free", heapMark);

  cfg::settings.begin();
  heapAfter("settings");
  log_::setLevel(static_cast<log_::Level>(cfg::settings.log.level));
  log_::setSerialEnabled(cfg::settings.log.serial && !cfg::settings.uart.enabled);

  LOG_I(kTag, "device '%s' (%s)", cfg::settings.device.name.c_str(),
        cfg::settings.chipId().c_str());

  // Hardware first: the IR service is the bus's transmitter, so it has to
  // exist before the bus is started.
  ir::irService.begin();
  heapAfter("ir");
  bus::commands.begin(&ir::irService);
  heapAfter("command bus");
  learn::wizard.begin();
  heapAfter("wizard");
  ir::remotes.begin();
  heapAfter("remotes");
  io::button.begin();
  heapAfter("button");
  io::uart.begin();
  heapAfter("uart");

  // Then the network stack.
  net::wifi.begin();
  heapAfter("wifi");
  net::web.begin();
  heapAfter("web server");
  net::ota.begin();
  heapAfter("ota");
  net::mqtt.begin();
  heapAfter("mqtt");
  net::telegram.begin();
  heapAfter("telegram");
  net::webhooks.begin();
  heapAfter("webhooks");
  net::modbusService.begin();
  heapAfter("modbus");
  app::scheduler.begin();
  heapAfter("scheduler");
  app::peers.begin();
  heapAfter("peers");
  app::peerClient.begin();
  heapAfter("peer client");
  app::automations.begin();
  heapAfter("automations");
  app::scenes.begin();
  heapAfter("scenes");
  app::stats.begin();
  heapAfter("stats");

  wireSubscribers();

  if (!ir::irService.ready()) {
    LOG_W(kTag, "no air conditioner configured yet — open the web interface "
                "and run the learning wizard");
  }
  LOG_I(kTag, "ready, %u bytes of heap free", ESP.getFreeHeap());
}

void loop() {
  // Ordered by latency sensitivity: the IR decoder first, because a missed
  // frame cannot be recovered, and the housekeeping last.
  ir::irService.loop();
  io::button.loop();
  io::uart.loop();

  net::wifi.loop();
  net::web.loop();
  net::mqtt.loop();
  net::telegram.loop();
  net::webhooks.loop();
  net::modbusService.loop();
  net::ota.loop();

  learn::wizard.loop();
  app::scheduler.loop();
  app::stats.loop();
  app::peerClient.loop();
  app::automations.loop();
  bus::commands.loop();
  cfg::settings.loop();

  pumpLearningNotifications();
  healthCheck();

  // Hand the SDK its time slice. Without this the watchdog eventually objects.
  yield();
}
