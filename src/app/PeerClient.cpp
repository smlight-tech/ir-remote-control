#include "PeerClient.h"

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

#include "../core/Log.h"
#include "DeviceTypes.h"
#include "Peers.h"

namespace app {
namespace {
const char *kTag = "peerclient";

// One peer is polled per tick, round-robin, so eight devices are refreshed
// over eight ticks rather than in one burst that would need eight sockets.
const uint32_t kPollTickMs = 2500;

// A peer that has not answered this many times running is reported offline.
const uint32_t kFailuresBeforeOffline = 2;

// Responses are truncated past this. A filtered parse only needs the first few
// hundred bytes; WLED's full state document is far larger and none of the rest
// is wanted.
const size_t kMaxResponseBytes = 1536;

const uint8_t kMaxQueue = 8;
}  // namespace


namespace {

// ESPHome paths carry an {entity} placeholder because its entity ids are
// chosen in each device's own configuration rather than fixed by the type.
String withEntity(const String &path, const String &entity) {
  String out = path;
  out.replace("{entity}", entity);
  return out;
}

// ESPHome takes commands as query parameters, not a JSON body. Flattening the
// command object is enough: its actions are all scalars.
String toQuery(const JsonDocument &body) {
  String query;
  for (JsonPairConst pair : body.as<JsonObjectConst>()) {
    if (!query.isEmpty()) query += '&';
    query += pair.key().c_str();
    query += '=';
    JsonVariantConst value = pair.value();
    String text;
    if (value.is<bool>())        text = value.as<bool>() ? "true" : "false";
    else if (value.is<float>())  text = String(value.as<float>(), 2);
    else                         text = value.as<const char *>() ?: "";
    // Only the characters an action value can realistically contain.
    text.replace(" ", "%20");
    text.replace("&", "%26");
    text.replace("#", "%23");
    query += text;
  }
  return query;
}

// Accepts the three ways people write a MAC address, because they all turn up
// on the labels: aa:bb:cc:dd:ee:ff, aa-bb-cc-dd-ee-ff, aabbccddeeff.
bool parseMac(const String &text, uint8_t out[6]) {
  uint8_t written = 0;
  uint8_t nibbles = 0;
  uint8_t byte = 0;

  for (size_t i = 0; i < text.length(); i++) {
    const char c = text[i];
    if (c == ':' || c == '-' || c == '.' || c == ' ') continue;

    uint8_t value;
    if (c >= '0' && c <= '9')      value = c - '0';
    else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
    else return false;

    byte = (byte << 4) | value;
    if (++nibbles == 2) {
      if (written >= 6) return false;
      out[written++] = byte;
      nibbles = 0;
      byte = 0;
    }
  }
  return written == 6 && nibbles == 0;
}

}  // namespace

PeerClient peerClient;

// ---------------------------------------------------------------------------

void PeerClient::begin() {
  client_.onConnect([](void *arg, AsyncClient *client) {
    PeerClient *self = static_cast<PeerClient *>(arg);
    client->write(self->inFlight_.request.c_str(),
                  self->inFlight_.request.length());
  }, this);

  client_.onData([](void *arg, AsyncClient *client, void *data, size_t len) {
    (void)client;
    PeerClient *self = static_cast<PeerClient *>(arg);
    if (self->response_.length() >= kMaxResponseBytes) return;
    self->response_.concat(static_cast<const char *>(data), len);
  }, this);

  client_.onDisconnect([](void *arg, AsyncClient *client) {
    (void)client;
    static_cast<PeerClient *>(arg)->finish(true);
  }, this);

  client_.onError([](void *arg, AsyncClient *client, int8_t error) {
    (void)client;
    (void)error;
    static_cast<PeerClient *>(arg)->finish(false);
  }, this);

  client_.onTimeout([](void *arg, AsyncClient *client, uint32_t time) {
    (void)time;
    client->close(true);
    static_cast<PeerClient *>(arg)->finish(false);
  }, this);

  client_.setRxTimeout(4);
  reconfigure();
}

void PeerClient::reconfigure() {
  // Drop cached state for peers that have been removed.
  std::vector<PeerState> kept;
  for (const PeerState &state : states_) {
    if (peers.find(state.peerId) != nullptr) kept.push_back(state);
  }
  states_ = kept;
  pollCursor_ = 0;
  nextPollAt_ = 0;   // refresh promptly after a change
}

// ---------------------------------------------------------------------------

PeerState *PeerClient::stateFor(const String &peerId) {
  for (PeerState &state : states_)
    if (state.peerId == peerId) return &state;
  states_.push_back(PeerState{peerId, false, 0, 0, String()});
  return &states_.back();
}

const PeerState *PeerClient::stateFor(const String &peerId) const {
  for (const PeerState &state : states_)
    if (state.peerId == peerId) return &state;
  return nullptr;
}

bool PeerClient::online(const String &peerId) const {
  const PeerState *state = stateFor(peerId);
  return state != nullptr && state->online;
}

bool PeerClient::readValue(const String &peerId, const String &field,
                           JsonDocument &out) const {
  const PeerState *state = stateFor(peerId);
  if (state == nullptr || state->values.isEmpty()) return false;

  JsonDocument cached;
  if (deserializeJson(cached, state->values) != DeserializationError::Ok) {
    return false;
  }
  JsonVariantConst value = cached[field];
  if (value.isNull()) return false;

  out.set(value);
  return true;
}

// ---------------------------------------------------------------------------

String PeerClient::buildRequest(const String &method, const String &host,
                                uint16_t port, const String &path,
                                const String &token, const String &body) const {
  String request;
  request.reserve(body.length() + 200);
  request += method;
  request += ' ';
  request += path;
  request += F(" HTTP/1.1\r\nHost: ");
  request += host;
  if (port != 80) {
    request += ':';
    request += port;
  }
  request += F("\r\nUser-Agent: SLWF-12\r\nConnection: close\r\n");
  if (!token.isEmpty()) {
    request += F("Authorization: Bearer ");
    request += token;
    request += F("\r\n");
  }
  if (!body.isEmpty()) {
    request += F("Content-Type: application/json\r\nContent-Length: ");
    request += body.length();
    request += F("\r\n");
  }
  request += F("\r\n");
  request += body;
  return request;
}

bool PeerClient::enqueue(const Job &job) {
  if (queue_.size() >= kMaxQueue) {
    LOG_W(kTag, "queue full, dropping a request for %s", job.peerId.c_str());
    return false;
  }
  queue_.push_back(job);
  return true;
}

bool PeerClient::command(const String &peerId, const JsonDocument &body,
                         String &error) {
  const Peer *peer = peers.find(peerId);
  if (peer == nullptr) {
    error = F("no such paired device");
    return false;
  }
  if (!peer->enabled) {
    error = F("that device is switched off in the pairing list");
    return false;
  }
  const DeviceFamily *family = deviceTypes.familyForType(peer->typeId);
  if (family == nullptr) {
    error = String(F("unknown device type '")) + peer->typeId + "'";
    return false;
  }

  if (family->needsEntity && peer->entity.isEmpty()) {
    error = String(F("'")) + peer->name +
            F("' needs its entity id filling in before it can be driven");
    return false;
  }

  // Not everything is a web server. A Wake-on-LAN target has no endpoint to
  // POST to and nothing to say back; the command *is* the packet.
  if (family->transport == Transport::Wol) return wake(*peer, error);

  // An action may carry its own endpoint instead of using the family's. That
  // is how ESPHome buttons work: each is a separate entity pressed at its own
  // URL, with nothing to send alongside it.
  String path;
  JsonDocument rest;
  const char *ownPath = body["_path"];
  if (ownPath != nullptr) {
    path = ownPath;
    for (JsonPairConst pair : body.as<JsonObjectConst>()) {
      if (strcmp(pair.key().c_str(), "_path") != 0) rest[pair.key()] = pair.value();
    }
  } else {
    path = family->commandPath;
    rest.set(body);
  }
  if (path.isEmpty()) {
    error = String(F("no command endpoint is known for type '")) + peer->typeId + "'";
    return false;
  }
  path = withEntity(path, peer->entity);

  String payload;
  if (family->queryCommands) {
    const String query = toQuery(rest);
    if (!query.isEmpty()) path += (path.indexOf('?') >= 0 ? '&' : '?') + query;
  } else {
    serializeJson(rest, payload);
  }

  Job job;
  job.kind = Kind::Command;
  job.peerId = peer->id;
  job.host = peer->host;
  job.port = peer->port;
  job.request = buildRequest("POST", peer->host, peer->port, path,
                             peer->token, payload);

  LOG_I(kTag, "%s <- %s", peer->name.c_str(), path.c_str());
  return enqueue(job);
}

// ---------------------------------------------------------------------------

// Wake-on-LAN. Six 0xFF bytes followed by the target's MAC sixteen times, sent
// to the broadcast address: the network card is listening for exactly that
// while the rest of the machine is off.
//
// Sent three times. The packet is unacknowledged by design — there is nobody
// awake to acknowledge it — and one lost datagram would otherwise be a machine
// that silently did not start.
bool PeerClient::wake(const Peer &peer, String &error) {
  uint8_t mac[6];
  if (!parseMac(peer.entity, mac)) {
    error = String(F("'")) + peer.name +
            F("' needs a MAC address, like a1:b2:c3:d4:e5:f6");
    return false;
  }

  uint8_t packet[102];
  memset(packet, 0xFF, 6);
  for (uint8_t repeat = 0; repeat < 16; repeat++) {
    memcpy(packet + 6 + repeat * 6, mac, 6);
  }

  // The magic packet is addressed to the broadcast address, not to the
  // machine: an ethernet card that is asleep has no IP to be reached at.
  IPAddress target;
  if (peer.host.isEmpty() || !target.fromString(peer.host)) {
    target = IPAddress(255, 255, 255, 255);
  }
  const uint16_t port = peer.port != 0 ? peer.port : 9;

  WiFiUDP udp;
  bool sent = false;
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    if (!udp.beginPacket(target, port)) continue;
    udp.write(packet, sizeof(packet));
    if (udp.endPacket()) sent = true;
  }

