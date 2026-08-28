// Telegram bot client, written directly against the Bot API.
//
// Why not a library: on an ESP8266 the expensive part of talking to Telegram
// is not the protocol, it is TLS. A BearSSL session costs 6-22 kB of heap and
// a handshake costs seconds of CPU, so the only workable design is to open one
// connection, hold it, and long-poll on it — reconnecting rarely. Off-the-shelf
// bot libraries open a fresh connection per poll, which on this hardware means
// the radio and the CPU are never idle.
//
// The whole client is therefore a non-blocking state machine that owns exactly
// one TLS session and one request queue.
#pragma once

#include <ArduinoJson.h>
#include <WiFiClientSecureBearSSL.h>

#include <vector>

#include "../core/AcState.h"
#include "../core/Source.h"

namespace net {

class TelegramService {
 public:
  void begin();
  void loop();
  void reconfigure();

  // Called when the AC changes because of somebody else, so subscribed chats
  // stay in the picture.
  void onStateChanged(const ac::State &state, src::Source source);

  // Used by the learning wizard to walk a Telegram user through teaching.
  void notifyLearning();

  bool connected() const { return client_ != nullptr && client_->connected(); }
  void statusJson(JsonObject out) const;

 private:
  enum class Phase : uint8_t {
    Disabled,
    Idle,          // connected, nothing outstanding
    Connecting,
    Sending,
    Reading,
    Backoff,
  };

  struct Request {
    String method;
    String body;
    bool isPoll = false;
  };

  void enqueue(const String &method, const JsonDocument &body,
               bool isPoll = false);
  void queuePoll();

  bool openConnection();
  void closeConnection(const char *why);
  void writeRequest(const Request &request);
  void readResponse();
  // Returns true once the whole body has arrived.
  bool pumpBody();
  void resetResponse();
  void handleResponse(const Request &request, const String &body);

  void handleUpdate(JsonObjectConst update);
  void handleMessage(int64_t chatId, const String &from, const String &text);
  void handleCallback(int64_t chatId, const String &queryId, const String &data,
                      int32_t messageId);

  bool authorise(int64_t chatId, const String &from);
  void sendMessage(int64_t chatId, const String &text, bool withKeyboard);
  void editMessage(int64_t chatId, int32_t messageId, const String &text,
                   bool withKeyboard);
  void answerCallback(const String &queryId, const String &text);
  void broadcast(const String &text);

  String describeState() const;
  static void buildKeyboard(JsonObject markup);

  BearSSL::WiFiClientSecure *client_ = nullptr;

  Phase phase_ = Phase::Disabled;
  std::vector<Request> queue_;
  Request inFlight_;
  String responseBuffer_;
  bool headersDone_ = false;
  bool bodyDone_ = false;
  int32_t contentLength_ = -1;
  bool chunked_ = false;
  int32_t chunkRemaining_ = -1;   // -1 = expecting a chunk-size line

  int64_t updateOffset_ = 0;
  uint32_t phaseStartedAt_ = 0;
  uint32_t backoffMs_ = 5000;
  uint32_t nextPollAt_ = 0;
  uint32_t messagesHandled_ = 0;
  uint32_t reconnects_ = 0;
  bool mflnSupported_ = false;
  bool mflnProbed_ = false;
  uint16_t rxBufferBytes_ = 0;
  String lastError_;
};

extern TelegramService telegram;

}  // namespace net
