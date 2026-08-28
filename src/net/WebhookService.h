// Outbound HTTP on every state change.
//
// This is the other half of "any client, via the API": the REST endpoints let
// anything drive the air conditioner, and this lets anything *follow* it
// without speaking MQTT. Node-RED, a home-grown logger, a bridge into some
// other system — anything that can receive a POST.
//
// Built on AsyncClient rather than HTTPClient on purpose. HTTPClient blocks
// until the request completes, and a webhook pointed at a host that has gone
// away would stall the main loop for seconds — long enough to miss infrared
// frames, which cannot be recovered. Nothing here waits.
//
// Plain http:// only. A second TLS session does not fit alongside everything
// else; if you need HTTPS, point this at something on your own network and let
// that forward it.
#pragma once

#include <ArduinoJson.h>
#include <ESPAsyncTCP.h>

#include "../core/AcState.h"
#include "../core/Source.h"

namespace net {

class WebhookService {
 public:
  void begin();
  void loop();
  void reconfigure();

  void onStateChanged(const ac::State &state, src::Source source);

  // Sends the current state immediately, ignoring the rate limit. Used by the
  // "test" button so a user can tell whether their URL works.
  bool sendNow(String &error);

  void statusJson(JsonObject out) const;

 private:
  bool parseUrl(String &host, uint16_t &port, String &path) const;
  String buildRequest(const ac::State &state, src::Source source) const;
  void dispatch();
  void finish(bool ok, const String &detail);

  AsyncClient client_;
  bool inFlight_ = false;
  bool pending_ = false;

  String payload_;              // the request, built before connecting
  uint32_t lastSentAt_ = 0;
  uint32_t deliveries_ = 0;
  uint32_t failures_ = 0;
  uint32_t consecutiveFailures_ = 0;
  uint32_t retryNotBefore_ = 0;
  String lastError_;
  uint16_t lastStatus_ = 0;

  ac::State pendingState_;
  src::Source pendingSource_ = src::Source::System;
};

extern WebhookService webhooks;

}  // namespace net
