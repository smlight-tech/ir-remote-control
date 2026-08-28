#include "Source.h"

#include <string.h>

namespace src {
namespace {
const char *const kNames[] = {"web",    "api",  "mqtt",   "telegram",
                              "ir",     "remote", "uart", "modbus",
                              "schedule", "automation", "system"};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == kCount,
              "Source names out of sync with the enum");
}  // namespace

const char *name(Source s) {
  const uint8_t i = static_cast<uint8_t>(s);
  return i < kCount ? kNames[i] : "?";
}

bool parse(const char *text, Source &out) {
  if (text == nullptr) return false;
  for (uint8_t i = 0; i < kCount; i++) {
    if (strcasecmp(kNames[i], text) == 0) {
      out = static_cast<Source>(i);
      return true;
    }
  }
  return false;
}

}  // namespace src
