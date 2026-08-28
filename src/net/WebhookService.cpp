#include "WebhookService.h"

#include <ESP8266WiFi.h>

#include "../core/CommandBus.h"
#include "../core/Log.h"
#include "../core/Settings.h"
#include "generated/version.h"

namespace net {
namespace {
const char *kTag = "hook";

// A URL that has stopped answering should not be retried on every change.
const uint32_t kBackoffStepMs = 15000;
const uint32_t kMaxBackoffMs = 300000;
const uint8_t kFailuresBeforeBackoff = 3;
}  // namespace

WebhookService webhooks;

// ---------------------------------------------------------------------------

void WebhookService::begin() {
  // Handlers are installed once and the client is reused, so there is no
  // object lifetime to get wrong inside a callback.
  client_.onConnect([](void *arg, AsyncClient *client) {
    WebhookService *self = static_cast<WebhookService *>(arg);
    client->write(self->payload_.c_str(), self->payload_.length());
  }, this);

  client_.onData([](void *arg, AsyncClient *client, void *data, size_t len) {
    (void)client;
    WebhookService *self = static_cast<WebhookService *>(arg);
    // Only the status line matters; the body is of no interest to us.
    if (self->lastStatus_ != 0 || len < 12) return;
    const char *text = static_cast<const char *>(data);
    if (strncmp(text, "HTTP/1.", 7) == 0) {
      self->lastStatus_ = atoi(text + 9);
    }
  }, this);

  client_.onDisconnect([](void *arg, AsyncClient *client) {
    (void)client;
    WebhookService *self = static_cast<WebhookService *>(arg);
    const bool ok = self->lastStatus_ >= 200 && self->lastStatus_ < 400;
    self->finish(ok, ok ? String(self->lastStatus_)
                        : String(F("HTTP ")) + self->lastStatus_);
  }, this);

  client_.onError([](void *arg, AsyncClient *client, int8_t error) {
    (void)client;
    WebhookService *self = static_cast<WebhookService *>(arg);
    self->finish(false, String(F("connection error ")) + error);
  }, this);

  client_.onTimeout([](void *arg, AsyncClient *client, uint32_t time) {
    (void)time;
    client->close(true);
    static_cast<WebhookService *>(arg)->finish(false, F("timed out"));
  }, this);

  client_.setRxTimeout(5);   // seconds

  reconfigure();
}

void WebhookService::reconfigure() {
  consecutiveFailures_ = 0;
  retryNotBefore_ = 0;
  lastError_ = "";
  if (cfg::settings.webhook.enabled) {
    LOG_I(kTag, "webhook enabled: %s", cfg::settings.webhook.url.c_str());
  }
}

// ---------------------------------------------------------------------------

bool WebhookService::parseUrl(String &host, uint16_t &port, String &path) const {
  const String &url = cfg::settings.webhook.url;
  if (!url.startsWith("http://")) return false;

  const int start = 7;
  int slash = url.indexOf('/', start);
  if (slash < 0) slash = url.length();

  String authority = url.substring(start, slash);
  path = slash < static_cast<int>(url.length()) ? url.substring(slash) : "/";

  const int colon = authority.indexOf(':');
  if (colon >= 0) {
    port = authority.substring(colon + 1).toInt();
    host = authority.substring(0, colon);
  } else {
    port = 80;
    host = authority;
  }
  return host.length() > 0 && port > 0;
}

String WebhookService::buildRequest(const ac::State &state,
                                    src::Source source) const {
  JsonDocument doc;
  doc["device"] = cfg::settings.device.name;
  doc["id"] = cfg::settings.chipId();
  doc["firmware"] = FW_VERSION;
  doc["source"] = src::name(source);
  doc["revision"] = bus::commands.revision();
  JsonObject stateJson = doc["state"].to<JsonObject>();
  ac::toJson(state, stateJson);

  String body;
  serializeJson(doc, body);

  String host, path;
  uint16_t port = 80;
  parseUrl(host, port, path);

  String request;
  request.reserve(body.length() + 220);
  request += F("POST ");
  request += path;
  request += F(" HTTP/1.1\r\nHost: ");
  request += host;
  if (port != 80) {
    request += ':';
    request += port;
  }
  request += F("\r\nUser-Agent: SLWF-12\r\nContent-Type: application/json"
               "\r\nConnection: close\r\n");

  const cfg::WebhookSettings &hook = cfg::settings.webhook;
  if (!hook.headerName.isEmpty() && !hook.headerValue.isEmpty()) {
    request += hook.headerName;
    request += F(": ");
    request += hook.headerValue;
    request += F("\r\n");
  }

  request += F("Content-Length: ");
  request += body.length();
  request += F("\r\n\r\n");
  request += body;
  return request;
}

// ---------------------------------------------------------------------------

void WebhookService::onStateChanged(const ac::State &state, src::Source source) {
  if (!cfg::settings.webhook.enabled) return;
  // Keep only the newest state: a burst of changes should produce one delivery
  // describing where the air conditioner ended up, not five describing how it
  // got there.
  pendingState_ = state;
  pendingSource_ = source;
  pending_ = true;
}

void WebhookService::loop() {
  if (!pending_ || inFlight_) return;
  if (!cfg::settings.webhook.enabled) {
    pending_ = false;
    return;
  }
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() < retryNotBefore_) return;

