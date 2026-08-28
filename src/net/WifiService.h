// Wi-Fi lifecycle and the first-run setup access point.
//
// Boot behaviour:
//   * no credentials stored  -> setup AP immediately
//   * credentials stored     -> join, and if the join fails raise the setup AP
//                               *while still retrying* in the background, so a
//                               router that comes back up later is picked up
//                               without anyone touching the device
//
// While the AP is up a DNS server answers every query with the device's own
// address, which is what makes phones pop the "sign in to network" sheet.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESP8266WiFi.h>

namespace net {

enum class WifiState : uint8_t {
  Idle,
  Connecting,
  Connected,
  SetupPortal,   // AP is up because we have no usable credentials
  Failed,
};

class WifiService {
 public:
  void begin();
  void loop();

  WifiState state() const { return state_; }
  bool isOnline() const { return state_ == WifiState::Connected; }
  bool portalActive() const { return apActive_; }

  IPAddress ip() const;
  String hostname() const;

  // Applies new credentials and reconnects. Returns immediately; watch
  // state() for the outcome.
  void connectTo(const String &ssid, const String &pass);

  void startPortal();
  void stopPortal();

  // Asynchronous scan. `scanJson` returns the most recent results.
  void requestScan();
  // Releases the results the SDK is holding. Worth doing once the list has
  // been read: on this chip they are a meaningful fraction of free memory.
  void forgetScan();
  bool scanInProgress() const { return scanRunning_; }
  void scanJson(JsonArray out) const;

  void statusJson(JsonObject out) const;

  static String signalQuality(int32_t rssi);

 private:
  void applyStaticIp();
  void beginConnect();
  void pollScan();

  WifiState state_ = WifiState::Idle;
  DNSServer dns_;
  bool apActive_ = false;
  bool scanRunning_ = false;

  uint32_t connectStartedAt_ = 0;
  uint32_t lastRetryAt_ = 0;
  uint8_t attempts_ = 0;
};

extern WifiService wifi;

}  // namespace net
