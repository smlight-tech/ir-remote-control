#include "IrService.h"

#include <IRtext.h>
#include <IRutils.h>

#include "../core/Log.h"
#include "../core/Settings.h"
#include "RemoteMap.h"

namespace ir {
namespace {
const char *kTag = "ir";

// 1024 entries is enough for every AC protocol the library knows except a few
// Hitachi variants; overflow is reported so the user is told what happened.
const uint16_t kCaptureBufferSize = 1024;
// Milliseconds of silence that ends a frame. AC frames have long internal gaps.
const uint8_t kCaptureTimeoutMs = 50;
// Ignore captures shorter than this — mostly fluorescent-light noise.
const uint16_t kNoiseThreshold = 12;
// How long to stay deaf after transmitting, so we do not hear ourselves.
const uint32_t kDeafMs = 350;
}  // namespace

IrService irService;

bool Capture::synthesisable() const {
  return protocol != decode_type_t::UNKNOWN && IRac::isProtocolSupported(protocol);
}

String Capture::protocolName() const { return typeToString(protocol, repeat); }

// ---------------------------------------------------------------------------

void IrService::begin() {
  reconfigure();
  codes.begin();
}

void IrService::reconfigure() {
  const cfg::PinSettings &pins = cfg::settings.pins;

  closeReceiver();
  delete ac_;
  delete send_;
  ac_ = nullptr;
  send_ = nullptr;

  rxPin_ = pins.irRx;
  txPin_ = pins.irTx;

  if (txPin_ >= 0) {
    send_ = new IRsend(static_cast<uint16_t>(txPin_), pins.irTxInverted);
    send_->begin();
    ac_ = new IRac(static_cast<uint16_t>(txPin_), pins.irTxInverted);
    LOG_I(kTag, "transmitter on GPIO%d%s", txPin_,
          pins.irTxInverted ? " (inverted)" : "");
    if (txPin_ == 16) {
      // GPIO16 is driven through the RTC domain rather than the normal GPIO
      // block, so each edge costs roughly a microsecond more. IRsend's
      // calibration absorbs most of that, but it is worth saying out loud.
      LOG_W(kTag, "GPIO16 has slower edges than other pins; if the AC ignores "
                  "commands, try the alternate driver on GPIO%d", PIN_IR_TX_ALT);
    }
  } else {
    LOG_W(kTag, "no transmit pin configured");
  }

  openReceiver();
}

bool IrService::openReceiver() {
  if (rxPin_ < 0) {
    LOG_W(kTag, "no receive pin configured");
    return false;
  }
  recv_ = new IRrecv(static_cast<uint16_t>(rxPin_), kCaptureBufferSize,
                     kCaptureTimeoutMs, /*save_buffer=*/false);
  recv_->setUnknownThreshold(kNoiseThreshold);
  recv_->enableIRIn();
  LOG_I(kTag, "receiver on GPIO%d", rxPin_);
  return true;
}

void IrService::closeReceiver() {
  if (recv_ == nullptr) return;
  recv_->disableIRIn();
  delete recv_;
  recv_ = nullptr;
}

// ---------------------------------------------------------------------------

void IrService::loop() {
  if (recv_ == nullptr) return;

  decode_results results;
  if (!recv_->decode(&results)) return;

  if (millis() < deafUntil_) {
    // Almost certainly the echo of our own transmission.
    recv_->resume();
    return;
  }

  handle(results);
  recv_->resume();
}

void IrService::handle(decode_results &results) {
  captureCount_++;
  lastCaptureAt_ = millis();

  Capture capture;
  capture.protocol = results.decode_type;
  capture.bits = results.bits;
  capture.value = results.value;
  capture.repeat = results.repeat;
  capture.overflow = results.overflow;
  capture.carrierKhz = cfg::settings.pins.irCarrierKhz;

  if (results.overflow) {
    LOG_W(kTag, "capture buffer overflowed (%u entries) — the frame was truncated",
          kCaptureBufferSize);
  }

  // A full AC state, if the library can make sense of the frame. `prev` is the
  // current state so that protocols carrying only partial information (a few
  // do) inherit the rest rather than resetting it.
  ac::State previous = bus::commands.state();
  capture.state = previous;
  capture.decodedState =
      IRAcUtils::decodeToState(&results, &capture.state, &previous);

  uint16_t *raw = resultToRawArray(&results);
  if (raw != nullptr) {
    capture.raw = raw;
    capture.rawLength = getCorrectedRawLength(&results);
  }

  LOG_I(kTag, "captured %s, %u bits, %u marks%s", capture.protocolName().c_str(),
        capture.bits, capture.rawLength,
        capture.decodedState ? ", decoded to a full state" : "");

  bool consumed = false;
  if (onCapture_) consumed = onCapture_(capture);

  if (!consumed && capture.decodedState && cfg::settings.ac.trackRemote) {
    // The user pressed their own remote. The AC has already acted on it; we
    // only need to catch up and tell every other client.
    bus::commands.observe(capture.state, src::Source::IrRemote);
  } else if (!consumed) {
    // Not a whole air-conditioner state, so it may be a button bound from some
    // other handset — a television remote, a spare, a wall panel.
    consumed = remotes.handle(capture);
  }

  last_ = capture;
  last_.raw = nullptr;  // the buffer does not outlive this function
  last_.rawLength = capture.rawLength;

  if (raw != nullptr) free(raw);
}

// ---------------------------------------------------------------------------

bool IrService::ready() const {
  if (send_ == nullptr) return false;
  if (cfg::settings.ac.useLearnedCodes) return codes.count() > 0;
  return !cfg::settings.ac.protocol.isEmpty();
}

bool IrService::sendState(const ac::State &state, bool beep) {
  if (send_ == nullptr) return false;

  const cfg::AcSettings &acCfg = cfg::settings.ac;

  if (!acCfg.useLearnedCodes && !acCfg.protocol.isEmpty()) {
    const decode_type_t protocol = strToDecodeType(acCfg.protocol.c_str());
    if (protocol != decode_type_t::UNKNOWN &&
        synthesise(state, protocol, acCfg.model, beep)) {
      return true;
    }
    LOG_W(kTag, "protocol '%s' could not encode this state; trying learned codes",
          acCfg.protocol.c_str());
  }

  RawCode code;
  String usedKey;
  if (!codes.resolve(state, code, usedKey)) {
    LOG_E(kTag, "no way to send %s — neither a usable protocol nor a learned code",
          CodeStore::keyFor(state).c_str());
    return false;
  }

  const bool ok = sendRaw(code.timings, code.length, code.carrierKhz,
                          acCfg.sendRepeats);
  code.release();
  if (ok) LOG_I(kTag, "replayed learned code '%s'", usedKey.c_str());
  return ok;
}

bool IrService::synthesise(const ac::State &state, decode_type_t protocol,
                           int16_t model, bool beep) {
  if (ac_ == nullptr) return false;
  if (!IRac::isProtocolSupported(protocol)) return false;

  ac::State desired = state;
  desired.protocol = protocol;
  desired.model = model;
  desired.beep = beep;
  // `clock` is left unset: writing the wall clock into every frame makes some
  // units chirp and adds nothing.
  desired.clock = -1;

  // The receiver must be off while transmitting: on the ESP8266 both share
  // interrupt time, and we would otherwise decode our own output.
  const bool hadReceiver = recv_ != nullptr;
  if (hadReceiver) recv_->disableIRIn();

  const bool ok = ac_->sendAc(desired, nullptr);

  for (uint8_t i = 0; ok && i < cfg::settings.ac.sendRepeats; i++) {
    delay(40);
    ac_->sendAc(desired, nullptr);
  }

  deafUntil_ = millis() + kDeafMs;
  if (hadReceiver) recv_->enableIRIn();

  if (ok) {
    sendCount_++;
    lastSendAt_ = millis();
    LOG_I(kTag, "sent %s state via protocol synthesis", typeToString(protocol).c_str());
  }
  return ok;
}

bool IrService::sendRaw(const uint16_t *timings, uint16_t length,
                        uint16_t carrierKhz, uint8_t repeats) {
  if (send_ == nullptr || timings == nullptr || length < 2) return false;
  if (carrierKhz < 30 || carrierKhz > 60) carrierKhz = 38;

  const bool hadReceiver = recv_ != nullptr;
  if (hadReceiver) recv_->disableIRIn();

  // sendRaw wants a non-const pointer but does not modify the buffer.
  for (uint8_t i = 0; i <= repeats; i++) {
    if (i > 0) delay(40);
    send_->sendRaw(const_cast<uint16_t *>(timings), length, carrierKhz);
  }

  deafUntil_ = millis() + kDeafMs;
  if (hadReceiver) recv_->enableIRIn();

  sendCount_++;
  lastSendAt_ = millis();
  return true;
}

bool IrService::sendStored(const String &key, uint8_t repeats) {
  RawCode code;
  if (!codes.load(key, code)) {
    LOG_E(kTag, "no stored code '%s'", key.c_str());
    return false;
  }
  const bool ok = sendRaw(code.timings, code.length, code.carrierKhz, repeats);
  code.release();
  return ok;
}

bool IrService::sendAs(decode_type_t protocol, int16_t model,
                       const ac::State &state) {
  return synthesise(state, protocol, model, /*beep=*/true);
}

}  // namespace ir
