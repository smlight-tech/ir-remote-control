// The firmware's view of devicetypes.json.
//
// The file is the single source of truth for what kinds of device exist, and
// it is data rather than code so that supporting a new one needs no rebuild.
// The browser reads all of it — labels, icons, the action lists that build the
// automation editor. This class reads only the parts the firmware itself needs
// in order to *talk* to a peer:
//
//   * how to reach the thing at all — the transport
//   * which endpoints to poll and command
//   * how to authenticate
//   * which state fields are worth caching
//
// Not everything worth automating speaks HTTP. A Wake-on-LAN target is not a
// device you can ask questions of; it is a MAC address and a magic packet. So
// the transport is declared by the family and the client dispatches on it,
// which means adding "things that are not web servers" costs a transport
// rather than a parallel notion of a service.
//
// Types are grouped into families because several product codes are the same
// thing underneath: SLWF-03, SLWF-09 and SLWF-11 are all WLED controllers, and
// nothing in the firmware should have to know that three times over.
#pragma once

#include <ArduinoJson.h>

#include <vector>

namespace app {

// How the firmware reaches a thing of this family.
enum class Transport : uint8_t {
  Http,   // a web server, polled and commanded — everything so far
  Wol,    // a MAC address and a magic packet; nothing to ask, only to tell
};

struct DeviceFamily {
  String id;
  Transport transport = Transport::Http;
  String statusPath;    // GET, to decide whether the peer is alive
  String statePath;     // GET, the values worth caching
  String commandPath;   // POST
  bool bearerAuth = false;
  // ESPHome takes commands as query parameters rather than a JSON body, so the
  // client has to know which shape to send.
  bool queryCommands = false;
  // ESPHome entity ids are chosen in each device's own configuration, so the
  // paths carry a {entity} placeholder filled in per paired device.
  bool needsEntity = false;
  // Field names to keep when caching a peer's state. Everything else in the
  // response is discarded, which is what keeps eight cached peers affordable.
  std::vector<String> fields;
};

class DeviceTypes {
 public:
  // Reads /devicetypes.json. Safe to call again after the filesystem image is
  // updated.
  //
  // Not called at boot. Parsing it costs about 1.8 kB that stays allocated,
  // and a device with nothing paired never needs it — the browser reads the
  // same file from the filesystem for itself. The first lookup loads it.
  bool load();

  const DeviceFamily *familyForType(const String &typeId) const;
  static const char *transportName(Transport transport);
  size_t familyCount() const { return families_.size(); }
  size_t typeCount() const { return typeToFamily_.size(); }

 private:
  struct TypeMapping {
    String typeId;
    String familyId;
  };

  bool loaded_ = false;

  std::vector<DeviceFamily> families_;
  std::vector<TypeMapping> typeToFamily_;
};

extern DeviceTypes deviceTypes;

}  // namespace app
