// Infrared receive and transmit.
//
// Two transmit strategies, picked automatically:
//
//   1. Protocol synthesis (preferred). If IRremoteESP8266 recognises the AC's
//      protocol it can build *any* state from scratch, so the device can send
//      combinations the user never demonstrated. This is what makes a single
//      button press during learning enough for full control.
//
//   2. Raw replay. For protocols the library cannot encode, the device replays
//      a stored capture from CodeStore.
//
// On the receive side the same decoder serves two purposes: capturing codes
// during learning, and watching the user's own remote so the reported state
// stays truthful when somebody walks over and presses a button.
#pragma once

#include <IRac.h>
#include <IRrecv.h>
#include <IRsend.h>

#include <functional>

#include "../core/CommandBus.h"
#include "CodeStore.h"

namespace ir {

// One decoded reception, handed to whoever is listening.
struct Capture {
  decode_type_t protocol = decode_type_t::UNKNOWN;
  uint16_t bits = 0;
  uint64_t value = 0;
  bool repeat = false;
  bool overflow = false;

  const uint16_t *raw = nullptr;  // valid only for the duration of the callback
  uint16_t rawLength = 0;
  uint16_t carrierKhz = 38;

  // True when the frame was understood well enough to yield a full AC state.
  bool decodedState = false;
  ac::State state;

  bool synthesisable() const;
  String protocolName() const;
};

// Return true to consume the capture (learning mode); false to let the normal
// remote-tracking path handle it.
using CaptureHandler = std::function<bool(const Capture &)>;

class IrService : public bus::Transmitter {
 public:
  void begin();
  void loop();

  // Re-open the hardware after the user changes pins in the settings.
  void reconfigure();

  // --- bus::Transmitter ---------------------------------------------------
  bool ready() const override;
  bool sendState(const ac::State &state, bool beep) override;

  // --- direct transmission -------------------------------------------------
  bool sendRaw(const uint16_t *timings, uint16_t length, uint16_t carrierKhz,
               uint8_t repeats = 0);
  bool sendStored(const String &key, uint8_t repeats = 0);
  // Sends `state` using an explicitly named protocol, ignoring the configured
  // one. Used by the "try another protocol" workflow.
  bool sendAs(decode_type_t protocol, int16_t model, const ac::State &state);

  void setCaptureHandler(CaptureHandler handler) { onCapture_ = handler; }
  void clearCaptureHandler() { onCapture_ = nullptr; }

  // Everything the UI needs to explain what the device is hearing.
  const Capture &lastCapture() const { return last_; }
  uint32_t lastCaptureAt() const { return lastCaptureAt_; }
  uint32_t captureCount() const { return captureCount_; }
  bool receiverActive() const { return recv_ != nullptr; }

  uint32_t sendCount() const { return sendCount_; }
  uint32_t lastSendAt() const { return lastSendAt_; }

 private:
  bool openReceiver();
  void closeReceiver();
  void handle(decode_results &results);
  bool synthesise(const ac::State &state, decode_type_t protocol, int16_t model,
                  bool beep);

  IRrecv *recv_ = nullptr;
  IRsend *send_ = nullptr;
  IRac *ac_ = nullptr;

  int8_t rxPin_ = -1;
  int8_t txPin_ = -1;

  CaptureHandler onCapture_ = nullptr;

  Capture last_;
  uint32_t lastCaptureAt_ = 0;
  uint32_t captureCount_ = 0;
  uint32_t sendCount_ = 0;
  uint32_t lastSendAt_ = 0;

  // Our own emissions bounce off walls and back into the receiver; ignore
  // anything arriving immediately after a transmission.
  uint32_t deafUntil_ = 0;
};

extern IrService irService;

}  // namespace ir
