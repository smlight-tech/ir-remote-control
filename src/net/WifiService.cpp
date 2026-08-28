#include "WifiService.h"

#include <ESP8266mDNS.h>

#include "../core/Log.h"
#include "../core/Settings.h"

namespace net {
namespace {
const char *kTag = "wifi";
const uint32_t kConnectTimeoutMs = 20000;
const uint32_t kRetryIntervalMs = 30000;
const uint8_t kDnsPort = 53;
const IPAddress kApAddress(192, 168, 4, 1);

// Advertised so other SLWF devices can discover this one. The type string is
// the id used in devicetypes.json.
const char *kPeerService = "slwf";
const char *kDeviceType = "slwf12";
}  // namespace

WifiService wifi;

// ---------------------------------------------------------------------------

void WifiService::begin() {
  WiFi.persistent(false);          // we own the credentials, not the SDK
  WiFi.setAutoReconnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);  // modem sleep adds latency to IR->MQTT
  WiFi.hostname(hostname());

  if (cfg::settings.wifi.ssid.isEmpty()) {
    LOG_I(kTag, "no credentials stored, raising the setup portal");
    startPortal();
    return;
  }

  beginConnect();
}

String WifiService::hostname() const {
  String name = cfg::settings.device.hostname;
  if (name.isEmpty()) name = String("slwf12-") + cfg::settings.chipId();
  return name;
}

IPAddress WifiService::ip() const {
  return apActive_ && state_ != WifiState::Connected ? WiFi.softAPIP()
                                                     : WiFi.localIP();
}

void WifiService::applyStaticIp() {
  const cfg::WifiSettings &w = cfg::settings.wifi;
  if (!w.useStatic) {
    WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0),
                IPAddress(0, 0, 0, 0));
    return;
  }
  if (!WiFi.config(w.ip, w.gateway, w.mask, w.dns)) {
    LOG_W(kTag, "static IP configuration rejected, falling back to DHCP");
  }
}

void WifiService::beginConnect() {
  const cfg::WifiSettings &w = cfg::settings.wifi;

  WiFi.mode(apActive_ ? WIFI_AP_STA : WIFI_STA);
  applyStaticIp();
  WiFi.begin(w.ssid.c_str(), w.pass.c_str());

  state_ = WifiState::Connecting;
  connectStartedAt_ = millis();
  attempts_++;
  LOG_I(kTag, "connecting to '%s' (attempt %u)", w.ssid.c_str(), attempts_);
}

void WifiService::connectTo(const String &ssid, const String &pass) {
  cfg::settings.wifi.ssid = ssid;
  cfg::settings.wifi.pass = pass;
  cfg::settings.touch();
  cfg::settings.save();   // credentials are worth an immediate write
  attempts_ = 0;
  beginConnect();
}

// ---------------------------------------------------------------------------

