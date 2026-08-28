#include "Scenes.h"

#include <LittleFS.h>

#include "../core/CommandBus.h"
#include "../core/Log.h"
#include "../core/Settings.h"

namespace app {
namespace {
const char *kTag = "scenes";
const char *kPath = "/scenes.json";

// A scene matches the current state when everything it *sets* agrees. Fields
// the scene leaves alone are ignored, which is what makes partial scenes work.
bool deltaMatches(const ac::Delta &delta, const ac::State &state) {
  if (delta.hasPower && delta.power != state.power) return false;
  if (!state.power) return delta.hasPower && !delta.power;

  if (delta.hasMode && delta.mode != state.mode) return false;
  if (delta.hasDegrees && lroundf(delta.degrees) != lroundf(state.degrees))
    return false;
  if (delta.hasFan && delta.fan != state.fanspeed) return false;
  if (delta.hasSwingV && delta.swingv != state.swingv) return false;
  if (delta.hasQuiet && delta.quiet != state.quiet) return false;
  if (delta.hasTurbo && delta.turbo != state.turbo) return false;
  if (delta.hasEcono && delta.econo != state.econo) return false;
  return true;
}
}  // namespace

Scenes scenes;

// ---------------------------------------------------------------------------

void Scenes::begin() {
  if (!load()) installDefaults();
  LOG_I(kTag, "%u scene(s)", (unsigned)scenes_.size());
}

void Scenes::installDefaults() {
  // Chosen to be immediately useful and to demonstrate the shape of a scene:
  // one full state, one partial, one that only switches off.
  struct Preset {
    const char *id;
    const char *name;
    const char *icon;
    const char *action;
  };
  const Preset presets[] = {
      {"comfort", "Comfort", "",
       "{\"hvac_mode\":\"cool\",\"temp\":24,\"fan\":\"auto\",\"swingv\":\"auto\"}"},
      {"night", "Night", "",
       "{\"hvac_mode\":\"cool\",\"temp\":26,\"fan\":\"low\",\"quiet\":true}"},
      {"boost", "Boost", "",
       "{\"hvac_mode\":\"cool\",\"temp\":18,\"fan\":\"max\",\"turbo\":true}"},
      {"away", "Away", "", "{\"power\":false}"},
  };

  scenes_.clear();
  for (const Preset &preset : presets) {
    JsonDocument doc;
    if (deserializeJson(doc, preset.action) != DeserializationError::Ok) continue;

    Scene scene;
    scene.id = preset.id;
    scene.name = preset.name;
    scene.icon = preset.icon;
    scene.actionJson = preset.action;

    String error;
    if (!ac::deltaFromJson(doc.as<JsonObjectConst>(), scene.action, error)) continue;
    scenes_.push_back(scene);
  }
  LOG_I(kTag, "installed %u default scene(s)", (unsigned)scenes_.size());
}

// ---------------------------------------------------------------------------

const Scene *Scenes::find(const String &idOrName) const {
  for (const Scene &scene : scenes_) {
    if (scene.id.equalsIgnoreCase(idOrName) || scene.name.equalsIgnoreCase(idOrName))
      return &scene;
  }
  return nullptr;
}

bool Scenes::apply(const String &idOrName, src::Source source, String &error) {
  const Scene *scene = find(idOrName);
  if (scene == nullptr) {
    error = String(F("no scene called '")) + idOrName + "'";
    return false;
  }

  LOG_I(kTag, "%s applied scene '%s'", src::name(source), scene->name.c_str());

  const bus::Outcome outcome = bus::commands.apply(scene->action, source);
  if (!outcome.ok() && outcome.result != bus::Result::Deferred) {
    error = outcome.message;
    return false;
  }
  return true;
}

const Scene *Scenes::matching(const ac::State &state) const {
  for (const Scene &scene : scenes_) {
    if (deltaMatches(scene.action, state)) return &scene;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------

void Scenes::toJson(JsonArray out) const {
  for (const Scene &scene : scenes_) {
    JsonObject entry = out.add<JsonObject>();
    entry["id"] = scene.id;
    entry["name"] = scene.name;
    entry["icon"] = scene.icon;

    JsonDocument action;
    if (!scene.actionJson.isEmpty() &&
        deserializeJson(action, scene.actionJson) == DeserializationError::Ok) {
      entry["action"] = action;
    } else {
      entry["action"].to<JsonObject>();
    }
  }
}

bool Scenes::fromJson(JsonArrayConst in, String &error) {
  if (in.size() > kMaxScenes) {
    error = String(F("at most ")) + kMaxScenes + F(" scenes are supported");
    return false;
  }

  std::vector<Scene> parsed;
  for (JsonObjectConst entry : in) {
    Scene scene;
    scene.name = entry["name"] | "";
    if (scene.name.isEmpty()) {
      error = F("every scene needs a name");
      return false;
    }

    if (entry["id"].is<const char *>()) {
      scene.id = entry["id"].as<const char *>();
    } else {
      // Derive a stable, filename-and-topic-safe id from the name.
      scene.id = scene.name;
      scene.id.toLowerCase();
      for (size_t i = 0; i < scene.id.length(); i++) {
        const char c = scene.id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) scene.id.setCharAt(i, '_');
      }
    }
    scene.icon = entry["icon"] | "";

    JsonObjectConst action = entry["action"];
    if (action.isNull()) {
      error = String(F("scene '")) + scene.name + F("' has no action");
      return false;
    }
    if (!ac::deltaFromJson(action, scene.action, error)) return false;
    if (scene.action.empty()) {
      error = String(F("scene '")) + scene.name + F("' does not change anything");
      return false;
    }
    serializeJson(action, scene.actionJson);

    parsed.push_back(scene);
  }

  scenes_ = parsed;
  return true;
}

// ---------------------------------------------------------------------------

bool Scenes::load() {
  File file = LittleFS.open(kPath, "r");
  if (!file) return false;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    LOG_W(kTag, "scenes parse error: %s", err.c_str());
    return false;
  }

  String error;
  if (!fromJson(doc.as<JsonArrayConst>(), error)) {
    LOG_W(kTag, "scenes rejected: %s", error.c_str());
    return false;
  }
  return !scenes_.empty();
}

bool Scenes::save() {
  JsonDocument doc;
  JsonArray root = doc.to<JsonArray>();
  toJson(root);

  File file = LittleFS.open(kPath, "w");
  if (!file) {
    LOG_E(kTag, "cannot write %s", kPath);
    return false;
  }
  serializeJson(doc, file);
  file.close();
  return true;
}

}  // namespace app
