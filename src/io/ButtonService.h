// The single hardware button on GPIO0 (U6 on the SLWF-12).
//
// GPIO0 is also the bootloader strap, so it is held high by R7 and pulled low
// by the button. It is safe to read at runtime; it just must not be low at
// reset unless you want the flashing bootloader.
//
// Gestures, deliberately escalating in destructiveness so nothing dangerous
// happens by accident:
//
//   click          toggle the air conditioner on or off
//   double click   start the "identify my remote" learning wizard
//   hold 3 s       raise the Wi-Fi setup access point
//   hold 10 s      factory reset, then restart
#pragma once

#include <Arduino.h>

namespace io {

class ButtonService {
 public:
  void begin();
  void loop();

  bool pressed() const { return pressed_; }
  uint32_t heldMs() const;

 private:
  void onClick();
  void onDoubleClick();
  void onHold(uint32_t heldMs);

  static const uint32_t kDebounceMs = 40;
  static const uint32_t kDoubleClickMs = 400;
  static const uint32_t kPortalHoldMs = 3000;
  static const uint32_t kFactoryHoldMs = 10000;

  int8_t pin_ = -1;
  bool pressed_ = false;
  bool holdFired_ = false;
  uint8_t clickCount_ = 0;

  uint32_t pressedAt_ = 0;
  uint32_t releasedAt_ = 0;
  uint32_t lastChangeAt_ = 0;
  bool lastRawLevel_ = true;
};

extern ButtonService button;

}  // namespace io