void WifiService::startPortal() {
  if (apActive_) return;

  const String ssid = String("SLWF-12 setup ") + cfg::settings.chipId();
  const String pass = cfg::settings.wifi.apPassword;

  WiFi.mode(state_ == WifiState::Connecting ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAPConfig(kApAddress, kApAddress, IPAddress(255, 255, 255, 0));

  const bool ok = pass.length() >= 8 ? WiFi.softAP(ssid.c_str(), pass.c_str())
                                     : WiFi.softAP(ssid.c_str());
  if (!ok) {
    LOG_E(kTag, "could not start the access point");
    return;
  }

  dns_.setErrorReplyCode(DNSReplyCode::NoError);
  dns_.start(kDnsPort, "*", kApAddress);

  apActive_ = true;
  if (state_ != WifiState::Connecting) state_ = WifiState::SetupPortal;
  LOG_I(kTag, "setup portal up: SSID '%s', http://%s/", ssid.c_str(),
        kApAddress.toString().c_str());
}

void WifiService::stopPortal() {
  if (!apActive_) return;
  dns_.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive_ = false;
  LOG_I(kTag, "setup portal closed");
}

// ---------------------------------------------------------------------------

void WifiService::loop() {
  if (apActive_) dns_.processNextRequest();
  if (scanRunning_) pollScan();

  const bool connected = WiFi.status() == WL_CONNECTED;

  switch (state_) {
    case WifiState::Connecting:
      if (connected) {
        state_ = WifiState::Connected;
        attempts_ = 0;
        LOG_I(kTag, "connected to '%s' as %s (%s)", WiFi.SSID().c_str(),
              WiFi.localIP().toString().c_str(),
              signalQuality(WiFi.RSSI()).c_str());

        if (MDNS.begin(hostname())) {
          MDNS.addService("http", "tcp", 80);

          // A service of our own, so other SLWF devices can find this one and
          // know what it is without probing. The TXT records carry enough for
          // a pairing dialogue to show a name and a type straight away.
          MDNS.addService(kPeerService, "tcp", 80);
          MDNS.addServiceTxt(kPeerService, "tcp", "type", kDeviceType);
          MDNS.addServiceTxt(kPeerService, "tcp", "id",
                             cfg::settings.chipId().c_str());
          MDNS.addServiceTxt(kPeerService, "tcp", "name",
                             cfg::settings.device.name.c_str());

          LOG_I(kTag, "reachable at http://%s.local/", hostname().c_str());
        }
        // Give the user a moment to see the success page before the AP drops.
        if (apActive_) {
          lastRetryAt_ = millis();
        }
      } else if (millis() - connectStartedAt_ > kConnectTimeoutMs) {
        LOG_W(kTag, "connection attempt timed out");
        state_ = WifiState::Failed;
        lastRetryAt_ = millis();
        if (!apActive_) startPortal();
      }
      break;

    case WifiState::Connected:
      if (!connected) {
        LOG_W(kTag, "connection lost");
        state_ = WifiState::Connecting;
        connectStartedAt_ = millis();
        break;
      }
      MDNS.update();
      // Once the station link is solid there is no reason to keep the setup
      // AP radiating; close it a few seconds after we come up.
      if (apActive_ && millis() - lastRetryAt_ > 8000) stopPortal();
      break;

    case WifiState::Failed:
    case WifiState::SetupPortal:
      if (!cfg::settings.wifi.ssid.isEmpty() &&
          millis() - lastRetryAt_ > kRetryIntervalMs) {
        lastRetryAt_ = millis();
        beginConnect();
      }
      break;

    case WifiState::Idle:
      break;
  }
}

// ---------------------------------------------------------------------------

void WifiService::requestScan() {
  if (scanRunning_) return;
  // The SDK keeps the previous results until told otherwise, so a second scan
  // without this holds two sets of them — and the setup page asks for a scan
  // every time somebody opens it, on a device that has a few kilobytes of
  // heap to its name.
  WiFi.scanDelete();
  scanRunning_ = true;
  WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true);
  LOG_D(kTag, "scan started");
}

void WifiService::forgetScan() {
  WiFi.scanDelete();
}

void WifiService::pollScan() {
  const int8_t result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) return;
  scanRunning_ = false;
  if (result < 0) {
    LOG_W(kTag, "scan failed");
    return;
  }
  LOG_D(kTag, "scan found %d network(s)", result);
}

void WifiService::scanJson(JsonArray out) const {
  const int16_t count = WiFi.scanComplete();
  if (count <= 0) return;

  for (int16_t i = 0; i < count; i++) {
    JsonObject entry = out.add<JsonObject>();
    entry["ssid"] = WiFi.SSID(i);
    entry["rssi"] = WiFi.RSSI(i);
    entry["quality"] = signalQuality(WiFi.RSSI(i));
    entry["channel"] = WiFi.channel(i);
    entry["secure"] = WiFi.encryptionType(i) != ENC_TYPE_NONE;
  }
}

String WifiService::signalQuality(int32_t rssi) {
  if (rssi >= -55) return F("excellent");
  if (rssi >= -67) return F("good");
  if (rssi >= -75) return F("fair");
  return F("weak");
}

void WifiService::statusJson(JsonObject out) const {
  const char *names[] = {"idle", "connecting", "connected", "portal", "failed"};
  out["state"] = names[static_cast<uint8_t>(state_)];
  out["online"] = isOnline();
  out["portal"] = apActive_;
  out["hostname"] = hostname();
  out["mac"] = WiFi.macAddress();

  if (state_ == WifiState::Connected) {
    out["ssid"] = WiFi.SSID();
    out["ip"] = WiFi.localIP().toString();
    out["gateway"] = WiFi.gatewayIP().toString();
    out["rssi"] = WiFi.RSSI();
    out["quality"] = signalQuality(WiFi.RSSI());
  }
  if (apActive_) {
    out["apSsid"] = WiFi.softAPSSID();
    out["apIp"] = WiFi.softAPIP().toString();
    out["apClients"] = WiFi.softAPgetStationNum();
  }
}

}  // namespace net
