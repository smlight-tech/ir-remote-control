#include "TelegramService.h"

#include <ESP8266WiFi.h>

#include "../app/Scenes.h"
#include "../core/CommandBus.h"
#include "../core/Log.h"
#include "../core/Settings.h"
#include "../ir/Learning.h"

namespace net {
namespace {
const char *kTag = "tg";
const char *kHost = "api.telegram.org";
const uint16_t kPort = 443;

// Long-poll window. Telegram holds the request open this long when there is
// nothing to report, which keeps one connection busy instead of many.
const uint8_t kLongPollSeconds = 20;

// Hard ceiling on a response body. Update payloads with a filter applied stay
// far below this; anything larger is a sign something went wrong.
const size_t kMaxResponseBytes = 6144;

const uint32_t kMaxBackoffMs = 300000;

String jsonEscapeFree(const String &text) {
  // Telegram accepts plain text; strip the few characters that would confuse
  // the lightweight formatting we use.
  String out = text;
  out.replace("<", "‹");
  out.replace(">", "›");
  out.replace("&", "+");
  return out;
}
}  // namespace

TelegramService telegram;

// ---------------------------------------------------------------------------

void TelegramService::begin() { reconfigure(); }

void TelegramService::reconfigure() {
  closeConnection("configuration changed");
  queue_.clear();

  const cfg::TelegramSettings &t = cfg::settings.telegram;
  if (!t.enabled || t.token.isEmpty()) {
    phase_ = Phase::Disabled;
    LOG_I(kTag, "disabled");
    return;
  }

  phase_ = Phase::Backoff;
  phaseStartedAt_ = millis();
  backoffMs_ = 1000;
  mflnProbed_ = false;
  LOG_I(kTag, "enabled, %u authorised chat(s)", t.allowedCount);
}

void TelegramService::closeConnection(const char *why) {
  if (client_ != nullptr) {
    client_->stop();
    delete client_;
    client_ = nullptr;
    LOG_D(kTag, "connection closed: %s", why);
  }
  headersDone_ = false;
  contentLength_ = -1;
  chunked_ = false;
  responseBuffer_ = "";
}

// ---------------------------------------------------------------------------

bool TelegramService::openConnection() {
  if (!mflnProbed_) {
    // Maximum-fragment-length negotiation lets the receive buffer shrink from
    // 16 kB to 512 bytes. Worth one probe per boot: on this part it is the
    // difference between Telegram fitting and not fitting.
    mflnSupported_ =
        BearSSL::WiFiClientSecure::probeMaxFragmentLength(kHost, kPort, 512);
    mflnProbed_ = true;
    LOG_I(kTag, "TLS fragment negotiation is %s",
          mflnSupported_ ? "supported" : "not supported by the server");
  }

  // A TLS 1.2 record may legally reach 16 kB and the *server* picks the size,
  // so without fragment negotiation that is what has to be reserved. There is
  // no clever way around it — only the honest choice between reserving the
  // memory and telling the user it does not fit.
  const uint16_t configured = cfg::settings.telegram.tlsBufferBytes;
  rxBufferBytes_ = configured != 0 ? configured : (mflnSupported_ ? 512 : 16384);

  // Buffers, plus BearSSL's own context and the handshake's working set.
  const uint32_t needed = rxBufferBytes_ + 512 + 6000;
  const uint32_t heap = ESP.getFreeHeap();
  if (heap < needed) {
    lastError_ = String(F("needs about ")) + (needed / 1024) +
                 F(" kB of heap for a ") + (rxBufferBytes_ / 1024) +
                 F(" kB TLS buffer, but only ") + (heap / 1024) +
                 F(" kB is free");
    LOG_W(kTag, "%s", lastError_.c_str());
    return false;
  }

  client_ = new BearSSL::WiFiClientSecure();
  if (client_ == nullptr) return false;

  // Telegram rotates its certificates, and pinning a fingerprint into firmware
  // means the bot silently dies on renewal. The bot token is the real
  // credential here, and it is only ever sent to api.telegram.org.
  client_->setInsecure();
  client_->setBufferSizes(rxBufferBytes_, 512);
  client_->setTimeout(15000);

  LOG_D(kTag, "connecting to %s", kHost);
  if (!client_->connect(kHost, kPort)) {
    char message[64];
    client_->getLastSSLError(message, sizeof(message));
    lastError_ = String(F("TLS connect failed: ")) + message;
    LOG_W(kTag, "%s", lastError_.c_str());
    closeConnection("connect failed");
    return false;
  }

  reconnects_++;
  lastError_ = "";
  LOG_I(kTag, "connected (heap %u)", ESP.getFreeHeap());
  return true;
}

// ---------------------------------------------------------------------------

void TelegramService::enqueue(const String &method, const JsonDocument &body,
                              bool isPoll) {
  if (phase_ == Phase::Disabled) return;
  // Never let a burst of state changes build an unbounded backlog.
  if (queue_.size() >= 8) {
    LOG_W(kTag, "outbound queue full, dropping %s", method.c_str());
    return;
  }
  Request request;
  request.method = method;
  serializeJson(body, request.body);
  request.isPoll = isPoll;
  queue_.push_back(request);
}

void TelegramService::queuePoll() {
  JsonDocument doc;
  doc["offset"] = updateOffset_;
  doc["limit"] = 3;
  doc["timeout"] = kLongPollSeconds;
  JsonArray allowed = doc["allowed_updates"].to<JsonArray>();
  allowed.add("message");
  allowed.add("callback_query");
  enqueue("getUpdates", doc, /*isPoll=*/true);
}

void TelegramService::writeRequest(const Request &request) {
  const String path = String("/bot") + cfg::settings.telegram.token + "/" +
                      request.method;

  client_->print(F("POST "));
  client_->print(path);
  client_->print(F(" HTTP/1.1\r\nHost: "));
  client_->print(kHost);
  client_->print(F("\r\nContent-Type: application/json\r\nConnection: keep-alive"
                   "\r\nContent-Length: "));
  client_->print(request.body.length());
  client_->print(F("\r\n\r\n"));
  client_->print(request.body);

  resetResponse();
}

void TelegramService::resetResponse() {
  headersDone_ = false;
  bodyDone_ = false;
  contentLength_ = -1;
  chunked_ = false;
  chunkRemaining_ = -1;
  responseBuffer_ = "";
}

// ---------------------------------------------------------------------------

void TelegramService::loop() {
  if (phase_ == Phase::Disabled) return;
  if (WiFi.status() != WL_CONNECTED) return;

  switch (phase_) {
    case Phase::Backoff:
      if (millis() - phaseStartedAt_ < backoffMs_) return;
      phase_ = Phase::Connecting;
      break;

    case Phase::Connecting:
      if (!openConnection()) {
        backoffMs_ = min(backoffMs_ * 2, kMaxBackoffMs);
        phase_ = Phase::Backoff;
        phaseStartedAt_ = millis();
        return;
      }
      backoffMs_ = 5000;
      phase_ = Phase::Idle;
      return;

    case Phase::Idle: {
      if (client_ == nullptr || !client_->connected()) {
        closeConnection("dropped while idle");
        phase_ = Phase::Backoff;
        phaseStartedAt_ = millis();
        return;
      }
      if (queue_.empty()) {
        if (millis() < nextPollAt_) return;
        queuePoll();
      }
      inFlight_ = queue_.front();
      queue_.erase(queue_.begin());
      writeRequest(inFlight_);
      phase_ = Phase::Reading;
      phaseStartedAt_ = millis();
      return;
    }

    case Phase::Reading:
      readResponse();
      return;

    default:
      return;
  }
}

void TelegramService::readResponse() {
  // A long poll legitimately takes up to kLongPollSeconds; anything much past
  // that means the connection is wedged.
  const uint32_t timeoutMs = (inFlight_.isPoll ? kLongPollSeconds + 15 : 20) * 1000UL;
  if (millis() - phaseStartedAt_ > timeoutMs) {
    LOG_W(kTag, "%s timed out", inFlight_.method.c_str());
    closeConnection("response timeout");
    phase_ = Phase::Backoff;
    phaseStartedAt_ = millis();
    backoffMs_ = 5000;
    return;
  }

  if (client_ == nullptr) {
    phase_ = Phase::Backoff;
    phaseStartedAt_ = millis();
    return;
  }

  // Headers first, one line at a time.
  while (!headersDone_ && client_->available() > 0) {
    const String line = client_->readStringUntil('\n');
    if (line.length() <= 1) {          // bare CRLF ends the header block
      headersDone_ = true;
      break;
    }
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) {
      contentLength_ = line.substring(15).toInt();
    } else if (lower.startsWith("transfer-encoding:") &&
               lower.indexOf("chunked") >= 0) {
      chunked_ = true;
    }
  }

