#include "Log.h"

#include <stdarg.h>

namespace log_ {
namespace {

// Records are stored back to back as:
//   uint32 seq | uint32 millis | uint8 level | uint8 len | len bytes of text
// When the buffer fills, the oldest whole records are dropped from the front.
struct Header {
  uint32_t seq;
  uint32_t at;
  uint8_t level;
  uint8_t len;
};

char s_buf[kBufferBytes];
size_t s_used = 0;
uint32_t s_seq = 0;

Level s_level = Level::Info;
bool s_serial = true;

const char *levelName(Level l) {
  switch (l) {
    case Level::Error: return "E";
    case Level::Warn:  return "W";
    case Level::Info:  return "I";
    default:           return "D";
  }
}

void dropOldest() {
  if (s_used < sizeof(Header)) {
    s_used = 0;
    return;
  }
  Header h;
  memcpy(&h, s_buf, sizeof(h));
  const size_t record = sizeof(Header) + h.len;
  if (record >= s_used) {
    s_used = 0;
    return;
  }
  memmove(s_buf, s_buf + record, s_used - record);
  s_used -= record;
}

void append(Level level, const char *text, size_t len) {
  if (len > 255) len = 255;
  const size_t record = sizeof(Header) + len;
  if (record > kBufferBytes) return;

  while (s_used + record > kBufferBytes) dropOldest();

  Header h;
  h.seq = ++s_seq;
  h.at = millis();
  h.level = static_cast<uint8_t>(level);
  h.len = static_cast<uint8_t>(len);

  memcpy(s_buf + s_used, &h, sizeof(h));
  memcpy(s_buf + s_used + sizeof(h), text, len);
  s_used += record;
}

}  // namespace

void begin(unsigned long baud) {
  Serial.begin(baud);
  Serial.setDebugOutput(false);
}

void setLevel(Level level) { s_level = level; }
Level level() { return s_level; }

void setSerialEnabled(bool enabled) { s_serial = enabled; }
bool serialEnabled() { return s_serial; }

uint32_t sequence() { return s_seq; }

void clear() {
  s_used = 0;
}

void write(Level level, const char *tag, const char *fmt, ...) {
  if (static_cast<uint8_t>(level) > static_cast<uint8_t>(s_level)) return;

  char line[192];
  const int prefix = snprintf(line, sizeof(line), "[%s] ", tag);
  if (prefix < 0) return;

  va_list args;
  va_start(args, fmt);
  const int written = vsnprintf(line + prefix, sizeof(line) - prefix, fmt, args);
  va_end(args);
  if (written < 0) return;

  size_t len = static_cast<size_t>(prefix) + static_cast<size_t>(written);
  if (len >= sizeof(line)) len = sizeof(line) - 1;

  append(level, line, len);

  if (s_serial) {
    Serial.printf("%8lu %s %s\r\n", millis(), levelName(level), line);
  }
}

uint32_t dump(String &out, uint32_t sinceSeq) {
  size_t offset = 0;
  uint32_t last = sinceSeq;

  while (offset + sizeof(Header) <= s_used) {
    Header h;
    memcpy(&h, s_buf + offset, sizeof(h));
    const size_t textAt = offset + sizeof(Header);
    if (textAt + h.len > s_used) break;

    if (h.seq > sinceSeq) {
      char meta[24];
      snprintf(meta, sizeof(meta), "%lu\t%lu\t%s\t",
               static_cast<unsigned long>(h.seq),
               static_cast<unsigned long>(h.at),
               levelName(static_cast<Level>(h.level)));
      out += meta;
      out.concat(s_buf + textAt, h.len);
      out += '\n';
      last = h.seq;
    }
    offset = textAt + h.len;
  }
  return last;
}

}  // namespace log_
