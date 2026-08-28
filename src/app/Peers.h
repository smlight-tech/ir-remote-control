// Paired devices.
//
// A peer is another device on the network that this one knows about: another
// SLWF-12, an SLWF-03 lighting controller, anything whose type appears in
// `devicetypes.json`. Pairing records *what kind of thing it is*, not merely an
// address, so that everything built on top later — automations, grouping,
// follow-the-leader behaviour — can ask "is this a climate device?" rather than
// assuming every peer is an air conditioner.
//
// Deliberately not in this layer:
//
//   * sending commands to peers. Which device drives which, and with what
//     offsets or delays, is an automation question. Answering it here would
//     bake air-conditioner assumptions into the pairing layer and make the
//     second device type awkward.
//   * knowing each type's protocol. The type database is a JSON file the
//     browser reads; the browser identifies a candidate and talks to it
//     directly. This firmware does the one job a browser cannot: mDNS.
//
// So this class stores peers, finds candidates, and reports them. That is all.
#pragma once

#include <ArduinoJson.h>

#include <vector>

namespace app {

struct Peer {
  String id;          // stable, from the device itself where possible
  String typeId;      // "slwf12", "slwf03", … as named in devicetypes.json
  String name;        // what the user calls it: "Bedroom"
  String host;        // hostname or address, no scheme
  uint16_t port = 80;
  String token;       // API token, when the peer requires one
  String entity;      // ESPHome entity id, substituted into {entity}
  bool enabled = true;
};

// Something seen on the network that is not paired yet.
struct Candidate {
  String host;
  String hostname;
  uint16_t port = 80;
  String typeId;      // from the mDNS TXT record, when the peer publishes one
  String name;
  bool known = false; // already in the peer list
};

class Peers {
 public:
  void begin();
  void loop();

  bool load();
  bool save();

  const std::vector<Peer> &all() const { return peers_; }
  size_t count() const { return peers_.size(); }
  const Peer *find(const String &id) const;

  void toJson(JsonArray out) const;
  bool fromJson(JsonArrayConst in, String &error);

  // Blocking mDNS browse. Only ever run from an explicit user action — see
  // the note in the implementation about why it cannot be a background task.
  void discover();
  void candidatesJson(JsonArray out) const;
  bool discovering() const { return discovering_; }
  uint32_t lastDiscoveryAt() const { return lastDiscoveryAt_; }

 private:
  static const uint8_t kMaxPeers = 8;

  std::vector<Peer> peers_;
  std::vector<Candidate> candidates_;
  bool discovering_ = false;
  uint32_t lastDiscoveryAt_ = 0;
};

extern Peers peers;

}  // namespace app
