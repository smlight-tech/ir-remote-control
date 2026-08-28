// UART client on UART0 (the RXD0/TXD0 pins, also wired to the CH340N).
//
// Line based, and deliberately bilingual: a JSON object per line for machines,
// and a handful of bare words for a human with a terminal open. Anything that
// is neither is answered with a one-line hint rather than silence.
//
// While this adapter is enabled the logger stops writing to the port, so the
// framing stays clean.
#pragma once

#include <Arduino.h>

#include "../core/AcState.h"
#include "../core/Source.h"

namespace io {

class UartService {
 public:
  void begin();
  void loop();
  void reconfigure();

  void onStateChanged(const ac::State &state, src::Source source);

  bool enabled() const { return enabled_; }

 private:
  void handleLine(const String &line);
  void handleJson(const String &line);
  void handleWord(const String &line);
  void emit(const JsonDocument &doc);
  void emitState(const char *event, src::Source source);
  void emitError(const String &message);

  static const size_t kMaxLine = 512;

  bool enabled_ = false;
  String buffer_;
};

extern UartService uart;

}  // namespace io
