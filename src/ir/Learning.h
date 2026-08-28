// The "teach me your air conditioner" workflow.
//
// One state machine, driven identically from the web UI, from Telegram, or
// from any other client — they all call the same methods and render the same
// `statusJson()`. Instructions are returned as translation *keys*, never as
// English sentences, so every client can present them in the user's language.
//
// Three routes to a working setup, tried in this order:
//
//   1. Identify. The user presses one button on their remote. If
//      IRremoteESP8266 recognises the protocol, the device can synthesise every
//      other command by itself and learning is finished in one press.
//
//   2. Sweep. If the frame decodes to nothing useful, the device offers to
//      send a test command with each protocol it knows, one at a time, and
//      asks the user whether the AC reacted.
//
//   3. Record. If neither works, the device records raw captures for a list of
//      states the user demonstrates on their remote.
#pragma once

#include <ArduinoJson.h>

#include <vector>

#include "IrService.h"

namespace learn {

enum class Phase : uint8_t {
  Idle,
  Identify,     // waiting for the first press
  Confirm,      // "we sent a test command — did the AC react?"
  Sweep,        // stepping through candidate protocols
  Record,       // capturing raw codes, one target at a time
  Bind,         // waiting for a button on some other remote
  Done,
  Failed,
};

struct Plan {
  std::vector<stdAc::opmode_t> modes;
  std::vector<stdAc::fanspeed_t> fans;
  float minTemp = 18.0f;
  float maxTemp = 26.0f;
  float step = 1.0f;
  bool includeOff = true;
  std::vector<String> buttons;   // extra free-standing buttons, e.g. "turbo"
};

class Wizard {
 public:
  void begin();
  void loop();

  // --- entry points --------------------------------------------------------
  bool startIdentify(String &error);
  bool startSweep(String &error);
  bool startRecord(const Plan &plan, String &error);
  // Record exactly these keys — used when a client wants to re-teach one code.
  bool startRecordKeys(const std::vector<String> &keys, String &error);

  // Wait for a button on any handset and bind it to an action.
  bool startBind(const String &action, const String &label,
                 const String &argument, String &error);

  // --- driving the machine -------------------------------------------------
  void confirm(bool worked);   // answer for Confirm / Sweep
  void skip();                 // give up on the current target and move on
  void retry();                // discard the last capture and wait again
  void cancel();

  // --- state ---------------------------------------------------------------
  Phase phase() const { return phase_; }
  bool active() const { return phase_ != Phase::Idle && phase_ != Phase::Done &&
                               phase_ != Phase::Failed; }
  void statusJson(JsonObject out) const;

  // A short, already-translated-by-the-client description of what the user
  // should do right now, as a key plus substitution arguments.
  const String &promptKey() const { return promptKey_; }

  static Plan defaultPlan();
  static bool planFromJson(JsonObjectConst in, Plan &out, String &error);

 private:
  bool onCapture(const ir::Capture &capture);
  void enter(Phase phase, const String &promptKey);
  void buildTargets(const Plan &plan);
  void advanceTarget();
  void adoptProtocol(decode_type_t protocol, int16_t model);
  void buildSweepList();
  bool sendSweepProbe();
  void finish(bool success, const String &messageKey);

  Phase phase_ = Phase::Idle;
  String promptKey_;
  String messageKey_;

  // Identify / Confirm
  decode_type_t candidate_ = decode_type_t::UNKNOWN;
  int16_t candidateModel_ = -1;

  // Sweep
  std::vector<uint8_t> sweep_;
  uint16_t sweepIndex_ = 0;

  // Bind
  String bindAction_;
  String bindLabel_;
  String bindArgument_;

  // Record
  std::vector<String> targets_;
  uint16_t targetIndex_ = 0;
  uint16_t recorded_ = 0;
  uint16_t skipped_ = 0;

  uint32_t startedAt_ = 0;
  uint32_t stepStartedAt_ = 0;
};

extern Wizard wizard;

}  // namespace learn