  if (!sent) {
    error = F("the magic packet could not be sent");
    return false;
  }

  // Nothing answers, so "it worked" means "it left" — reported as such rather
  // than pretending to know whether the machine started.
  PeerState *state = stateFor(peer.id);
  state->lastSeenAt = millis();
  LOG_I(kTag, "%s <- wake-on-lan", peer.name.c_str());
  return true;
}

// ---------------------------------------------------------------------------

void PeerClient::loop() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!busy_ && !queue_.empty()) {
    dispatch();
    return;
  }

  if (busy_) return;
  if (millis() < nextPollAt_) return;
  nextPollAt_ = millis() + kPollTickMs;

  const std::vector<Peer> &list = peers.all();
  if (list.empty()) return;

  // Round-robin, one peer per tick.
  for (size_t attempt = 0; attempt < list.size(); attempt++) {
    if (pollCursor_ >= list.size()) pollCursor_ = 0;
    const Peer &peer = list[pollCursor_++];
    if (!peer.enabled) continue;

    const DeviceFamily *family = deviceTypes.familyForType(peer.typeId);
    if (family == nullptr || family->statePath.isEmpty()) continue;
    if (family->needsEntity && peer.entity.isEmpty()) continue;

    Job job;
    job.kind = Kind::Poll;
    job.peerId = peer.id;
    job.host = peer.host;
    job.port = peer.port;
    job.request = buildRequest("GET", peer.host, peer.port,
                               withEntity(family->statePath, peer.entity),
                               peer.token, String());
    enqueue(job);
    return;
  }
}

