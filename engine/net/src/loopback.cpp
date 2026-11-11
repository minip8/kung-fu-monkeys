#include "kfm/net/loopback.hpp"

#include "kfm/core/assert.hpp"

#include <algorithm>
#include <utility>

namespace kfm::net {
namespace {

constexpr uint32_t kMaxAttempts = 32;  // a reliable message is retransmitted at most this often
constexpr uint64_t kMaxJitterNs = 0xFFFF'FFFEull;

[[nodiscard]] bool chance(Rng& rng, uint32_t ppm) {
  // Always draws, so the stream does not depend on the probability values.
  return rng.below(kPpmOne) < ppm;
}

// Uniform in [0, maxNs], maxNs clamped to ~4.29 s. Always draws.
[[nodiscard]] uint64_t uniformNs(Rng& rng, uint64_t maxNs) {
  const uint64_t span = std::min(maxNs, kMaxJitterNs) + 1;
  return (static_cast<uint64_t>(rng.next()) * span) >> 32;
}

[[nodiscard]] Rng directionRng(uint64_t seed, PeerId client, bool toServer) {
  const uint64_t salt = (static_cast<uint64_t>(client.value) << 1) | (toServer ? 1u : 0u);
  return Rng::seeded((seed * 0x9E37'79B9'7F4A'7C15ull) ^ (salt * 0xBF58'476D'1CE4'E5B9ull));
}

}  // namespace

// --- LoopbackTransport -------------------------------------------------------

LoopbackTransport::LoopbackTransport(Key /*key*/, LoopbackNetwork& net, bool isServer, PeerId self,
                                     uint64_t nowNs)
    : net_(&net), isServer_(isServer), self_(self), nowNs_(nowNs) {}

LoopbackTransport::Peer* LoopbackTransport::findPeer(PeerId peer) {
  if (!peer.isValid() || peer.value >= peers_.size()) return nullptr;
  Peer& p = peers_[peer.value];
  return p.state == PeerState::None ? nullptr : &p;
}

const LoopbackTransport::Peer* LoopbackTransport::findPeer(PeerId peer) const {
  if (!peer.isValid() || peer.value >= peers_.size()) return nullptr;
  const Peer& p = peers_[peer.value];
  return p.state == PeerState::None ? nullptr : &p;
}

void LoopbackTransport::addPeer(PeerId peer, uint64_t connectAtNs) {
  if (peers_.size() <= peer.value) peers_.resize(static_cast<size_t>(peer.value) + 1);
  Peer& p = peers_[peer.value];
  p.state = PeerState::Pending;
  p.connectAtNs = connectAtNs;
  p.lastReliableDeliverNs = 0;
  pendingEvents_.push_back(ConnectionEvent{ConnectionEventKind::Connected, peer, connectAtNs});
}

void LoopbackTransport::markClosed(PeerId peer, uint64_t atNs) {
  Peer* p = findPeer(peer);
  if (p == nullptr || p->state == PeerState::Closed) return;
  const bool wasConnected = p->state == PeerState::Connected;
  p->state = PeerState::Closed;
  if (isServer_ && net_->openLinks_ > 0) --net_->openLinks_;
  // A link that never connected closes silently; its pending Connected
  // event is skipped because the state is no longer Pending.
  if (wasConnected) {
    pendingEvents_.push_back(ConnectionEvent{ConnectionEventKind::Disconnected, peer, atNs});
  }
}

void LoopbackTransport::send(PeerId peer, Lane lane, std::span<const std::byte> data) {
  const size_t limit = lane == Lane::Unreliable ? kMaxUnreliableBytes : kMaxReliableBytes;
  const Peer* p = findPeer(peer);
  if (data.size() > limit || p == nullptr || p->state != PeerState::Connected) {
    ++stats_.sendErrors;
    return;
  }
  ++stats_.packetsSent;
  stats_.bytesSent += data.size();
  net_->route(*this, peer, lane, false, data);
}

void LoopbackTransport::sendUnreliable(PeerId peer, std::span<const std::byte> data) {
  send(peer, Lane::Unreliable, data);
}

void LoopbackTransport::sendReliable(PeerId peer, std::span<const std::byte> data) {
  send(peer, Lane::Reliable, data);
}

bool LoopbackTransport::recv(Packet& out) {
  if (queue_.empty()) return false;
  current_ = std::move(queue_.front());
  queue_.pop_front();
  out.peer = current_.from;
  out.lane = current_.lane;
  out.receivedNs = current_.receivedNs;
  out.data = current_.data;
  return true;
}

void LoopbackTransport::poll(uint64_t nowNs, ConnectionEvents& events) {
  const uint64_t prevNs = nowNs_;
  nowNs_ = std::max(nowNs_, nowNs);

  emitDueEvents(events);

  // Messages due by now, in (delivery time, send order) order.
  std::ranges::sort(inbound_, [](const InFlight& a, const InFlight& b) {
    return a.deliverAtNs != b.deliverAtNs ? a.deliverAtNs < b.deliverAtNs : a.order < b.order;
  });
  size_t arrived = 0;
  while (arrived < inbound_.size() && inbound_[arrived].deliverAtNs <= nowNs_) {
    InFlight& m = inbound_[arrived++];
    const Peer* p = findPeer(m.from);
    if (p == nullptr) continue;
    if (m.close) {
      if (p->state == PeerState::Connected || p->state == PeerState::Pending) {
        markClosed(m.from, nowNs_);
      }
      continue;
    }
    if (p->state != PeerState::Connected) continue;
    if (queue_.size() >= kRecvQueueCapacity) {
      ++stats_.recvDropped;
      continue;
    }
    ++stats_.packetsReceived;
    stats_.bytesReceived += m.data.size();
    queue_.push_back(Received{m.from, m.lane, std::max(m.deliverAtNs, prevNs), std::move(m.data)});
  }
  inbound_.erase(inbound_.begin(), inbound_.begin() + static_cast<ptrdiff_t>(arrived));

  // A close that arrived above is seen in this same poll.
  emitDueEvents(events);
}

void LoopbackTransport::emitDueEvents(ConnectionEvents& events) {
  std::ranges::stable_sort(pendingEvents_, {}, &ConnectionEvent::timeNs);
  size_t due = 0;
  while (due < pendingEvents_.size() && pendingEvents_[due].timeNs <= nowNs_) {
    const ConnectionEvent& e = pendingEvents_[due++];
    if (e.kind == ConnectionEventKind::Connected) {
      Peer* p = findPeer(e.peer);
      if (p == nullptr || p->state != PeerState::Pending) continue;
      p->state = PeerState::Connected;
    }
    events.push(e);
  }
  pendingEvents_.erase(pendingEvents_.begin(),
                       pendingEvents_.begin() + static_cast<ptrdiff_t>(due));
}

void LoopbackTransport::disconnect(PeerId peer) {
  const Peer* p = findPeer(peer);
  if (p == nullptr || p->state == PeerState::Closed) return;
  // The close travels behind every reliable message already sent.
  net_->route(*this, peer, Lane::Reliable, true, {});
  markClosed(peer, nowNs_);
}

bool LoopbackTransport::isConnected(PeerId peer) const {
  const Peer* p = findPeer(peer);
  return p != nullptr && p->state == PeerState::Connected;
}

TransportStats LoopbackTransport::stats() const {
  return stats_;
}

// --- LoopbackNetwork ---------------------------------------------------------

LoopbackNetwork::LoopbackNetwork(uint64_t seed, uint64_t nowNs)
    : seed_(seed),
      server_(std::make_unique<LoopbackTransport>(LoopbackTransport::Key{}, *this, true,
                                                  kServerPeer, nowNs)) {}

LoopbackTransport& LoopbackNetwork::connectClient(uint64_t nowNs, const LinkConditions& toServer,
                                                  const LinkConditions& toClient) {
  KFM_CHECK(links_.size() + 1 < PeerId::kInvalidValue);
  const PeerId id{static_cast<PeerId::rep>(links_.size() + 1)};
  Link link;
  link.client =
      std::make_unique<LoopbackTransport>(LoopbackTransport::Key{}, *this, false, id, nowNs);
  link.toServer = Direction{toServer, {}, directionRng(seed_, id, true), {}};
  link.toClient = Direction{toClient, {}, directionRng(seed_, id, false), {}};
  LoopbackTransport& client = *link.client;
  links_.push_back(std::move(link));

  if (openLinks_ >= kMaxPeers) {
    // Server full: the client never connects and sees its link close.
    client.peers_.resize(1);
    client.peers_[0].state = LoopbackTransport::PeerState::Closed;
    client.pendingEvents_.push_back(
        ConnectionEvent{ConnectionEventKind::Disconnected, kServerPeer, nowNs});
    return client;
  }
  ++openLinks_;
  server_->addPeer(id, nowNs + toServer.reliable.latencyNs);
  client.addPeer(kServerPeer, nowNs + toClient.reliable.latencyNs);
  return client;
}

LoopbackNetwork::Link* LoopbackNetwork::findLink(PeerId client) {
  if (!client.isValid() || client.value == 0 || client.value > links_.size()) return nullptr;
  return &links_[client.value - 1u];
}

const LoopbackNetwork::Link* LoopbackNetwork::findLink(PeerId client) const {
  if (!client.isValid() || client.value == 0 || client.value > links_.size()) return nullptr;
  return &links_[client.value - 1u];
}

LoopbackTransport* LoopbackNetwork::client(PeerId id) {
  const Link* l = findLink(id);
  return l == nullptr ? nullptr : l->client.get();
}

void LoopbackNetwork::setConditions(PeerId client, const LinkConditions& toServer,
                                    const LinkConditions& toClient) {
  Link* l = findLink(client);
  if (l == nullptr) return;
  l->toServer.conditions = toServer;
  l->toClient.conditions = toClient;
}

void LoopbackNetwork::cut(PeerId client) {
  Link* l = findLink(client);
  if (l == nullptr) return;
  l->client->markClosed(kServerPeer, l->client->nowNs_);
  server_->markClosed(client, server_->nowNs_);
}

void LoopbackNetwork::setOutage(PeerId client, bool toServer, const Outage& outage) {
  Link* l = findLink(client);
  if (l == nullptr) return;
  (toServer ? l->toServer : l->toClient).outage = outage;
}

EmulatorStats LoopbackNetwork::emulatorStats(PeerId client, bool toServer) const {
  const Link* l = findLink(client);
  if (l == nullptr) return {};
  return toServer ? l->toServer.stats : l->toClient.stats;
}

void LoopbackNetwork::route(LoopbackTransport& from, PeerId to, Lane lane, bool close,
                            std::span<const std::byte> data) {
  const PeerId clientId = from.isServer_ ? to : from.self_;
  Link* link = findLink(clientId);
  if (link == nullptr) return;
  Direction& dir = from.isServer_ ? link->toClient : link->toServer;
  LoopbackTransport& receiver = from.isServer_ ? *link->client : *server_;
  const PeerId seenAs = from.isServer_ ? kServerPeer : from.self_;
  const uint64_t sendNs = from.nowNs_;
  const bool inOutage = sendNs >= dir.outage.fromNs && sendNs < dir.outage.untilNs;
  const bool blackedOut = inOutage && dir.outage.unreliable;

  const auto enqueue = [&](uint64_t deliverAtNs) {
    LoopbackTransport::InFlight m;
    m.deliverAtNs = deliverAtNs;
    m.order = order_++;
    m.from = seenAs;
    m.lane = lane;
    m.close = close;
    m.data.assign(data.begin(), data.end());
    receiver.inbound_.push_back(std::move(m));
  };

  if (lane == Lane::Unreliable) {
    const LaneConditions& c = dir.conditions.unreliable;
    ++dir.stats.unreliableSent;
    const bool lost = chance(dir.rng, c.lossPpm);
    const bool duplicated = chance(dir.rng, c.duplicatePpm);
    if (lost) {
      ++dir.stats.unreliableLost;
      return;
    }
    if (blackedOut) {
      ++dir.stats.unreliableLost;
      ++dir.stats.unreliableBlackedOut;
    } else if (duplicated) {
      ++dir.stats.unreliableDuplicated;
    }
    for (int copy = 0; copy < (duplicated ? 2 : 1); ++copy) {
      // Drawn for a blacked-out message too: the outage shifts no later draw.
      uint64_t delay = c.latencyNs + uniformNs(dir.rng, c.jitterNs);
      const bool heldBack = chance(dir.rng, c.reorderPpm);
      if (blackedOut) continue;
      if (heldBack) {
        ++dir.stats.unreliableHeldBack;
        delay += c.reorderDelayNs;
      }
      enqueue(sendNs + delay);
    }
    return;
  }

  const LaneConditions& c = dir.conditions.reliable;
  ++dir.stats.reliableSent;
  uint64_t delay = c.latencyNs + uniformNs(dir.rng, c.jitterNs);
  for (uint32_t attempt = 1; attempt < kMaxAttempts && chance(dir.rng, c.lossPpm); ++attempt) {
    ++dir.stats.reliableRetransmits;
    delay += c.retransmitNs;
  }
  LoopbackTransport::Peer* senderPeer = from.findPeer(to);
  KFM_ASSERT(senderPeer != nullptr);
  uint64_t deliverAtNs = sendNs + delay;
  if (inOutage && dir.outage.reliable) {
    const uint64_t after = dir.outage.untilNs + c.latencyNs;
    if (after > deliverAtNs) {
      deliverAtNs = after;
      ++dir.stats.reliableHeld;
    }
  }
  if (senderPeer != nullptr) {
    deliverAtNs = std::max(deliverAtNs, senderPeer->lastReliableDeliverNs);
    senderPeer->lastReliableDeliverNs = deliverAtNs;
  }
  enqueue(deliverAtNs);
}

}  // namespace kfm::net
