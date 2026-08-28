// Every command that reaches the AC carries the client it came from.
//
// Two things depend on this: the user can enable/disable each client
// independently, and the IR sender must *not* re-transmit a state that arrived
// from the physical remote (the AC already acted on it).
#pragma once

#include <Arduino.h>

namespace src {

enum class Source : uint8_t {
  Web = 0,       // built-in web UI
  Api,           // raw HTTP/REST callers
  Mqtt,          // MQTT, including Home Assistant
  Telegram,      // Telegram bot
  IrRemote,      // the AC's own IR remote, observed by the receiver
  Remote,        // some other remote — a TV handset, a spare — with bound buttons
  Uart,          // host connected to UART0
  Modbus,        // a PLC or building-management system writing registers
  Schedule,      // built-in timers and schedules
  Automation,    // an IF/THEN rule firing
  System,        // firmware itself (boot restore, learning) — never gated
  Count
};

static const uint8_t kCount = static_cast<uint8_t>(Source::Count);

const char *name(Source s);
bool parse(const char *text, Source &out);

// Whether a state change from this source should be re-emitted over IR.
inline bool shouldTransmit(Source s) { return s != Source::IrRemote; }

// Sources the user is allowed to toggle. `System` is always on.
inline bool isGateable(Source s) { return s != Source::System; }

}  // namespace src
