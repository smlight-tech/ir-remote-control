#include "Learning.h"

#include <IRutils.h>

#include <algorithm>

#include "../core/Log.h"
#include "../core/Settings.h"
#include "RemoteMap.h"

namespace learn {
namespace {
const char *kTag = "learn";

// A wizard left half-finished should not hold the IR receiver hostage.
const uint32_t kStepTimeoutMs = 5UL * 60UL * 1000UL;
// Beyond this a "teach every combination" plan stops being a workflow and
// starts being a punishment.
const uint16_t kMaxTargets = 96;

const char *phaseName(Phase p) {
  switch (p) {
    case Phase::Idle:     return "idle";
    case Phase::Identify: return "identify";
    case Phase::Confirm:  return "confirm";
    case Phase::Sweep:    return "sweep";
    case Phase::Record:   return "record";
    case Phase::Bind:     return "bind";
    case Phase::Done:     return "done";
    case Phase::Failed:   return "failed";
  }
  return "?";
}
}  // namespace

Wizard wizard;

// ---------------------------------------------------------------------------

void Wizard::begin() {
  ir::irService.setCaptureHandler(
      [this](const ir::Capture &c) { return this->onCapture(c); });
}

void Wizard::loop() {
  if (!active()) return;
  if (millis() - stepStartedAt_ > kStepTimeoutMs) {
    LOG_W(kTag, "no activity for five minutes, cancelling");
    finish(false, F("learn.timeout"));
  }
}

void Wizard::enter(Phase phase, const String &promptKey) {
  phase_ = phase;
  promptKey_ = promptKey;
  stepStartedAt_ = millis();
}

void Wizard::finish(bool success, const String &messageKey) {
  phase_ = success ? Phase::Done : Phase::Failed;
  messageKey_ = messageKey;
  promptKey_ = "";
  LOG_I(kTag, "wizard finished: %s (%s)", success ? "success" : "failed",
        messageKey.c_str());
}

void Wizard::cancel() {
  if (phase_ == Phase::Idle) return;
  LOG_I(kTag, "wizard cancelled");
  phase_ = Phase::Idle;
  promptKey_ = "";
  messageKey_ = "";
  targets_.clear();
  sweep_.clear();
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

bool Wizard::startIdentify(String &error) {
  if (!ir::irService.receiverActive()) {
    error = F("the IR receiver is not configured");
    return false;
  }
  cancel();
  startedAt_ = millis();
  candidate_ = decode_type_t::UNKNOWN;
  candidateModel_ = -1;
  enter(Phase::Identify, F("learn.press_any_button"));
  LOG_I(kTag, "identify: waiting for a remote press");
  return true;
}

bool Wizard::startSweep(String &error) {
  if (!ir::irService.ready() && cfg::settings.pins.irTx < 0) {
    error = F("the IR transmitter is not configured");
    return false;
  }
  cancel();
  startedAt_ = millis();
  buildSweepList();
  if (sweep_.empty()) {
    error = F("no candidate protocols available");
    return false;
  }
  sweepIndex_ = 0;
  enter(Phase::Sweep, F("learn.sweep_watch"));
  sendSweepProbe();
  LOG_I(kTag, "sweep: %u candidate protocols", (unsigned)sweep_.size());
  return true;
}

bool Wizard::startRecord(const Plan &plan, String &error) {
  if (!ir::irService.receiverActive()) {
    error = F("the IR receiver is not configured");
    return false;
  }
  cancel();
  buildTargets(plan);
  if (targets_.empty()) {
    error = F("the plan does not contain anything to record");
    return false;
  }
  startedAt_ = millis();
  targetIndex_ = 0;
  recorded_ = 0;
  skipped_ = 0;
  enter(Phase::Record, F("learn.set_remote_to"));
  LOG_I(kTag, "record: %u target(s)", (unsigned)targets_.size());
  return true;
}

bool Wizard::startRecordKeys(const std::vector<String> &keys, String &error) {
  if (!ir::irService.receiverActive()) {
    error = F("the IR receiver is not configured");
    return false;
  }
  if (keys.empty()) {
    error = F("no keys given");
    return false;
  }
  cancel();
  targets_ = keys;
  if (targets_.size() > kMaxTargets) targets_.resize(kMaxTargets);
  startedAt_ = millis();
  targetIndex_ = 0;
  recorded_ = 0;
  skipped_ = 0;
  enter(Phase::Record, F("learn.set_remote_to"));
  return true;
}

bool Wizard::startBind(const String &action, const String &label,
                       const String &argument, String &error) {
  if (!ir::irService.receiverActive()) {
    error = F("the IR receiver is not configured");
    return false;
  }
  ir::RemoteAction parsed;
  if (!ir::RemoteMap::parseAction(action.c_str(), parsed)) {
    error = String(F("unknown action: ")) + action;
    return false;
  }

  cancel();
  bindAction_ = action;
  bindLabel_ = label;
  bindArgument_ = argument;
  startedAt_ = millis();
  enter(Phase::Bind, F("learn.press_button_to_bind"));
  LOG_I(kTag, "waiting for a button to bind to %s", action.c_str());
  return true;
}

// ---------------------------------------------------------------------------
// Plans
// ---------------------------------------------------------------------------

Plan Wizard::defaultPlan() {
  Plan plan;
  plan.modes = {stdAc::opmode_t::kCool, stdAc::opmode_t::kHeat};
  plan.fans = {stdAc::fanspeed_t::kAuto};
  plan.minTemp = 18.0f;
  plan.maxTemp = 26.0f;
  plan.step = 1.0f;
  plan.includeOff = true;
  return plan;
}

bool Wizard::planFromJson(JsonObjectConst in, Plan &out, String &error) {
  out = Plan();

  JsonArrayConst modes = in["modes"];
  if (!modes.isNull()) {
    out.modes.clear();
    for (JsonVariantConst v : modes) {
      stdAc::opmode_t mode;
      if (!ac::parseMode(v.as<const char *>(), mode)) {
        error = String(F("unknown mode in plan: ")) + (v.as<const char *>() ?: "?");
        return false;
      }
      out.modes.push_back(mode);
    }
  } else {
    out.modes = {stdAc::opmode_t::kCool};
  }

  JsonArrayConst fans = in["fans"];
  if (!fans.isNull()) {
    out.fans.clear();
    for (JsonVariantConst v : fans) {
      stdAc::fanspeed_t fan;
      if (!ac::parseFan(v.as<const char *>(), fan)) {
        error = String(F("unknown fan speed in plan: ")) + (v.as<const char *>() ?: "?");
        return false;
      }
      out.fans.push_back(fan);
    }
  } else {
    out.fans = {stdAc::fanspeed_t::kAuto};
  }

  if (!in["minTemp"].isNull()) out.minTemp = in["minTemp"].as<float>();
  if (!in["maxTemp"].isNull()) out.maxTemp = in["maxTemp"].as<float>();
  if (!in["step"].isNull()) out.step = in["step"].as<float>();
  if (!in["includeOff"].isNull()) out.includeOff = in["includeOff"].as<bool>();

  if (out.step <= 0.0f) out.step = 1.0f;
  if (out.maxTemp < out.minTemp) {
    error = F("maximum temperature is below the minimum");
    return false;
  }

  JsonArrayConst buttons = in["buttons"];
  if (!buttons.isNull()) {
    for (JsonVariantConst v : buttons) {
      const char *name = v.as<const char *>();
      if (name != nullptr) out.buttons.push_back(String(name));
    }
  }
  return true;
}

void Wizard::buildTargets(const Plan &plan) {
  targets_.clear();

  if (plan.includeOff) targets_.push_back(F("off"));

  for (stdAc::opmode_t mode : plan.modes) {
    for (float t = plan.minTemp; t <= plan.maxTemp + 0.001f; t += plan.step) {
      for (stdAc::fanspeed_t fan : plan.fans) {
        if (targets_.size() >= kMaxTargets) {
          LOG_W(kTag, "plan truncated at %u targets", kMaxTargets);
          return;
        }
        ac::State state = ac::defaultState();
        state.power = true;
        state.mode = mode;
        state.degrees = t;
        state.fanspeed = fan;
        targets_.push_back(ir::CodeStore::keyFor(state));
      }
    }
  }

  for (const String &button : plan.buttons) {
    if (targets_.size() >= kMaxTargets) return;
    targets_.push_back(ir::CodeStore::buttonKey(button));
  }
}

// ---------------------------------------------------------------------------
// Capture handling
// ---------------------------------------------------------------------------

bool Wizard::onCapture(const ir::Capture &capture) {
  if (!active()) return false;
  if (capture.repeat) return true;  // swallow repeats, but do not act on them

  switch (phase_) {
    case Phase::Identify: {
      if (capture.synthesisable()) {
        candidate_ = capture.protocol;
        candidateModel_ = capture.state.model;
        LOG_I(kTag, "identified %s", typeToString(candidate_).c_str());

        // Send the state we just heard straight back. If the AC reacts, the
        // protocol is right — and the AC is left exactly as the user set it.
        ac::State probe = capture.state;
        probe.power = true;
        ir::irService.sendAs(candidate_, candidateModel_, probe);
        enter(Phase::Confirm, F("learn.did_it_react"));
      } else {
        // Nothing usable. Keep the raw frame as the "off"/first code so the
        // effort is not wasted, and offer the sweep.
        LOG_W(kTag, "protocol %s cannot be synthesised",
              capture.protocolName().c_str());
        if (capture.raw != nullptr && capture.rawLength > 1) {
          ir::codes.store(F("btn_first_capture"), capture.raw, capture.rawLength,
                          capture.carrierKhz);
        }
        candidate_ = capture.protocol;
        enter(Phase::Confirm, F("learn.not_recognised"));
      }
      return true;
    }

    case Phase::Record: {
      if (targetIndex_ >= targets_.size()) return true;
      if (capture.raw == nullptr || capture.rawLength < 2) {
        LOG_W(kTag, "capture too short to store, waiting for another press");
        return true;
      }
      const String &key = targets_[targetIndex_];
      if (ir::codes.store(key, capture.raw, capture.rawLength,
                          capture.carrierKhz)) {
        recorded_++;
      }
      advanceTarget();
      return true;
    }

    case Phase::Bind: {
      ir::RemoteBinding binding;
      binding.label = bindLabel_;
      binding.protocol = capture.protocol;
      binding.value = capture.value;
      binding.bits = capture.bits;
      binding.argument = bindArgument_;
      ir::RemoteMap::parseAction(bindAction_.c_str(), binding.action);

      if (capture.decodedState) {
        // Binding a button that is also a complete air-conditioner command
        // would break remote tracking: the same frame cannot both mean "adopt
        // this state" and "do this action".
        finish(false, F("learn.bind_is_ac_remote"));
        return true;
      }

      String error;
      if (!ir::remotes.bind(binding, error)) {
        LOG_W(kTag, "bind failed: %s", error.c_str());
        finish(false, F("learn.bind_failed"));
        return true;
      }
      finish(true, F("learn.bind_done"));
      return true;
    }

    default:
      // In Confirm and Sweep the user is pressing buttons on the *device's*
      // behalf, not the remote's; ignore stray traffic.
      return true;
  }
}

void Wizard::advanceTarget() {
  targetIndex_++;
  if (targetIndex_ >= targets_.size()) {
    cfg::settings.ac.useLearnedCodes = true;
    cfg::settings.touch();
    finish(true, F("learn.recording_complete"));
    return;
  }
  enter(Phase::Record, F("learn.set_remote_to"));
}

// ---------------------------------------------------------------------------
// Confirmation and sweeping
// ---------------------------------------------------------------------------

void Wizard::confirm(bool worked) {
  if (phase_ == Phase::Confirm) {
    if (worked && candidate_ != decode_type_t::UNKNOWN &&
        IRac::isProtocolSupported(candidate_)) {
      adoptProtocol(candidate_, candidateModel_);
      finish(true, F("learn.protocol_adopted"));
    } else {
      // Either the test did nothing, or the frame was never decodable. Offer
      // the sweep next; the client decides whether to start it.
      finish(false, F("learn.try_sweep_or_record"));
    }
    return;
  }

  if (phase_ == Phase::Sweep) {
    if (worked) {
      const decode_type_t protocol = static_cast<decode_type_t>(sweep_[sweepIndex_]);
      adoptProtocol(protocol, -1);
      finish(true, F("learn.protocol_adopted"));
      return;
    }
    sweepIndex_++;
    if (sweepIndex_ >= sweep_.size()) {
      finish(false, F("learn.sweep_exhausted"));
      return;
    }
    enter(Phase::Sweep, F("learn.sweep_watch"));
    sendSweepProbe();
  }
}

void Wizard::skip() {
  if (phase_ == Phase::Record) {
    skipped_++;
    advanceTarget();
    return;
  }
  if (phase_ == Phase::Sweep) {
    confirm(false);
    return;
  }
}

void Wizard::retry() {
  if (phase_ == Phase::Sweep) {
    sendSweepProbe();
    return;
  }
  stepStartedAt_ = millis();
}

void Wizard::adoptProtocol(decode_type_t protocol, int16_t model) {
  cfg::AcSettings &acCfg = cfg::settings.ac;
  acCfg.protocol = typeToString(protocol);
  acCfg.model = model;
  acCfg.useLearnedCodes = false;
  cfg::settings.touch();
  LOG_I(kTag, "adopted protocol %s (model %d)", acCfg.protocol.c_str(), model);
}

void Wizard::buildSweepList() {
  sweep_.clear();
  // decode_type_t values are small positive integers. isProtocolSupported() is
  // a plain switch, so probing the whole byte range is safe and avoids
  // depending on the library's "last protocol" constant.
  for (uint16_t i = 1; i <= 255; i++) {
    const decode_type_t protocol = static_cast<decode_type_t>(i);
    if (IRac::isProtocolSupported(protocol)) sweep_.push_back(static_cast<uint8_t>(i));
  }
  // If the receiver already told us something, try that one first.
  if (candidate_ != decode_type_t::UNKNOWN) {
    for (size_t i = 0; i < sweep_.size(); i++) {
      if (sweep_[i] == static_cast<uint8_t>(candidate_)) {
        std::swap(sweep_[0], sweep_[i]);
        break;
      }
    }
  }
}

bool Wizard::sendSweepProbe() {
  if (sweepIndex_ >= sweep_.size()) return false;
  const decode_type_t protocol = static_cast<decode_type_t>(sweep_[sweepIndex_]);

  // A distinctive, obviously-visible command: on, cool, 24 °C, with the beep
  // enabled so units that chirp announce themselves.
  ac::State probe = ac::defaultState();
  probe.power = true;
  probe.mode = stdAc::opmode_t::kCool;
  probe.degrees = 24.0f;
  probe.fanspeed = stdAc::fanspeed_t::kAuto;

  LOG_I(kTag, "sweep %u/%u: %s", sweepIndex_ + 1, (unsigned)sweep_.size(),
        typeToString(protocol).c_str());
  return ir::irService.sendAs(protocol, -1, probe);
}

// ---------------------------------------------------------------------------

void Wizard::statusJson(JsonObject out) const {
  out["phase"] = phaseName(phase_);
  out["active"] = active();
  out["prompt"] = promptKey_;
  out["message"] = messageKey_;
  out["elapsed"] = active() ? (millis() - startedAt_) / 1000 : 0;

  if (candidate_ != decode_type_t::UNKNOWN) {
    out["candidate"] = typeToString(candidate_);
    out["candidateSupported"] = IRac::isProtocolSupported(candidate_);
  }

  if (phase_ == Phase::Sweep) {
    JsonObject sweep = out["sweep"].to<JsonObject>();
    sweep["index"] = sweepIndex_ + 1;
    sweep["total"] = sweep_.size();
    if (sweepIndex_ < sweep_.size())
      sweep["protocol"] = typeToString(static_cast<decode_type_t>(sweep_[sweepIndex_]));
  }

  if (phase_ == Phase::Record || phase_ == Phase::Done) {
    JsonObject record = out["record"].to<JsonObject>();
    record["index"] = targetIndex_;
    record["total"] = targets_.size();
    record["recorded"] = recorded_;
    record["skipped"] = skipped_;
    if (targetIndex_ < targets_.size()) record["target"] = targets_[targetIndex_];
    JsonArray remaining = record["targets"].to<JsonArray>();
    for (size_t i = targetIndex_; i < targets_.size() && i < targetIndex_ + 8u; i++)
      remaining.add(targets_[i]);
  }
}

}  // namespace learn
