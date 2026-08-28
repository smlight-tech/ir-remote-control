#include "DeviceTypes.h"

#include <LittleFS.h>

#include "../core/Log.h"

namespace app {
namespace {
const char *kTag = "types";

// Served to the browser as a static file and read here as configuration; one
// file, so the two views cannot drift.
const char *kPath = "/devicetypes.json";
const char *kGzipPath = "/devicetypes.json.gz";
}  // namespace

DeviceTypes deviceTypes;

bool DeviceTypes::load() {
  loaded_ = true;   // set first: a failed read must not retry on every lookup

  families_.clear();
  typeToFamily_.clear();

  // The build gzips web assets into the filesystem image, so the plain name
  // may not exist. Reading the compressed copy would need an inflater the
  // firmware does not carry, so the build keeps this one uncompressed — if
  // only the .gz is present, that is a packaging mistake worth saying out loud.
  if (!LittleFS.exists(kPath)) {
    if (LittleFS.exists(kGzipPath)) {
      LOG_E(kTag, "%s is gzipped; the firmware cannot read it. Add it to the "
                  "no-compress list in tools/build_web.py", kPath);
    } else {
      LOG_W(kTag, "%s is missing — no device types are known", kPath);
    }
    return false;
  }

  File file = LittleFS.open(kPath, "r");
  if (!file) return false;

  // Only the fields listed here survive parsing, which keeps a growing type
  // database from growing the firmware's heap use with it.
  JsonDocument filter;
  JsonObject family = filter["families"]["*"].to<JsonObject>();
  family["api"]["status"] = true;
  family["api"]["state"] = true;
  family["api"]["command"] = true;
  family["auth"] = true;
  family["commandStyle"] = true;
  family["needsEntity"] = true;
  family["info"] = true;
  family["transport"] = true;
  JsonObject condition = family["conditions"][0].to<JsonObject>();
  condition["id"] = true;
  condition["path"] = true;
  JsonObject type = filter["types"][0].to<JsonObject>();
  type["id"] = true;
  type["family"] = true;

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, file, DeserializationOption::Filter(filter));
  file.close();

  if (err) {
    LOG_E(kTag, "%s could not be parsed: %s", kPath, err.c_str());
    return false;
  }

  for (JsonPairConst entry : doc["families"].as<JsonObjectConst>()) {
    DeviceFamily family_;
    family_.id = entry.key().c_str();
    JsonObjectConst value = entry.value().as<JsonObjectConst>();
    family_.statusPath = value["api"]["status"] | "";
    family_.statePath = value["api"]["state"] | "";
    family_.commandPath = value["api"]["command"] | "";
    family_.bearerAuth = String(value["auth"] | "none") == "bearer";
    family_.queryCommands = String(value["commandStyle"] | "json") == "query";
    family_.needsEntity = value["needsEntity"] | false;
    family_.transport = String(value["transport"] | "http") == "wol"
                            ? Transport::Wol
                            : Transport::Http;

    // A field the card shows under the controls — for an SLWF-12, which
    // protocol it was taught. It has to be cached like a condition field, so
    // it joins the same list.
    const char *info = value["info"];
    if (info != nullptr) family_.fields.push_back(String(info));

    for (JsonObjectConst cond : value["conditions"].as<JsonArrayConst>()) {
      // `path` is where the value lives in that device's own JSON; `id` is
      // what the automation editor calls it. They differ for WLED, where the
      // brightness field is `bri`.
      const char *path = cond["path"];
      const char *id = cond["id"];
      const char *field = path != nullptr ? path : id;
      if (field != nullptr) family_.fields.push_back(String(field));
    }
    families_.push_back(family_);
  }

  for (JsonObjectConst entry : doc["types"].as<JsonArrayConst>()) {
    TypeMapping mapping;
    mapping.typeId = entry["id"] | "";
    mapping.familyId = entry["family"] | "";
    if (!mapping.typeId.isEmpty() && !mapping.familyId.isEmpty()) {
      typeToFamily_.push_back(mapping);
    }
  }

  LOG_I(kTag, "%u device type(s) in %u family(ies)",
        (unsigned)typeToFamily_.size(), (unsigned)families_.size());
  return true;
}

const char *DeviceTypes::transportName(Transport transport) {
  return transport == Transport::Wol ? "wol" : "http";
}

const DeviceFamily *DeviceTypes::familyForType(const String &typeId) const {
  // Loaded on the first question anybody asks, rather than at boot.
  if (!loaded_) const_cast<DeviceTypes *>(this)->load();

  for (const TypeMapping &mapping : typeToFamily_) {
    if (mapping.typeId != typeId) continue;
    for (const DeviceFamily &family : families_) {
      if (family.id == mapping.familyId) return &family;
    }
    return nullptr;
  }
  return nullptr;
}

}  // namespace app
