// Named presets — "Night", "Boost", "Away" — applied with one tap.
//
// A scene is just a stored `ac::Delta`, so it composes with everything else:
// the same scene can be fired from the web card, a Telegram button, a schedule,
// or a Home Assistant dropdown, and it goes through the same command bus with
// the same source gating as any other command.
//
// Partial scenes are deliberately allowed. "Quiet" can set nothing but the fan
// speed, leaving the mode and temperature as they were.
#pragma once

#include <ArduinoJson.h>

#include <vector>

#include "../core/AcState.h"
#include "../core/Source.h"

namespace app {

struct Scene {
  String id;
  String name;
  String icon;          // emoji, shown by clients that can render one
  ac::Delta action;
  String actionJson;    // kept verbatim so a round trip through a client is lossless
};

class Scenes {
 public:
  void begin();

  bool load();
  bool save();

  const std::vector<Scene> &all() const { return scenes_; }
  size_t count() const { return scenes_.size(); }

  // Accepts an id or a (case-insensitive) name, so `/scene night` works.
  const Scene *find(const String &idOrName) const;
  bool apply(const String &idOrName, src::Source source, String &error);

  void toJson(JsonArray out) const;
  bool fromJson(JsonArrayConst in, String &error);

  // Which scene, if any, the current state matches — so a Home Assistant
  // dropdown or the web card can show the active one rather than guessing.
  const Scene *matching(const ac::State &state) const;

 private:
  void installDefaults();

  static const uint8_t kMaxScenes = 8;

  std::vector<Scene> scenes_;
};

extern Scenes scenes;

}  // namespace app