void PeerClient::dispatch() {
  inFlight_ = queue_.front();
  queue_.erase(queue_.begin());

  busy_ = true;
  response_ = "";
  headersDone_ = false;

  if (!client_.connect(inFlight_.host.c_str(), inFlight_.port)) {
    finish(false);
  }
}

void PeerClient::finish(bool connected) {
  if (!busy_) return;
  busy_ = false;

  PeerState *state = stateFor(inFlight_.peerId);

  const int status = response_.startsWith("HTTP/1.")
                         ? response_.substring(9, 12).toInt()
                         : 0;
  const bool ok = connected && status >= 200 && status < 300;

  if (!ok) {
    state->failures++;
    if (state->online && state->failures >= kFailuresBeforeOffline) {
      state->online = false;
      LOG_W(kTag, "%s is not answering", inFlight_.peerId.c_str());
    }
    response_ = "";
    return;
  }

  state->failures = 0;
  state->lastSeenAt = millis();
  if (!state->online) {
    state->online = true;
    LOG_I(kTag, "%s is online", inFlight_.peerId.c_str());
  }

  if (inFlight_.kind == Kind::Poll) absorb(response_);
  response_ = "";
}

// Keeps only the fields this device's family declares, so the cache stays a
// few dozen bytes however large the peer's status document is.
void PeerClient::absorb(const String &raw) {
  const int start = raw.indexOf("\r\n\r\n");
  if (start < 0) return;

  const Peer *peer = peers.find(inFlight_.peerId);
  if (peer == nullptr) return;
  const DeviceFamily *family = deviceTypes.familyForType(peer->typeId);
  if (family == nullptr) return;

  JsonDocument filter;
  for (const String &field : family->fields) filter[field] = true;
  // An SLWF-12 wraps its state; a WLED controller does not. Accepting both
  // shapes here is cheaper than teaching the type database about envelopes.
  for (const String &field : family->fields) filter["state"][field] = true;

  JsonDocument doc;
  if (deserializeJson(doc, raw.c_str() + start + 4,
                      DeserializationOption::Filter(filter)) !=
      DeserializationError::Ok) {
    return;
  }

  JsonDocument flat;
  JsonObjectConst nested = doc["state"].as<JsonObjectConst>();
  for (const String &field : family->fields) {
    JsonVariantConst value = doc[field].as<JsonVariantConst>();
    if (value.isNull() && !nested.isNull()) value = nested[field];
    if (!value.isNull()) flat[field] = value;
  }

  PeerState *state = stateFor(inFlight_.peerId);
  state->values = "";
  serializeJson(flat, state->values);
}

// ---------------------------------------------------------------------------

void PeerClient::statusJson(JsonArray out) const {
  for (const Peer &peer : peers.all()) {
    JsonObject entry = out.add<JsonObject>();
    entry["id"] = peer.id;
    entry["name"] = peer.name;
    entry["type"] = peer.typeId;
    entry["enabled"] = peer.enabled;

    const DeviceFamily *family = deviceTypes.familyForType(peer.typeId);
    // A Wake-on-LAN target answers nothing, so "offline" would be a lie told
    // once a second. Stateless things simply have no such indicator.
    const bool stateless = family == nullptr || family->statePath.isEmpty();
    if (stateless) entry["stateless"] = true;

    const PeerState *state = stateFor(peer.id);
    entry["online"] = stateless || (state != nullptr && state->online);
    if (state != nullptr && state->lastSeenAt != 0) {
      entry["secondsAgo"] = (millis() - state->lastSeenAt) / 1000;
    }
    if (state != nullptr && !state->values.isEmpty()) {
      JsonDocument cached;
      if (deserializeJson(cached, state->values) == DeserializationError::Ok) {
        entry["state"] = cached;
      }
    }
  }
}

}  // namespace app
