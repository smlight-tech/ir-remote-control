#include "CodeStore.h"

#include <LittleFS.h>

#include "../core/Log.h"

namespace ir {
namespace {
const char *kTag = "codes";
const char *kDir = "/codes";
const uint32_t kMagic = 0x31435249;  // "IRC1"

struct FileHeader {
  uint32_t magic;
  uint16_t carrierKhz;
  uint16_t length;
};

// Keys become filenames, so keep them to a conservative character set.
bool safeChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}
}  // namespace

CodeStore codes;

void RawCode::release() {
  if (timings != nullptr) {
    ::free(timings);
    timings = nullptr;
  }
  length = 0;
}

// ---------------------------------------------------------------------------

bool CodeStore::begin() {
  if (!LittleFS.exists(kDir)) LittleFS.mkdir(kDir);
  count_ = recount();
  LOG_I(kTag, "%u learned code(s), %u bytes", count_, (unsigned)bytesUsed());
  return true;
}

uint16_t CodeStore::recount() {
  uint16_t n = 0;
  Dir dir = LittleFS.openDir(kDir);
  while (dir.next()) n++;
  return n;
}

String CodeStore::keyFor(const ac::State &state) {
  if (!state.power) return F("off");

  char buf[40];
  snprintf(buf, sizeof(buf), "%s_%d_%s", ac::modeName(state.mode),
           static_cast<int>(lroundf(state.degrees)), ac::fanName(state.fanspeed));
  return String(buf);
}

String CodeStore::buttonKey(const String &name) {
  String key = F("btn_");
  key += name;
  key.toLowerCase();
  return key;
}

bool CodeStore::validKey(const String &key) {
  if (key.isEmpty() || key.length() > 48) return false;
  for (size_t i = 0; i < key.length(); i++)
    if (!safeChar(key[i])) return false;
  return true;
}

String CodeStore::pathFor(const String &key) {
  return String(kDir) + "/" + key + ".ir";
}

bool CodeStore::has(const String &key) const {
  return validKey(key) && LittleFS.exists(pathFor(key));
}

bool CodeStore::load(const String &key, RawCode &out) const {
  if (!validKey(key)) return false;

  File file = LittleFS.open(pathFor(key), "r");
  if (!file) return false;

  FileHeader header;
  if (file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) !=
      sizeof(header)) {
    file.close();
    return false;
  }
  if (header.magic != kMagic || header.length == 0 ||
      header.length > kMaxRawLength) {
    LOG_W(kTag, "corrupt code '%s'", key.c_str());
    file.close();
    return false;
  }

  const size_t bytes = header.length * sizeof(uint16_t);
  uint16_t *buffer = static_cast<uint16_t *>(malloc(bytes));
  if (buffer == nullptr) {
    LOG_E(kTag, "out of memory loading '%s' (%u bytes)", key.c_str(),
          (unsigned)bytes);
    file.close();
    return false;
  }

  const size_t read = file.read(reinterpret_cast<uint8_t *>(buffer), bytes);
  file.close();

  if (read != bytes) {
    ::free(buffer);
    return false;
  }

  out.timings = buffer;
  out.length = header.length;
  out.carrierKhz = header.carrierKhz;
  return true;
}

bool CodeStore::resolve(const ac::State &state, RawCode &out,
                        String &usedKey) const {
  // 1. Exact match.
  String key = keyFor(state);
  if (load(key, out)) {
    usedKey = key;
    return true;
  }

  if (!state.power) return false;  // "off" has no sensible fallback

  const int wanted = static_cast<int>(lroundf(state.degrees));
  const char *mode = ac::modeName(state.mode);

  // 2. Same mode and temperature, any fan speed.
  static const char *const kFanNames[] = {"auto",        "min",  "low", "medium",
                                          "medium_high", "high", "max"};
  for (const char *fan : kFanNames) {
    key = String(mode) + "_" + wanted + "_" + fan;
    if (load(key, out)) {
      usedKey = key;
      return true;
    }
  }

  // 3. Same mode, nearest temperature that was actually taught.
  int bestDelta = 0x7fff;
  String bestKey;
  Dir dir = LittleFS.openDir(kDir);
  const String prefix = String(mode) + "_";
  while (dir.next()) {
    String name = dir.fileName();
    if (!name.endsWith(".ir")) continue;
    name.remove(name.length() - 3);
    if (!name.startsWith(prefix)) continue;

    const int underscore = name.indexOf('_', prefix.length());
    if (underscore < 0) continue;
    const int temp = name.substring(prefix.length(), underscore).toInt();
    const int delta = abs(temp - wanted);
    if (delta < bestDelta) {
      bestDelta = delta;
      bestKey = name;
    }
  }

  if (!bestKey.isEmpty() && load(bestKey, out)) {
    usedKey = bestKey;
    LOG_W(kTag, "no code for %s, falling back to %s", keyFor(state).c_str(),
          bestKey.c_str());
    return true;
  }

  return false;
}

bool CodeStore::store(const String &key, const uint16_t *timings,
                      uint16_t length, uint16_t carrierKhz) {
  if (!validKey(key)) {
    LOG_E(kTag, "refusing to store invalid key '%s'", key.c_str());
    return false;
  }
  if (timings == nullptr || length < 2 || length > kMaxRawLength) {
    LOG_E(kTag, "refusing to store '%s': bad length %u", key.c_str(), length);
    return false;
  }

  const bool isNew = !has(key);

  File file = LittleFS.open(pathFor(key), "w");
  if (!file) {
    LOG_E(kTag, "cannot write '%s'", key.c_str());
    return false;
  }

  FileHeader header{kMagic, carrierKhz, length};
  file.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header));
  file.write(reinterpret_cast<const uint8_t *>(timings),
             length * sizeof(uint16_t));
  file.close();

  if (isNew) count_++;
  LOG_I(kTag, "stored '%s' (%u marks @ %u kHz)", key.c_str(), length, carrierKhz);
  return true;
}

bool CodeStore::remove(const String &key) {
  if (!has(key)) return false;
  if (!LittleFS.remove(pathFor(key))) return false;
  if (count_ > 0) count_--;
  return true;
}

void CodeStore::clear() {
  Dir dir = LittleFS.openDir(kDir);
  while (dir.next()) LittleFS.remove(String(kDir) + "/" + dir.fileName());
  count_ = 0;
  LOG_W(kTag, "all learned codes erased");
}

void CodeStore::listJson(JsonArray out) const {
  Dir dir = LittleFS.openDir(kDir);
  while (dir.next()) {
    String name = dir.fileName();
    if (!name.endsWith(".ir")) continue;
    name.remove(name.length() - 3);

    JsonObject entry = out.add<JsonObject>();
    entry["key"] = name;
    // Subtract the header and convert to mark/space count.
    const size_t size = dir.fileSize();
    entry["marks"] = size > sizeof(FileHeader)
                         ? (size - sizeof(FileHeader)) / sizeof(uint16_t)
                         : 0;
    entry["bytes"] = size;
  }
}

bool CodeStore::codeToJson(const String &key, JsonObject out) const {
  RawCode code;
  if (!load(key, code)) return false;

  out["key"] = key;
  out["khz"] = code.carrierKhz;
  JsonArray timings = out["timings"].to<JsonArray>();
  for (uint16_t i = 0; i < code.length; i++) timings.add(code.timings[i]);

  code.release();
  return true;
}

size_t CodeStore::bytesUsed() const {
  size_t total = 0;
  Dir dir = LittleFS.openDir(kDir);
  while (dir.next()) total += dir.fileSize();
  return total;
}

}  // namespace ir