  if (!headersDone_) return;
  if (!pumpBody()) return;

  handleResponse(inFlight_, responseBuffer_);
  resetResponse();
  phase_ = Phase::Idle;
}

// Reads body bytes into `responseBuffer_`, transparently unwrapping chunked
// transfer encoding — which api.telegram.org does use, so the naive
// "concatenate everything after the headers" approach would hand the JSON
// parser a document with hex length prefixes sprinkled through it.
bool TelegramService::pumpBody() {
  if (bodyDone_) return true;

  // Bounded per call so the rest of the loop keeps running on a big response.
  uint16_t budget = 1024;

  while (budget > 0 && client_->available() > 0) {
    if (!chunked_) {
      const int c = client_->read();
      if (c < 0) break;
      budget--;
      if (responseBuffer_.length() < kMaxResponseBytes)
        responseBuffer_ += static_cast<char>(c);

      if (contentLength_ >= 0 &&
          responseBuffer_.length() >= static_cast<size_t>(contentLength_)) {
        bodyDone_ = true;
        return true;
      }
      continue;
    }

    if (chunkRemaining_ < 0) {
      // Chunk size line: hex digits, optionally followed by ";extension".
      String line = client_->readStringUntil('\n');
      line.trim();
      if (line.isEmpty()) continue;          // trailing CRLF of the last chunk
      const int semicolon = line.indexOf(';');
      if (semicolon >= 0) line = line.substring(0, semicolon);
      chunkRemaining_ = static_cast<int32_t>(strtol(line.c_str(), nullptr, 16));
      if (chunkRemaining_ == 0) {
        bodyDone_ = true;
        return true;
      }
      continue;
    }

    const int c = client_->read();
    if (c < 0) break;
    budget--;
    chunkRemaining_--;
    if (responseBuffer_.length() < kMaxResponseBytes)
      responseBuffer_ += static_cast<char>(c);

    if (chunkRemaining_ == 0) chunkRemaining_ = -1;   // expect the next header
  }

  // A closed connection with nothing buffered ends the body too — this is the
  // HTTP/1.0-style "read until EOF" case.
  if (!client_->connected() && client_->available() == 0) {
    bodyDone_ = true;
    return true;
  }
  return false;
}

