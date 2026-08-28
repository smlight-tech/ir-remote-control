#include "Peers.h"

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>

#include "../core/Log.h"
#include "../core/Settings.h"

namespace app {
namespace {
const char *kTag = "peers";
const char *kPath = "/peers.json";

// Services worth browsing. "slwf" is what this firmware advertises; "wled" is
// what a WLED controller advertises, which is how an SLWF-03 turns up.
//
// The service a device answers on is only a *hint* at its type. Identification
// proper is the browser's job — it fetches the candidate's status endpoint and
// matches it against devicetypes.json — because that path has to exist anyway
// for devices added by address, and duplicating it here would mean two places
// to keep correct.
struct Browse {
  const char *service;
  const char *hint;
};
const Browse kBrowses[] = {
    {"slwf", "slwf12"},
    {"wled", "slwf03"},
};
const char *kProtocol = "tcp";
}  // namespace

Peers peers;

// ---------------------------------------------------------------------------

void Peers::begin() {
  load();
  if (!peers_.empty()) LOG_I(kTag, "%u paired device(s)", (unsigned)peers_.size());
}

void Peers::loop() {
  // Nothing periodic. Peers are contacted by the browser, not by this device:
  // polling eight peers from an ESP8266 would cost heap and buy nothing that
  // the page cannot do for itself.
}

const Peer *Peers::find(const String &id) const {
  for (const Peer &peer : peers_)
    if (peer.id == id) return &peer;
  return nullptr;
}

// ---------------------------------------------------------------------------

void Peers::discover() {
  // MDNS.queryService blocks for the whole browse window. That is why this is
  // only ever called from an explicit "look for devices" action and never on a
  // timer: a two-second stall would drop infrared frames, which cannot be
  // recovered, and nobody would connect the two symptoms.
  discovering_ = true;
  candidates_.clear();

  const String ourAddress = WiFi.localIP().toString();

  for (const Browse &browse : kBrowses) {
    LOG_I(kTag, "browsing for _%s._%s", browse.service, kProtocol);
    const uint32_t found = MDNS.queryService(browse.service, kProtocol);

    for (uint32_t i = 0; i < found; i++) {
      Candidate candidate;
      const char *hostname = MDNS.answerHostname(i);
      candidate.hostname = hostname != nullptr ? hostname : "";
      candidate.host = MDNS.answerIP(i).toString();
      candidate.port = MDNS.answerPort(i);
      candidate.typeId = browse.hint;
      candidate.name = candidate.hostname;

      // Never offer to pair with ourselves.
      if (candidate.host == ourAddress) continue;

      bool duplicate = false;
      for (const Candidate &seen : candidates_) {
        if (seen.host == candidate.host) { duplicate = true; break; }
      }
      if (duplicate) continue;

      for (const Peer &peer : peers_) {
        if (peer.host == candidate.host || peer.host == candidate.hostname) {
          candidate.known = true;
          break;
        }
      }
      candidates_.push_back(candidate);
    }
    MDNS.removeQuery();
  }

  discovering_ = false;
  lastDiscoveryAt_ = millis();
  LOG_I(kTag, "found %u candidate device(s)", (unsigned)candidates_.size());
}

void Peers::candidatesJson(JsonArray out) const {
  for (const Candidate &candidate : candidates_) {
    JsonObject entry = out.add<JsonObject>();
    entry["host"] = candidate.host;
    entry["hostname"] = candidate.hostname;
    entry["port"] = candidate.port;
    entry["type"] = candidate.typeId;
    entry["name"] = candidate.name;
    entry["known"] = candidate.known;
  }
}

// ---------------------------------------------------------------------------

void Peers::toJson(JsonArray out) const {
  for (const Peer &peer : peers_) {
    JsonObject entry = out.add<JsonObject>();
    entry["id"] = peer.id;
    entry["type"] = peer.typeId;
    entry["name"] = peer.name;
    entry["host"] = peer.host;
    entry["port"] = peer.port;
    entry["enabled"] = peer.enabled;
    entry["entity"] = peer.entity;
    // The token is a credential for somebody else's device; it goes out only
    // as a flag, exactly like this device's own secrets.
    entry["hasToken"] = !peer.token.isEmpty();
  }
}

bool Peers::fromJson(JsonArrayConst in, String &error) {
  if (in.size() > kMaxPeers) {
    error = String(F("at most ")) + kMaxPeers + F(" devices can be paired");
    return false;
  }

  std::vector<Peer> parsed;
  for (JsonObjectConst entry : in) {
    Peer peer;
    peer.name = entry["name"] | "";
    peer.host = entry["host"] | "";
    peer.typeId = entry["type"] | "";
    peer.port = entry["port"] | 80;
    peer.enabled = entry["enabled"] | true;
    peer.entity = entry["entity"] | "";

    if (peer.host.isEmpty()) {
      error = F("every paired device needs an address");
      return false;
    }
    if (peer.typeId.isEmpty()) {
      error = String(F("'")) + peer.name + F("' has no device type");
      return false;
    }
    if (peer.name.isEmpty()) peer.name = peer.host;

    peer.id = entry["id"] | "";
    if (peer.id.isEmpty()) peer.id = peer.host;

    // An absent token means "keep whatever was stored"; an explicit empty
    // string clears it. Same rule the device uses for its own secrets.
    if (entry["token"].is<const char *>()) {
      peer.token = entry["token"].as<const char *>();
    } else {
      const Peer *existing = find(peer.id);
      if (existing != nullptr) peer.token = existing->token;
    }

    parsed.push_back(peer);
  }

  peers_ = parsed;
  return true;
}

// ---------------------------------------------------------------------------

bool Peers::load() {
  File file = LittleFS.open(kPath, "r");
  if (!file) return false;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    LOG_W(kTag, "peers parse error: %s", err.c_str());
    return false;
  }

  // Tokens are stored, so read them straight rather than through fromJson's
  // keep-what-you-had rule.
  peers_.clear();
  for (JsonObjectConst entry : doc.as<JsonArrayConst>()) {
    Peer peer;
    peer.id = entry["id"] | "";
    peer.typeId = entry["type"] | "";
    peer.name = entry["name"] | "";
    peer.host = entry["host"] | "";
    peer.port = entry["port"] | 80;
    peer.token = entry["token"] | "";
    peer.entity = entry["entity"] | "";
    peer.enabled = entry["enabled"] | true;
    if (!peer.host.isEmpty()) peers_.push_back(peer);
  }
  return true;
}

bool Peers::save() {
  JsonDocument doc;
  JsonArray root = doc.to<JsonArray>();
  for (const Peer &peer : peers_) {
    JsonObject entry = root.add<JsonObject>();
    entry["id"] = peer.id;
    entry["type"] = peer.typeId;
    entry["name"] = peer.name;
    entry["host"] = peer.host;
    entry["port"] = peer.port;
    entry["token"] = peer.token;
    entry["entity"] = peer.entity;
    entry["enabled"] = peer.enabled;
  }

  File file = LittleFS.open(kPath, "w");
  if (!file) {
    LOG_E(kTag, "cannot write %s", kPath);
    return false;
  }
  serializeJson(doc, file);
  file.close();
  LOG_I(kTag, "%u paired device(s) saved", (unsigned)peers_.size());
  return true;
}

}  // namespace app
