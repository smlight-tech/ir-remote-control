// HTTP + WebSocket front end.
//
// Serves the single-page UI straight out of LittleFS (pre-gzipped by
// tools/build_web.py), exposes the REST API under /api, and pushes live state
// to every open browser over /ws so the climate card tracks the AC even when
// somebody changes it from Telegram or from their own remote.
#pragma once

#include <ESPAsyncWebServer.h>

#include "../core/AcState.h"
#include "../core/Source.h"

namespace net {

class WebService {
 public:
  void begin();
  void loop();

  // Push helpers. Cheap no-ops when nobody is connected.
  void broadcastState(src::Source source);
  void broadcastLearning();
  void broadcastLog();

  // Whether anybody is watching the log. Clients ask with {"t":"log","on":…}
  // and stop asking when they leave the page; nothing is sent otherwise.
  void broadcastNotice(const char *level, const String &messageKey);

  AsyncWebServer &server() { return server_; }
  uint32_t clientCount() const;

 private:
  void registerStaticRoutes();
  void registerWebSocket();
  bool handleCaptivePortal(AsyncWebServerRequest *request);

  AsyncWebServer server_{80};
  AsyncWebSocket ws_{"/ws"};

  uint32_t lastLogSeqSent_ = 0;

  // Client ids that asked for the log. Small and linear on purpose: there are
  // never more than a handful of sockets, and a set would cost more than it
  // saves.
  static const uint8_t kMaxLogWatchers = 4;
  uint32_t logWatchers_[kMaxLogWatchers] = {0, 0, 0, 0};

  void watchLog(uint32_t clientId, bool wanted);
  bool anyoneWatchingLog() const;
  uint32_t lastCleanupAt_ = 0;
  bool statePending_ = false;
  uint32_t statePendingAt_ = 0;
};

extern WebService web;

}  // namespace net