void TelegramService::handleResponse(const Request &request, const String &raw) {
  if (!request.isPoll) return;   // fire-and-forget for sends

  nextPollAt_ = millis() + cfg::settings.telegram.pollSeconds * 1000UL;

  // Chunked bodies arrive with size prefixes; find the JSON inside.
  const int start = raw.indexOf('{');
  if (start < 0) return;

  // Only pull out what the bot actually uses. Without a filter a busy chat can
  // produce a document several times larger than the free heap.
  JsonDocument filter;
  JsonObject result = filter["result"][0].to<JsonObject>();
  result["update_id"] = true;
  JsonObject message = result["message"].to<JsonObject>();
  message["text"] = true;
  message["chat"]["id"] = true;
  message["from"]["first_name"] = true;
  JsonObject callback = result["callback_query"].to<JsonObject>();
  callback["id"] = true;
  callback["data"] = true;
  callback["from"]["first_name"] = true;
  callback["message"]["message_id"] = true;
  callback["message"]["chat"]["id"] = true;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(
      doc, raw.c_str() + start, DeserializationOption::Filter(filter));
  if (err) {
    LOG_W(kTag, "could not parse update: %s", err.c_str());
    return;
  }

  for (JsonObjectConst update : doc["result"].as<JsonArrayConst>()) {
    const int64_t id = update["update_id"] | 0;
    if (id >= updateOffset_) updateOffset_ = id + 1;
    handleUpdate(update);
  }
}

// ---------------------------------------------------------------------------

void TelegramService::handleUpdate(JsonObjectConst update) {
  JsonObjectConst message = update["message"];
  if (!message.isNull()) {
    handleMessage(message["chat"]["id"] | 0,
                  message["from"]["first_name"] | "there",
                  message["text"] | "");
    return;
  }

  JsonObjectConst callback = update["callback_query"];
  if (!callback.isNull()) {
    handleCallback(callback["message"]["chat"]["id"] | 0,
                   callback["id"] | "",
                   callback["data"] | "",
                   callback["message"]["message_id"] | 0);
  }
}

