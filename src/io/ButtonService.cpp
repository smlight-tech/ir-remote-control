#include "ButtonService.h"

#include "../core/CommandBus.h"
#include "../core/Log.h"
#include "../core/Settings.h"
#include "../ir/Learning.h"
#include "../net/OtaService.h"
#include "../net/WifiService.h"

namespace io {
namespace {
const char *kTag = "button";
}

ButtonService button;

// ---------------------------------------------------------------------------

void ButtonService::begin() {
  pin_ = cfg::settings.pins.button;
  if (pin_ < 0) {
    LOG_I(kTag, "no button configured");
    return;
  }
  // GPIO0 already has a hardware pull-up; INPUT is enough and avoids fighting
  // it. Other pins get the internal pull-up so the wiring stays simple.
  pinMode(pin_, pin_ == 0 ? INPUT : INPUT_PULLUP);
  lastRawLevel_ = digitalRead(pin_) != LOW;
  LOG_I(kTag, "button on GPIO%d", pin_);
}

uint32_t ButtonService::heldMs() const {
  return pressed_ ? millis() - pressedAt_ : 0;
}

void ButtonService::loop() {
  if (pin_ < 0) return;

  const uint32_t now = millis();
  const bool raw = digitalRead(pin_) != LOW;   // true = released

  if (raw != lastRawLevel_) {
    lastRawLevel_ = raw;
    lastChangeAt_ = now;
    return;                                    // wait out the bounce
  }
  if (now - lastChangeAt_ < kDebounceMs) return;

  const bool nowPressed = !raw;

  if (nowPressed && !pressed_) {
    pressed_ = true;
    pressedAt_ = now;
    holdFired_ = false;
  } else if (!nowPressed && pressed_) {
    pressed_ = false;
    const uint32_t held = now - pressedAt_;
    releasedAt_ = now;
    if (!holdFired_ && held < kPortalHoldMs) clickCount_++;
  }

  if (pressed_ && !holdFired_) {
    const uint32_t held = now - pressedAt_;
    if (held >= kFactoryHoldMs) {
      holdFired_ = true;
      clickCount_ = 0;
      onHold(held);
    } else if (held >= kPortalHoldMs && held < kPortalHoldMs + kDebounceMs * 2) {
      // Fire the portal gesture as soon as the threshold is crossed so the
      // user gets feedback without having to guess how long ten seconds is.
      onHold(held);
      // Deliberately not setting holdFired_: keeping the button down for the
      // full ten seconds must still reach the factory reset.
    }
  }

  if (clickCount_ > 0 && !pressed_ && now - releasedAt_ > kDoubleClickMs) {
    const uint8_t clicks = clickCount_;
    clickCount_ = 0;
    if (clicks == 1) onClick();
    else onDoubleClick();
  }
}

// ---------------------------------------------------------------------------

void ButtonService::onClick() {
  LOG_I(kTag, "click: toggling power");
  ac::Delta delta;
  delta.hasPower = true;
  delta.power = !bus::commands.state().power;

  const bus::Outcome outcome = bus::commands.apply(delta, src::Source::System);
  if (!outcome.ok()) LOG_W(kTag, "%s", outcome.message.c_str());
}

void ButtonService::onDoubleClick() {
  LOG_I(kTag, "double click: starting the learning wizard");
  String error;
  if (!learn::wizard.startIdentify(error)) LOG_W(kTag, "%s", error.c_str());
}

void ButtonService::onHold(uint32_t held) {
  if (held >= kFactoryHoldMs) {
    LOG_W(kTag, "held for %lus: factory reset", (unsigned long)(held / 1000));
    cfg::settings.factoryReset();
    net::ota.scheduleRestart(500);
    return;
  }
  LOG_I(kTag, "held for %lus: raising the setup access point",
        (unsigned long)(held / 1000));
  net::wifi.startPortal();
}

}  // namespace io
