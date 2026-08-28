// Lightweight logger with an in-RAM ring buffer so the web UI can show recent
// activity without a serial cable attached.
//
// The buffer is a single fixed allocation — no String churn, no heap
// fragmentation, which matters a great deal on an ESP8266 that also has to
// hold a TLS session and an async web server.
#pragma once

#include <Arduino.h>

namespace log_ {

enum class Level : uint8_t {
  Error = 0,
  Warn = 1,
  Info = 2,
  Debug = 3,
};

// Total bytes kept for the in-memory log — roughly 15 lines. This is static
// DRAM on a part that has about 30 kB of it left after the SDK, so it is
// deliberately small; the web UI streams lines over the WebSocket as they
// happen and keeps its own scrollback.
static const size_t kBufferBytes = 1280;

void begin(unsigned long baud);

void setLevel(Level level);
Level level();

// When false, nothing is written to the hardware UART. Used when the UART
// client adapter owns the port and stray text would corrupt its framing.
void setSerialEnabled(bool enabled);
bool serialEnabled();

void write(Level level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Copies the ring buffer out as newline-separated records, newest last.
// `sinceSeq` allows the UI to poll incrementally; pass 0 for everything.
// Returns the sequence number of the last record copied.
uint32_t dump(String &out, uint32_t sinceSeq = 0);

uint32_t sequence();
void clear();

}  // namespace log_

#define LOG_E(tag, ...) ::log_::write(::log_::Level::Error, tag, __VA_ARGS__)
#define LOG_W(tag, ...) ::log_::write(::log_::Level::Warn, tag, __VA_ARGS__)
#define LOG_I(tag, ...) ::log_::write(::log_::Level::Info, tag, __VA_ARGS__)

#if defined(SLWF12_DEBUG)
#define LOG_D(tag, ...) ::log_::write(::log_::Level::Debug, tag, __VA_ARGS__)
#else
// Compiled out entirely in release builds, but the arguments still have to
// type-check, so a mistake in a debug log cannot break the release build.
#define LOG_D(tag, ...)                     \
  do {                                      \
    if (false) ::log_::write(::log_::Level::Debug, tag, __VA_ARGS__); \
  } while (0)
#endif