bool TelegramService::authorise(int64_t chatId, const String &from) {
  cfg::TelegramSettings &t = cfg::settings.telegram;
  if (t.isAllowed(chatId)) return true;

  // First person to say hello owns the bot; after that the door is shut.
  if (t.openEnrolment && t.allowedCount == 0) {
    t.allow(chatId);
    t.openEnrolment = false;
    cfg::settings.touch();
    LOG_I(kTag, "enrolled %s (chat %lld) as the bot owner", from.c_str(),
          (long long)chatId);
    sendMessage(chatId,
                F("You are now the owner of this SLWF-12 bridge. Nobody else "
                  "can control it through this bot.\n\nSend /menu to begin."),
                true);
    return false;   // the greeting above is the whole response
  }

  LOG_W(kTag, "rejected chat %lld (%s)", (long long)chatId, from.c_str());
  sendMessage(chatId,
              F("This bridge already has an owner. Ask them to add your chat "
                "id in the web interface."),
              false);
  return false;
}

void TelegramService::handleMessage(int64_t chatId, const String &from,
                                    const String &text) {
  if (chatId == 0) return;
  messagesHandled_++;

  if (!authorise(chatId, from)) return;
  if (!cfg::settings.isSourceEnabled(src::Source::Telegram)) {
    sendMessage(chatId, F("Telegram control is switched off for this device."),
                false);
    return;
  }

  String command = text;
  command.trim();
  const int space = command.indexOf(' ');
  String argument = space > 0 ? command.substring(space + 1) : String();
  if (space > 0) command = command.substring(0, space);
  command.toLowerCase();
  argument.trim();

  ac::Delta delta;
  bool hasDelta = false;

  if (command == "/start" || command == "/menu" || command == "/status") {
    sendMessage(chatId, describeState(), true);
    return;
  }
  if (command == "/on" || command == "/off") {
    delta.hasPower = true;
    delta.power = command == "/on";
    hasDelta = true;
  } else if (command == "/temp") {
    delta.hasDegrees = true;
    delta.degrees = argument.toFloat();
    hasDelta = true;
  } else if (command == "/mode") {
    if (!ac::parseMode(argument.c_str(), delta.mode)) {
      sendMessage(chatId, F("Modes: auto, cool, heat, dry, fan_only"), false);
      return;
    }
    delta.hasMode = true;
    delta.hasPower = true;
    delta.power = true;
    hasDelta = true;
  } else if (command == "/fan") {
    if (!ac::parseFan(argument.c_str(), delta.fan)) {
      sendMessage(chatId,
                  F("Fan speeds: auto, min, low, medium, medium_high, high, max"),
                  false);
      return;
    }
    delta.hasFan = true;
    hasDelta = true;
  } else if (command == "/scene") {
    if (argument.isEmpty()) {
      String list = F("Scenes:\n");
      for (const app::Scene &scene : app::scenes.all()) {
        list += scene.icon;
        list += ' ';
        list += scene.name;
        list += '\n';
      }
      list += F("\nSend /scene <name>");
      sendMessage(chatId, list, false);
      return;
    }
    String error;
    if (!app::scenes.apply(argument, src::Source::Telegram, error)) {
      sendMessage(chatId, String(F("Not applied: ")) + error, false);
      return;
    }
    sendMessage(chatId, describeState(), true);
    return;
  } else if (command == "/resend") {
    bus::commands.resend(src::Source::Telegram);
    sendMessage(chatId, F("Sent the current state again."), false);
    return;
  } else if (command == "/learn") {
    String error;
    if (!learn::wizard.startIdentify(error)) {
      sendMessage(chatId, String(F("Cannot start learning: ")) + error, false);
      return;
    }
    sendMessage(chatId,
                F("Learning started.\n\nPoint your air-conditioner remote at "
                  "the SLWF-12 and press any button. I will tell you what I "
                  "hear."),
                false);
    return;
  } else if (command == "/cancel") {
    learn::wizard.cancel();
    sendMessage(chatId, F("Learning cancelled."), false);
    return;
  } else if (command == "/help") {
    sendMessage(chatId,
                F("/menu — buttons\n/status — what the AC is doing\n"
                  "/on, /off\n/temp 24\n/mode cool\n/fan auto\n"
                  "/scene — list scenes; /scene night to apply one\n"
                  "/resend — send the current state again\n"
                  "/learn — teach me your remote\n/cancel"),
                false);
    return;
  } else {
    sendMessage(chatId, F("I did not understand that. Try /help."), false);
    return;
  }

  if (!hasDelta) return;

  const bus::Outcome outcome = bus::commands.apply(delta, src::Source::Telegram);
  if (!outcome.ok()) {
    sendMessage(chatId, String(F("Not applied: ")) + outcome.message, false);
    return;
  }
  sendMessage(chatId, describeState(), true);
}

