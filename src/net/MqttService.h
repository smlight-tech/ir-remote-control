// MQTT client, including Home Assistant auto-discovery.
//
// The device announces itself as a `climate` entity, so it shows up in Home
// Assistant as a thermostat card with no YAML at all. Two extra entities come
// along for free: a signal-strength sensor and a "resend" button for the times
// an IR command does not land.
//
// State is published as one JSON document plus a handful of plain-text topics.
// Home Assistant reads the JSON through value templates; anything else — Node
// RED, a shell script, another controller — can subscribe to the simple topics.
#pragma once

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFiClient.h>

#include "../core/AcState.h"
#include "../core/Source.h"

namespace net {

class MqttService {
 public:
  void begin();
  void loop();

  // Re-reads the settings and reconnects. Called when the user saves changes.
  void reconfigure();

  void publishState(bool force = false);
  void publishDiscovery();
  void publishAvailability(bool online);

  // The Homie 4.0 convention — a second, self-describing topic tree for
  // controllers that do not speak Home Assistant discovery.
  void publishHomieDescription();
  void publishHomieState(const ac::State &state);

  bool connected() const;
  void statusJson(JsonObject out) const;

 private:
  void connect();
  void subscribeAll();
  void onMessage(char *topic, const uint8_t *payload, unsigned int length);
  void handleCommand(const String &leaf, const String &payload);

  String topic(const char *leaf) const;
  String homieTopic(const char *leaf) const;
  bool handleHomieCommand(const String &fullTopic, const String &payload);
  String uniqueId() const;
  void publishDiscoveryFor(const char *component, const char *objectId,
                           JsonDocument &doc);
  void addDeviceBlock(JsonObject parent) const;

  WiFiClient net_;
  PubSubClient client_{net_};

  bool enabled_ = false;
  bool discoveryPublished_ = false;
  uint32_t lastAttemptAt_ = 0;
  uint32_t retryDelayMs_ = 5000;
  uint32_t lastPublishAt_ = 0;
  uint32_t lastRevision_ = 0xFFFFFFFF;
  uint32_t connectCount_ = 0;
  String lastError_;
};

extern MqttService mqtt;

}  // namespace net
