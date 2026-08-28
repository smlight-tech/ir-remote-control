// Firmware and filesystem updates.
//
// Three routes in, because different people update in different ways:
//   * drag a .bin onto the web UI                       (POST /api/ota/upload)
//   * point the device at a plain-HTTP URL              (POST /api/ota/url)
//   * push over the network from PlatformIO             (ArduinoOTA / espota)
//
// The device deliberately does not poll GitHub over TLS. A BearSSL session
// costs more heap than an ESP8266 running a web server, MQTT and a TLS-based
// Telegram poller can spare; the browser checks for new releases instead and
// hands the device a URL only when the user asks for it.
#pragma once

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

namespace net {

class OtaService {
 public:
  void begin();
  void loop();

  void registerRoutes(AsyncWebServer &server);

  // Restarts after `delayMs`, giving the HTTP response time to be flushed.
  void scheduleRestart(uint32_t delayMs);

  void statusJson(JsonObject out) const;

  bool inProgress() const { return inProgress_; }

 private:
  bool beginUpdate(const String &filename, size_t totalSize);

  bool inProgress_ = false;
  bool isFilesystem_ = false;
  String lastError_;
  uint8_t progressPercent_ = 0;

  uint32_t restartAt_ = 0;
};

extern OtaService ota;

}  // namespace net