void TelegramService::handleCallback(int64_t chatId, const String &queryId,
                                     const String &data, int32_t messageId) {
  if (chatId == 0) return;
  if (!cfg::settings.telegram.isAllowed(chatId)) {
    answerCallback(queryId, F("Not authorised"));
    return;
  }

  ac::Delta delta;
  const ac::State &state = bus::commands.state();

  if (data == "pw") {
    delta.hasPower = true;
    delta.power = !state.power;
  } else if (data == "t+") {
    delta.hasDegrees = true;
    delta.degrees = state.degrees + cfg::settings.ac.tempStep;
  } else if (data == "t-") {
    delta.hasDegrees = true;
    delta.degrees = state.degrees - cfg::settings.ac.tempStep;
  } else if (data.startsWith("md:")) {
    if (ac::parseMode(data.substring(3).c_str(), delta.mode)) {
      delta.hasMode = true;
      delta.hasPower = true;
      delta.power = true;
    }
  } else if (data.startsWith("fn:")) {
    if (ac::parseFan(data.substring(3).c_str(), delta.fan)) delta.hasFan = true;
  } else if (data.startsWith("sc:")) {
    String error;
    app::scenes.apply(data.substring(3), src::Source::Telegram, error);
    answerCallback(queryId, error);
    editMessage(chatId, messageId, describeState(), true);
    return;
  } else if (data == "rs") {
    bus::commands.resend(src::Source::Telegram);
    answerCallback(queryId, F("Resent"));
    editMessage(chatId, messageId, describeState(), true);
    return;
  } else if (data == "yes" || data == "no") {
    learn::wizard.confirm(data == "yes");
    answerCallback(queryId, F("Noted"));
    notifyLearning();
    return;
  }

  if (delta.empty()) {
    answerCallback(queryId, "");
    return;
  }

  const bus::Outcome outcome = bus::commands.apply(delta, src::Source::Telegram);
  answerCallback(queryId, outcome.ok() ? String() : outcome.message);
  editMessage(chatId, messageId, describeState(), true);
}

// ---------------------------------------------------------------------------

String TelegramService::describeState() const {
  const ac::State &state = bus::commands.state();

  String text;
  text.reserve(220);
  text += cfg::settings.device.name;
  text += "\n\n";

  if (!state.power) {
    text += F("Off");
  } else {
    text += F("On · ");
    text += ac::modeName(state.mode);
    text += F(" · ");
    text += String(state.degrees, state.degrees == (int)state.degrees ? 0 : 1);
    text += state.celsius ? F("°C") : F("°F");
    text += F("\nFan: ");
    text += ac::fanName(state.fanspeed);
    if (state.swingv != stdAc::swingv_t::kOff) {
      text += F(" · swing ");
      text += ac::swingVName(state.swingv);
    }
  }

  text += F("\n\nLast changed by: ");
  text += src::name(bus::commands.lastSource());

  if (!ir::irService.ready()) {
    text += F("\n\n⚠ No air conditioner is configured yet. Send /learn.");
  }
  return text;
}