  const uint32_t interval = cfg::settings.webhook.minIntervalSeconds * 1000UL;
  if (lastSentAt_ != 0 && millis() - lastSentAt_ < interval) return;

  dispatch();
}

void WebhookService::dispatch() {
  String host, path;
  uint16_t port = 80;
  if (!parseUrl(host, port, path)) {
    LOG_E(kTag, "cannot parse the webhook URL");
    pending_ = false;
    lastError_ = F("the URL could not be parsed");
    return;
  }

  payload_ = buildRequest(pendingState_, pendingSource_);
  pending_ = false;
  inFlight_ = true;
  lastStatus_ = 0;

  if (!client_.connect(host.c_str(), port)) {
    finish(false, F("could not start the connection"));
  }
}

void WebhookService::finish(bool ok, const String &detail) {
  if (!inFlight_) return;
  inFlight_ = false;
  lastSentAt_ = millis();

  if (ok) {
    deliveries_++;
    consecutiveFailures_ = 0;
    lastError_ = "";
    LOG_D(kTag, "delivered (%s)", detail.c_str());
    return;
  }

  failures_++;
  consecutiveFailures_++;
  lastError_ = detail;

  if (consecutiveFailures_ >= kFailuresBeforeBackoff) {
    const uint32_t wait =
        min(kBackoffStepMs * (consecutiveFailures_ - kFailuresBeforeBackoff + 1),
            kMaxBackoffMs);
    retryNotBefore_ = millis() + wait;
    LOG_W(kTag, "%lu failures in a row (%s); pausing for %lus",
          (unsigned long)consecutiveFailures_, detail.c_str(),
          (unsigned long)(wait / 1000));
  } else {
    LOG_W(kTag, "delivery failed: %s", detail.c_str());
  }
}

bool WebhookService::sendNow(String &error) {
  if (!cfg::settings.webhook.enabled) {
    error = F("the webhook is switched off");
    return false;
  }
  if (inFlight_) {
    error = F("a delivery is already in progress");
    return false;
  }
  pendingState_ = bus::commands.state();
  pendingSource_ = src::Source::System;
  pending_ = true;
  retryNotBefore_ = 0;
  lastSentAt_ = 0;      // bypass the rate limit for an explicit test
  dispatch();
  return true;
}

// ---------------------------------------------------------------------------

void WebhookService::statusJson(JsonObject out) const {
  out["enabled"] = cfg::settings.webhook.enabled;
  out["url"] = cfg::settings.webhook.url;
  out["deliveries"] = deliveries_;
  out["failures"] = failures_;
  out["inFlight"] = inFlight_;
  if (lastStatus_ != 0) out["lastStatus"] = lastStatus_;
  if (!lastError_.isEmpty()) out["lastError"] = lastError_;
  if (retryNotBefore_ > millis()) {
    out["pausedFor"] = (retryNotBefore_ - millis()) / 1000;
  }
}

}  // namespace net
