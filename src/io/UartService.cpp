#include "UartService.h"

#include <ArduinoJson.h>

#include "../core/CommandBus.h"
#include "../core/Log.h"
#include "../core/Settings.h"
#include "../net/ApiRoutes.h"

namespace io {
namespace {
const char *kTag = "uart";
}

UartService uart;

// ---------------------------------------------------------------------------

void UartService::begin() { reconfigure(); }

void UartService::reconfigure() {
  const cfg::UartSettings &u = cfg::settings.uart;
  enabled_ = u.enabled;
  buffer_ = "";

  if (!enabled_) {
    log_::setSerialEnabled(cfg::settings.log.serial);
    return;
  }

  if (Serial.baudRate() != static_cast<int>(u.baud)) {
    Serial.flush();
    Serial.begin(u.baud);
  }
  // The port now carries a protocol; log lines would corrupt it.
  log_::setSerialEnabled(false);

  JsonDocument doc;
  doc["event"] = "ready";
  doc["device"] = cfg::settings.device.name;
  doc["id"] = cfg::settings.chipId();
  emit(doc);

  LOG_I(kTag, "UART client enabled at %lu baud", (unsigned long)u.baud);
}

// ---------------------------------------------------------------------------

void UartService::loop() {
  if (!enabled_) return;

  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      const String line = buffer_;
      buffer_ = "";
      if (line.length() > 0) handleLine(line);
      continue;
    }
    if (buffer_.length() < kMaxLine) {
      buffer_ += c;
    } else {
      // Runaway line: drop it and resynchronise on the next newline.
      buffer_ = "";
      emitError(F("line too long"));
    }
  }
}

void UartService::handleLine(const String &raw) {
  String line = raw;
  line.trim();
  if (line.isEmpty()) return;

  if (line[0] == '{') {
    handleJson(line);
  } else {
    handleWord(line);
  }
}

void UartService::handleJson(const String &line) {
  JsonDocument doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok) {
    emitError(F("malformed JSON"));
    return;
  }

  JsonObjectConst object = doc.as<JsonObjectConst>();
  const String command = object["cmd"] | "set";

  if (command == "status") {
    JsonDocument out;
    JsonObject root = out.to<JsonObject>();
    root["event"] = "status";
    net::buildStatus(root);
    emit(out);
    return;
  }
  if (command == "state") {
    emitState("state", bus::commands.lastSource());
    return;
  }
  if (command == "resend") {
    const bus::Outcome outcome = bus::commands.resend(src::Source::Uart);
    if (!outcome.ok()) emitError(outcome.message);
    else emitState("state", src::Source::Uart);
    return;
  }
  if (command != "set") {
    emitError(String(F("unknown command: ")) + command);
    return;
  }

  ac::Delta delta;
  String error;
  if (!ac::deltaFromJson(object, delta, error)) {
    emitError(error);
    return;
  }

  const bus::Outcome outcome = bus::commands.apply(delta, src::Source::Uart);
  if (!outcome.ok()) {
    emitError(outcome.message);
    return;
  }
  emitState("state", src::Source::Uart);
}

void UartService::handleWord(const String &line) {
  String word = line;
  word.toLowerCase();

  String argument;
  const int space = word.indexOf(' ');
  if (space > 0) {
    argument = word.substring(space + 1);
    word = word.substring(0, space);
    argument.trim();
  }

  ac::Delta delta;

  if (word == "help" || word == "?") {
    Serial.println(F("on | off | temp <c> | mode <name> | fan <name> | "
                     "status | state | resend"));
    Serial.println(F("or send a JSON object, e.g. {\"cmd\":\"set\","
                     "\"mode\":\"cool\",\"temp\":24}"));
    return;
  }
  if (word == "status") {
    JsonDocument out;
    JsonObject root = out.to<JsonObject>();
    root["event"] = "status";
    net::buildStatus(root);
    emit(out);
    return;
  }
  if (word == "state") {
    emitState("state", bus::commands.lastSource());
    return;
  }
  if (word == "resend") {
    bus::commands.resend(src::Source::Uart);
    emitState("state", src::Source::Uart);
    return;
  }
  if (word == "on" || word == "off") {
    delta.hasPower = true;
    delta.power = word == "on";
  } else if (word == "temp") {
    delta.hasDegrees = true;
    delta.degrees = argument.toFloat();
  } else if (word == "mode") {
    if (!ac::parseMode(argument.c_str(), delta.mode)) {
      emitError(F("unknown mode"));
      return;
    }
    delta.hasMode = true;
    delta.hasPower = true;
    delta.power = true;
  } else if (word == "fan") {
    if (!ac::parseFan(argument.c_str(), delta.fan)) {
      emitError(F("unknown fan speed"));
      return;
    }
    delta.hasFan = true;
  } else {
    emitError(F("unknown command — send 'help'"));
    return;
  }

  const bus::Outcome outcome = bus::commands.apply(delta, src::Source::Uart);
  if (!outcome.ok()) {
    emitError(outcome.message);
    return;
  }
  emitState("state", src::Source::Uart);
}

// ---------------------------------------------------------------------------

void UartService::onStateChanged(const ac::State &state, src::Source source) {
  (void)state;
  if (!enabled_ || !cfg::settings.uart.emitEvents) return;
  if (source == src::Source::Uart) return;   // already acknowledged
  emitState("changed", source);
}

void UartService::emitState(const char *event, src::Source source) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["event"] = event;
  root["source"] = src::name(source);
  root["revision"] = bus::commands.revision();
  JsonObject state = root["state"].to<JsonObject>();
  ac::toJson(bus::commands.state(), state);
  emit(doc);
}

void UartService::emitError(const String &message) {
  JsonDocument doc;
  doc["event"] = "error";
  doc["error"] = message;
  emit(doc);
}

void UartService::emit(const JsonDocument &doc) {
  serializeJson(doc, Serial);
  Serial.print("\r\n");
}

}  // namespace io