void TelegramService::buildKeyboard(JsonObject markup) {
  JsonArray rows = markup["inline_keyboard"].to<JsonArray>();

  JsonArray row1 = rows.add<JsonArray>();
  JsonObject power = row1.add<JsonObject>();
  power["text"] = bus::commands.state().power ? "⏻ Off" : "⏻ On";
  power["callback_data"] = "pw";
  JsonObject down = row1.add<JsonObject>();
  down["text"] = "−";
  down["callback_data"] = "t-";
  JsonObject up = row1.add<JsonObject>();
  up["text"] = "+";
  up["callback_data"] = "t+";

  JsonArray row2 = rows.add<JsonArray>();
  const char *const modes[] = {"cool", "heat", "dry", "fan_only", "auto"};
  const char *const labels[] = {"❄ Cool", "☀ Heat", "💧 Dry", "🌀 Fan", "🅰 Auto"};
  for (uint8_t i = 0; i < 5; i++) {
    JsonObject button = row2.add<JsonObject>();
    button["text"] = labels[i];
    button["callback_data"] = String("md:") + modes[i];
  }

  JsonArray row3 = rows.add<JsonArray>();
  const char *const fans[] = {"auto", "low", "medium", "high"};
  for (const char *fan : fans) {
    JsonObject button = row3.add<JsonObject>();
    button["text"] = fan;
    button["callback_data"] = String("fn:") + fan;
  }

  if (app::scenes.count() > 0) {
    JsonArray sceneRow = rows.add<JsonArray>();
    for (const app::Scene &scene : app::scenes.all()) {
      JsonObject button = sceneRow.add<JsonObject>();
      button["text"] = scene.icon.isEmpty() ? scene.name
                                            : scene.icon + " " + scene.name;
      button["callback_data"] = String("sc:") + scene.id;
    }
  }

  JsonArray lastRow = rows.add<JsonArray>();
  JsonObject resend = lastRow.add<JsonObject>();
  resend["text"] = "↻ Resend";
  resend["callback_data"] = "rs";
}

void TelegramService::sendMessage(int64_t chatId, const String &text,
                                  bool withKeyboard) {
  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["text"] = jsonEscapeFree(text);
  doc["disable_notification"] = true;
  if (withKeyboard) buildKeyboard(doc["reply_markup"].to<JsonObject>());
  enqueue("sendMessage", doc);
}

void TelegramService::editMessage(int64_t chatId, int32_t messageId,
                                  const String &text, bool withKeyboard) {
  if (messageId == 0) {
    sendMessage(chatId, text, withKeyboard);
    return;
  }
  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["message_id"] = messageId;
  doc["text"] = jsonEscapeFree(text);
  if (withKeyboard) buildKeyboard(doc["reply_markup"].to<JsonObject>());
  enqueue("editMessageText", doc);
}

void TelegramService::answerCallback(const String &queryId, const String &text) {
  if (queryId.isEmpty()) return;
  JsonDocument doc;
  doc["callback_query_id"] = queryId;
  if (!text.isEmpty()) doc["text"] = jsonEscapeFree(text);
  enqueue("answerCallbackQuery", doc);
}

void TelegramService::broadcast(const String &text) {
  const cfg::TelegramSettings &t = cfg::settings.telegram;
  for (uint8_t i = 0; i < t.allowedCount; i++) sendMessage(t.allowed[i], text, false);
}

// ---------------------------------------------------------------------------

void TelegramService::onStateChanged(const ac::State &state,
                                     src::Source source) {
  (void)state;
  if (phase_ == Phase::Disabled) return;
  if (!cfg::settings.telegram.notifyOnChange) return;
  // Do not narrate the user's own button presses back at them.
  if (source == src::Source::Telegram) return;

  String text = describeState();
  text += F("\n(changed via ");
  text += src::name(source);
  text += ')';
  broadcast(text);
}

void TelegramService::notifyLearning() {
  if (phase_ == Phase::Disabled) return;

  JsonDocument doc;
  JsonObject status = doc.to<JsonObject>();
  learn::wizard.statusJson(status);

  String text = F("Learning: ");
  text += status["phase"].as<const char *>();
  const char *prompt = status["prompt"];
  if (prompt != nullptr && prompt[0] != '\0') {
    text += '\n';
    text += prompt;
  }
  const char *candidate = status["candidate"];
  if (candidate != nullptr) {
    text += F("\nProtocol heard: ");
    text += candidate;
  }
  broadcast(text);
}

// ---------------------------------------------------------------------------

void TelegramService::statusJson(JsonObject out) const {
  const char *names[] = {"disabled", "idle", "connecting", "sending",
                         "reading", "backoff"};
  out["enabled"] = phase_ != Phase::Disabled;
  out["phase"] = names[static_cast<uint8_t>(phase_)];
  out["connected"] = const_cast<TelegramService *>(this)->connected();
  out["authorisedChats"] = cfg::settings.telegram.allowedCount;
  out["messages"] = messagesHandled_;
  out["reconnects"] = reconnects_;
  out["fragmentNegotiation"] = mflnSupported_;
  out["tlsBufferBytes"] = rxBufferBytes_;
  out["freeHeap"] = ESP.getFreeHeap();
  out["queue"] = queue_.size();
  if (!lastError_.isEmpty()) out["lastError"] = lastError_;
}

}  // namespace net
