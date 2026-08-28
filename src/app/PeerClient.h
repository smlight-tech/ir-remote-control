// Talking to paired devices.
//
// This is the *only* thing that speaks to a peer, and it lives on the device
// rather than in the browser for one decisive reason: an automation runs with
// nobody watching. "If the air conditioner is on, turn the lamp on" has to work
// at three in the morning with every phone asleep, so the firmware has to be
// able to poll and command peers itself. Once it can, there is no cross-origin
// request for a browser to make, peer credentials never leave the device, and
// the interface and the automation engine share one code path.
//
// Everything is asynchronous. A peer that has been unplugged must not stall the
// main loop, because a stalled loop drops infrared frames and nothing connects
// those two symptoms.
//
// Only a handful of fields per peer are cached — the ones the device's family
// declares as conditions — so eight paired devices cost a few hundred bytes
// rather than eight full status documents.
#pragma once

#include <ArduinoJson.h>
#include <ESPAsyncTCP.h>

#include <vector>

#include "Peers.h"

namespace app {

struct PeerState {
  String peerId;
  bool online = false;
  uint32_t lastSeenAt = 0;
  uint32_t failures = 0;
  String values;        // compact JSON of the cached fields
};

class PeerClient {
 public:
  void begin();
  void loop();

  // Forget cached state for peers that no longer exist, and poll new ones
  // promptly rather than waiting for the next cycle.
  void reconfigure();

  // Queues a command for a peer. The body is that device's own command shape —
  // {"power":true} for an SLWF-12, {"on":true} for a WLED controller — because
  // the automation editor built it from the same type database.
  bool command(const String &peerId, const JsonDocument &body, String &error);

  // Cached view, for automations and for the interface.
  bool online(const String &peerId) const;
  bool readValue(const String &peerId, const String &field,
                 JsonDocument &out) const;

  void statusJson(JsonArray out) const;

 private:
  enum class Kind : uint8_t { Poll, Command };

  struct Job {
    Kind kind = Kind::Poll;
    String peerId;
    String request;     // the whole HTTP request, built before connecting
    String host;
    uint16_t port = 80;
  };

  PeerState *stateFor(const String &peerId);
  const PeerState *stateFor(const String &peerId) const;

  bool enqueue(const Job &job);
  void dispatch();
  void finish(bool ok);
  void absorb(const String &body);

  String buildRequest(const String &method, const String &host, uint16_t port,
                      const String &path, const String &token,
                      const String &body) const;

  // Transports other than HTTP. These complete within the call — a magic
  // packet is one datagram with nothing to wait for — so they never touch the
  // queue.
  bool wake(const Peer &peer, String &error);

  AsyncClient client_;
  std::vector<Job> queue_;
  Job inFlight_;
  bool busy_ = false;

  String response_;
  bool headersDone_ = false;

  std::vector<PeerState> states_;
  uint32_t nextPollAt_ = 0;
  size_t pollCursor_ = 0;
};

extern PeerClient peerClient;

}  // namespace app
