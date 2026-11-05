#include "kfm/net/net_server.hpp"

#include "kfm/sim/step.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>

namespace kfm::net {
namespace {

constexpr uint64_t kNsPerSecond = 1'000'000'000ull;
// Button bits a PlayerInput may carry (§5.4: Throw .. Taunt).
constexpr uint8_t kValidButtons = 0x1F;

[[nodiscard]] constexpr sim::PlayerMask bit(uint16_t slot) {
  return static_cast<sim::PlayerMask>(1u << slot);
}

[[nodiscard]] constexpr bool has(sim::PlayerMask m, uint16_t slot) {
  return ((static_cast<uint32_t>(m) >> slot) & 1u) != 0;
}

[[nodiscard]] int popcount(sim::PlayerMask m) {
  return std::popcount(static_cast<uint32_t>(m));
}

// The lowest n set bits of m.
[[nodiscard]] sim::PlayerMask lowestBits(sim::PlayerMask m, int n) {
  sim::PlayerMask out = 0;
  for (uint16_t s = 0; s < sim::kMaxPlayers && n > 0; ++s) {
    if (!has(m, s)) continue;
    out = static_cast<sim::PlayerMask>(out | bit(s));
    --n;
  }
  return out;
}

// The highest n set bits of m.
[[nodiscard]] sim::PlayerMask highestBits(sim::PlayerMask m, int n) {
  sim::PlayerMask out = 0;
  for (int s = sim::kMaxPlayers - 1; s >= 0 && n > 0; --s) {
    if (!has(m, static_cast<uint16_t>(s))) continue;
    out = static_cast<sim::PlayerMask>(out | bit(static_cast<uint16_t>(s)));
    --n;
  }
  return out;
}

[[nodiscard]] int8_t saturate8(int64_t v) {
  return static_cast<int8_t>(std::clamp<int64_t>(v, std::numeric_limits<int8_t>::min(),
                                                 std::numeric_limits<int8_t>::max()));
}

[[nodiscard]] bool sameToken(std::span<const std::byte> a, std::span<const std::byte> b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

}  // namespace

NetServer::NetServer(ITransport& transport, const sim::ContentDB& content, ServerConfig config)
    : transport_(&transport),
      content_(&content),
      config_(std::move(config)),
      contentHash_(sim::contentHash(content)),
      world_(sim::initialWorld(config_.match)),
      assembler_(world_.tick),
      present_(static_cast<sim::PlayerMask>(config_.match.players & kAllPlayersMask)),
      botRng_(Rng::seeded(config_.botSeed)),
      botSeed_(config_.botSeed),
      peers_(kMaxPeers),
      buf_(kMaxReliableBytes) {
  assembler_.setPresent(present_);
}

// --- roster -----------------------------------------------------------------------

bool NetServer::setBot(PlayerId slot, bool bot) {
  if (!slot.isValid() || slot.value >= sim::kMaxPlayers) return false;
  if (bot && slot.value >= config_.maxPlayers) return false;
  const sim::PlayerMask b = bit(slot.value);
  if (bot) {
    if ((bots_ & b) != 0) return true;
    if ((present_ & b) != 0) return false;  // owned, reserved or in the initial roster
    bots_ = static_cast<sim::PlayerMask>(bots_ | b);
    present_ = static_cast<sim::PlayerMask>(present_ | b);
    assembler_.clearSlot(slot);
    return true;
  }
  if ((bots_ & b) == 0) return false;
  bots_ = static_cast<sim::PlayerMask>(bots_ & ~b);
  releaseSlots(b, PeerId::invalid());
  return true;
}

sim::PlayerMask NetServer::freeSlots() const {
  const uint32_t open = (1u << std::min<uint32_t>(config_.maxPlayers, sim::kMaxPlayers)) - 1u;
  return static_cast<sim::PlayerMask>(open & kAllPlayersMask & ~uint32_t{present_});
}

sim::PlayerMask NetServer::reservedSlots() const {
  sim::PlayerMask m = 0;
  for (const Reservation& r : reservations_)
    if (r.used) m = static_cast<sim::PlayerMask>(m | r.slots);
  return m;
}

PeerId NetServer::ownerOf(PlayerId slot) const {
  if (!slot.isValid() || slot.value >= sim::kMaxPlayers) return PeerId::invalid();
  for (const Peer& p : peers_)
    if (p.used && has(p.info.slots, slot.value)) return p.info.peer;
  return PeerId::invalid();
}

void NetServer::assignSlots(Peer& p, sim::PlayerMask slots) {
  p.info.slots = static_cast<sim::PlayerMask>(p.info.slots | slots);
  present_ = static_cast<sim::PlayerMask>(present_ | slots);
  for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
    if (!has(slots, s)) continue;
    const PlayerId id{static_cast<PlayerId::rep>(s)};
    nicknames_[s] = p.nickname;
    assembler_.clearSlot(id);  // the previous owner's inputs must not repeat
    for (Peer& q : peers_) {
      if (!q.used || q.info.state != PeerState::Active) continue;
      sendReliableMsg(q, PlayerJoinedMsg{id, nextTick(), &q == &p});
    }
  }
}

void NetServer::releaseSlots(sim::PlayerMask slots, PeerId peer) {
  slots = static_cast<sim::PlayerMask>(slots & present_);
  if (slots == 0) return;
  present_ = static_cast<sim::PlayerMask>(present_ & ~slots);
  for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
    if (!has(slots, s)) continue;
    const PlayerId id{static_cast<PlayerId::rep>(s)};
    assembler_.clearSlot(id);
    nicknames_[s] = Nickname{};
    for (Peer& q : peers_) {
      if (!q.used || q.info.state != PeerState::Active) continue;
      sendReliableMsg(q, PlayerLeftMsg{id, nextTick()});
    }
  }
  emit(ServerEventKind::SlotsReleased, peer, slots);
}

void NetServer::releaseReservations() {
  for (Reservation& r : reservations_) {
    if (!r.used) continue;
    r.used = false;
    releaseSlots(r.slots, PeerId::invalid());
  }
}

const Nickname& NetServer::nickname(PlayerId slot) const {
  static constexpr Nickname kNone{};
  if (!slot.isValid() || slot.value >= sim::kMaxPlayers) return kNone;
  return nicknames_[slot.value];
}

void NetServer::setRoomState(const RoomStateMsg& state) {
  roomState_ = state;
  for (Peer& q : peers_) {
    if (q.used && q.info.state == PeerState::Active) sendReliableMsg(q, state);
  }
}

void NetServer::expireReservations() {
  for (Reservation& r : reservations_) {
    if (!r.used || nextTick() < r.expires) continue;
    r.used = false;
    releaseSlots(r.slots, PeerId::invalid());
  }
}

// --- peers ------------------------------------------------------------------------

NetServer::Peer* NetServer::findPeer(PeerId id) {
  for (Peer& p : peers_)
    if (p.used && p.info.peer == id) return &p;
  return nullptr;
}

const NetServer::Peer* NetServer::findPeer(PeerId id) const {
  for (const Peer& p : peers_)
    if (p.used && p.info.peer == id) return &p;
  return nullptr;
}

NetServer::Peer* NetServer::addPeer(PeerId id, uint64_t nowNs) {
  for (Peer& p : peers_) {
    if (p.used) continue;
    p = Peer{};
    p.used = true;
    p.info.peer = id;
    p.lastRecvNs = nowNs;
    p.bucketNs = uint64_t{config_.inputMsgsPerSecond} * kNsPerSecond;
    p.bucketAtNs = nowNs;
    p.errorWindowStartNs = nowNs;
    p.roomBucketNs = uint64_t{config_.roomMsgsPerSecond} * kNsPerSecond;
    p.roomBucketAtNs = nowNs;
    return &p;
  }
  return nullptr;
}

void NetServer::removePeer(Peer& p, bool reserve) {
  const sim::PlayerMask slots = p.info.slots;
  p.info.slots = 0;
  p.used = false;
  if (slots == 0) return;
  if (reserve && p.tokenLength > 0) {
    for (Reservation& r : reservations_) {
      if (r.used) continue;
      r.used = true;
      r.slots = slots;
      r.expires = nextTick().plus(static_cast<int32_t>(config_.reconnectGraceTicks));
      r.token = p.token;
      r.tokenLength = p.tokenLength;
      return;
    }
  }
  releaseSlots(slots, p.info.peer);
}

std::optional<ServerPeerInfo> NetServer::peerInfo(PeerId peer) const {
  if (const Peer* p = findPeer(peer)) return p->info;
  return std::nullopt;
}

std::vector<ServerPeerInfo> NetServer::peers() const {
  std::vector<ServerPeerInfo> out;
  for (const Peer& p : peers_)
    if (p.used) out.push_back(p.info);
  return out;
}

void NetServer::kick(PeerId peer, DisconnectReason reason) {
  Peer* p = findPeer(peer);
  if (p == nullptr) return;
  sendReliableMsg(*p, DisconnectMsg{reason});
  transport_->disconnect(peer);
  ++stats_.kicks;
  emit(ServerEventKind::PeerKicked, peer, p->info.slots, static_cast<uint8_t>(reason));
  removePeer(*p, false);
}

void NetServer::restartMatch(uint64_t seed, Tick startTick, uint64_t botSeed) {
  sim::MatchConfig config = config_.match;
  config.seed = seed;
  restartMatch(config, startTick, botSeed);
}

void NetServer::restartMatch(const sim::MatchConfig& config, Tick startTick, uint64_t botSeed) {
  const Tick start = std::max(startTick, nextTick());
  restart_ = PendingRestart{config, botSeed, start};
  const MatchConfigMsg m{announced(config), start};
  for (Peer& q : peers_) {
    if (q.used && q.info.state == PeerState::Active) sendReliableMsg(q, m);
  }
}

uint8_t NetServer::humanLeadTicks() const {
  std::array<int32_t, kMaxPeers> leads{};
  size_t n = 0;
  for (const Peer& p : peers_) {
    if (!p.used || p.info.state != PeerState::Active || p.info.slots == 0 ||
        p.info.leadSamples == 0 || n == leads.size()) {
      continue;
    }
    leads[n++] = p.info.leadEmaQ8;
  }
  if (n == 0) return 0;
  // The lower median: one slow link does not slow every bot down.
  const auto mid = static_cast<std::ptrdiff_t>((n - 1) / 2);
  std::nth_element(leads.begin(), leads.begin() + mid,
                   leads.begin() + static_cast<std::ptrdiff_t>(n));
  const int32_t rounded = (leads[static_cast<size_t>(mid)] + 128) / 256;  // Q8 → ticks, rounded
  return static_cast<uint8_t>(std::clamp<int32_t>(rounded, 0, kMaxHumanLeadTicks));
}

std::optional<Tick> NetServer::pendingRestart() const {
  if (!restart_) return std::nullopt;
  return restart_->startTick;
}

void NetServer::applyRestart(Tick t, const PendingRestart& r) {
  config_.match = announced(r.config);
  world_ = sim::initialWorld(config_.match);
  world_.tick = t;  // ticks are monotonic across matches; the sim only uses differences
  botRng_ = Rng::seeded(r.botSeed);
  botSeed_ = r.botSeed;
  restart_.reset();
  ++matchIndex_;
  ++stats_.matchRestarts;
  for (Peer& q : peers_) {
    if (q.used && q.info.state == PeerState::Active) sendSnapshot(q);
  }
  emit(ServerEventKind::MatchRestarted, PeerId::invalid(), present_);
}

void NetServer::shutdown() {
  if (shuttingDown_) return;
  shuttingDown_ = true;
  for (Peer& q : peers_) {
    if (q.used) sendReliableMsg(q, DisconnectMsg{DisconnectReason::ServerShuttingDown});
  }
}

void NetServer::closeAll() {
  for (Peer& q : peers_) {
    if (!q.used) continue;
    transport_->disconnect(q.info.peer);
    emit(ServerEventKind::PeerLeft, q.info.peer, q.info.slots,
         static_cast<uint8_t>(DisconnectReason::ServerShuttingDown));
    removePeer(q, false);
  }
}

void NetServer::reject(Peer& p, RejectReason reason) {
  sendReliableMsg(p, RejectMsg{reason});
  transport_->disconnect(p.info.peer);
  ++stats_.rejects;
  emit(ServerEventKind::PeerRejected, p.info.peer, 0, static_cast<uint8_t>(reason));
  removePeer(p, false);
}

void NetServer::protocolError(Peer& p, uint64_t nowNs) {
  ++stats_.protocolErrors;
  ++p.info.protocolErrors;
  if (nowNs - p.errorWindowStartNs > config_.protocolErrorWindowNs) {
    p.errorWindowStartNs = nowNs;
    p.errorsInWindow = 0;
  }
  if (++p.errorsInWindow > config_.maxProtocolErrors) {
    kick(p.info.peer, DisconnectReason::ProtocolError);
  }
}

void NetServer::emit(ServerEventKind kind, PeerId peer, sim::PlayerMask slots, uint8_t reason,
                     const RoomSettings& settings) {
  events_.push_back(ServerEvent{kind, peer, slots, nextTick(), reason, settings});
}

// --- sending ----------------------------------------------------------------------

void NetServer::sendReliable(Peer& p, std::span<const std::byte> bytes) {
  transport_->sendReliable(p.info.peer, bytes);
  stats_.bytesSent += bytes.size();
  p.info.bytesOut += bytes.size();
}

void NetServer::sendUnreliable(Peer& p, std::span<const std::byte> bytes) {
  transport_->sendUnreliable(p.info.peer, bytes);
  stats_.bytesSent += bytes.size();
  p.info.bytesOut += bytes.size();
}

template <class M>
void NetServer::sendReliableMsg(Peer& p, const M& m) {
  const size_t n = encode(m, buf_);
  if (n > 0) sendReliable(p, {buf_.data(), n});
}

void NetServer::sendSnapshot(Peer& p) {
  snapshot_.config = announced(config_.match);
  snapshot_.world = world_;
  const size_t n = encode(snapshot_, buf_);
  if (n == 0) return;
  sendReliable(p, {buf_.data(), n});
  ++stats_.snapshotsSent;
}

void NetServer::welcome(Peer& p) {
  sendReliableMsg(p, WelcomeMsg{p.info.slots, announced(config_.match), nextTick()});
  sendSnapshot(p);
  if (restart_) {
    sendReliableMsg(p, MatchConfigMsg{announced(restart_->config), restart_->startTick});
  }
  if (roomState_) sendReliableMsg(p, *roomState_);
}

// --- receiving --------------------------------------------------------------------

void NetServer::pump(uint64_t nowNs) {
  nowNs_ = nowNs;
  connEvents_.clear();
  transport_->poll(nowNs, connEvents_);
  for (const ConnectionEvent& ev : connEvents_.span()) {
    if (ev.kind == ConnectionEventKind::Connected) {
      if (findPeer(ev.peer) == nullptr && addPeer(ev.peer, nowNs) == nullptr) {
        transport_->disconnect(ev.peer);  // peer table full
      }
    } else if (Peer* p = findPeer(ev.peer)) {
      emit(ServerEventKind::PeerLeft, ev.peer, p->info.slots);
      removePeer(*p, true);
    }
  }

  Packet packet;
  while (transport_->recv(packet)) {
    stats_.bytesReceived += packet.data.size();
    Peer* p = findPeer(packet.peer);
    if (p == nullptr) continue;
    p->info.bytesIn += packet.data.size();
    p->lastRecvNs = std::max({p->lastRecvNs, packet.receivedNs, nowNs});
    onPacket(*p, packet);
  }

  for (Peer& p : peers_) {
    if (!p.used || nowNs - p.lastRecvNs <= config_.peerTimeoutNs) continue;
    transport_->disconnect(p.info.peer);
    emit(ServerEventKind::PeerLeft, p.info.peer, p.info.slots,
         static_cast<uint8_t>(DisconnectReason::Timeout));
    removePeer(p, true);
  }
}

void NetServer::onPacket(Peer& p, const Packet& packet) {
  const Decoded<MessageType> type = peekType(packet.data);
  if (!type) {
    protocolError(p, nowNs_);
    return;
  }
  switch (*type) {
    case MessageType::Hello:
      if (p.info.state != PeerState::AwaitingHello) {
        protocolError(p, nowNs_);
      } else {
        onHello(p, packet);
      }
      return;
    case MessageType::Ping: {
      PingMsg ping;
      if (!decode(packet.data, ping)) {
        protocolError(p, nowNs_);
        return;
      }
      const uint64_t hold = nowNs_ > packet.receivedNs ? nowNs_ - packet.receivedNs : 0;
      const PongMsg pong{ping.sendNs, nextTick(),
                         static_cast<uint32_t>(std::min<uint64_t>(hold, UINT32_MAX))};
      std::array<std::byte, kPongMsgBytes> out{};
      const size_t n = encode(pong, out);
      if (n > 0) sendUnreliable(p, {out.data(), n});
      return;
    }
    case MessageType::Pong:
      return;  // the server does not ping
    case MessageType::Disconnect: {
      DisconnectMsg d;
      if (!decode(packet.data, d)) {
        protocolError(p, nowNs_);
        return;
      }
      transport_->disconnect(p.info.peer);
      emit(ServerEventKind::PeerLeft, p.info.peer, p.info.slots, static_cast<uint8_t>(d.reason));
      removePeer(p, false);  // a deliberate quit gives the slots up at once
      return;
    }
    default:
      break;
  }
  if (p.info.state != PeerState::Active) {
    protocolError(p, nowNs_);
    return;
  }
  switch (*type) {
    case MessageType::Input:
      onInput(p, packet);
      return;
    case MessageType::JoinMatch:
      onJoinMatch(p, packet);
      return;
    case MessageType::ResyncRequest:
      onResync(p, packet);
      return;
    case MessageType::RoomSettings:
    case MessageType::Start:
      onRoomMessage(p, packet, *type);
      return;
    default:
      protocolError(p, nowNs_);  // a server-to-client message
      return;
  }
}

void NetServer::onHello(Peer& p, const Packet& packet) {
  const Decoded<uint16_t> version = peekHelloVersion(packet.data);
  if (!version || *version != kProtocolVersion) {
    reject(p, RejectReason::ProtocolMismatch);
    return;
  }
  HelloMsg h;
  if (!decode(packet.data, h)) {
    reject(p, RejectReason::ProtocolMismatch);
    return;
  }
  if (h.contentHash != contentHash_) {
    reject(p, RejectReason::ContentMismatch);
    return;
  }
  if (h.buildId != config_.buildId) {
    reject(p, RejectReason::BuildMismatch);
    return;
  }
  if (shuttingDown_) {
    reject(p, RejectReason::ServerShuttingDown);
    return;
  }
  if (config_.acceptToken && !config_.acceptToken(h.tokenBytes())) {
    reject(p, RejectReason::BadToken);
    return;
  }
  p.token = h.token;
  p.tokenLength = h.tokenLength;
  p.nickname = h.nickname;
  const std::span<const std::byte> token = h.tokenBytes();

  // Reconnect: a reservation or a still-connected peer with the same token.
  sim::PlayerMask reclaimed = 0;
  bool reconnect = false;
  if (!token.empty()) {
    for (Reservation& r : reservations_) {
      if (!r.used || !sameToken(token, {r.token.data(), r.tokenLength})) continue;
      reclaimed = r.slots;
      r.used = false;
      reconnect = true;
      break;
    }
    if (!reconnect) {
      for (Peer& q : peers_) {
        if (!q.used || &q == &p || q.info.state != PeerState::Active ||
            !sameToken(token, {q.token.data(), q.tokenLength})) {
          continue;
        }
        reclaimed = q.info.slots;
        q.info.slots = 0;  // handed over, not released
        transport_->disconnect(q.info.peer);
        emit(ServerEventKind::PeerLeft, q.info.peer, reclaimed);
        removePeer(q, false);
        reconnect = true;
        break;
      }
    }
  }

  p.info.state = PeerState::Active;
  if (reconnect) {
    p.info.slots = reclaimed;
    assembler_.setPresent(present_);
    welcome(p);
    ++stats_.reconnects;
    emit(ServerEventKind::PeerReconnected, p.info.peer, reclaimed);
    return;
  }
  const sim::PlayerMask free = freeSlots();
  if (popcount(free) < h.requestedSlots) {
    p.info.state = PeerState::AwaitingHello;
    reject(p, RejectReason::MatchFull);
    return;
  }
  const sim::PlayerMask slots = lowestBits(free, h.requestedSlots);
  // Welcome and the snapshot first, so the joiner holds the world before
  // its own PlayerJoined arrives.
  p.info.slots = slots;
  for (uint16_t s = 0; s < sim::kMaxPlayers; ++s)
    if (has(slots, s)) nicknames_[s] = p.nickname;
  present_ = static_cast<sim::PlayerMask>(present_ | slots);
  welcome(p);
  p.info.slots = 0;
  assignSlots(p, slots);
  ++stats_.joins;
  emit(ServerEventKind::PeerJoined, p.info.peer, slots);
}

void NetServer::onInput(Peer& p, const Packet& packet) {
  const uint64_t now = std::max(packet.receivedNs, p.bucketAtNs);
  const uint64_t cap = uint64_t{config_.inputMsgsPerSecond} * kNsPerSecond;
  p.bucketNs = std::min(cap, p.bucketNs + (now - p.bucketAtNs) * config_.inputMsgsPerSecond);
  p.bucketAtNs = now;
  if (p.bucketNs < kNsPerSecond) {
    ++stats_.rateLimited;
    ++p.info.rateLimited;
    return;
  }
  p.bucketNs -= kNsPerSecond;

  InputMsg m;
  if (!decode(packet.data, m)) {
    protocolError(p, nowNs_);
    return;
  }
  ++p.info.inputMsgs;
  bool bad = false;
  if ((m.slots & ~p.info.slots) != 0) {
    bad = true;  // someone else's slot: those inputs are ignored
    ++stats_.rejectedInputs;
    ++p.info.rejectedInputs;
  }
  const sim::PlayerMask own = static_cast<sim::PlayerMask>(m.slots & p.info.slots);
  for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
    if (!has(own, s)) continue;
    const PlayerId id{static_cast<PlayerId::rep>(s)};
    for (uint8_t i = 0; i < m.count; ++i) {
      const sim::PlayerInput& in = m.inputs[s][i];
      if ((in.buttons & ~kValidButtons) != 0) {
        bad = true;
        ++stats_.rejectedInputs;
        ++p.info.rejectedInputs;
        continue;
      }
      switch (assembler_.put(id, m.startTick.plus(i), in)) {
        case InputFrameAssembler::PutResult::Accepted:
          ++stats_.inputsAccepted;
          break;
        case InputFrameAssembler::PutResult::Late:
          ++stats_.inputsLate;
          break;
        case InputFrameAssembler::PutResult::TooEarly:
        case InputFrameAssembler::PutResult::BadSlot:
          bad = true;
          ++stats_.rejectedInputs;
          ++p.info.rejectedInputs;
          break;
      }
    }
  }

  if (const std::optional<Tick> held = frames_.newest(); m.lastAuthTick && held) {
    const Tick ack = std::min(*m.lastAuthTick, *held);
    if (!p.info.lastAuthAck || ack > *p.info.lastAuthAck) p.info.lastAuthAck = ack;
  }
  const Tick newest = m.endTick().plus(-1);
  if (own != 0 && (!p.info.lastInputTick || newest > *p.info.lastInputTick)) {
    p.info.lastInputTick = newest;
    const int8_t margin = saturate8(newest.minus(nextTick()));
    p.info.lastMargin = margin;
    p.marginSinceSend = p.marginSinceSend ? std::min(*p.marginSinceSend, margin) : margin;
    const int32_t q8 = int32_t{margin} * 256;
    p.info.marginEmaQ8 =
        p.info.inputMsgs <= 1 ? q8 : p.info.marginEmaQ8 + (q8 - p.info.marginEmaQ8) / 8;
    // Lead = margin + rtt/2, the round trip from the frame it acknowledges
    // (sent when nextTick() was ack + 1); see the header comment.
    if (m.lastAuthTick) {
      const int32_t rtt = std::max<int32_t>(nextTick().minus(*m.lastAuthTick) - 1, 0);
      const int32_t leadQ8 = q8 + rtt * 128;
      p.info.leadEmaQ8 =
          p.info.leadSamples == 0 ? leadQ8 : p.info.leadEmaQ8 + (leadQ8 - p.info.leadEmaQ8) / 8;
      ++p.info.leadSamples;
    }
  }
  if (bad) protocolError(p, nowNs_);
}

void NetServer::onJoinMatch(Peer& p, const Packet& packet) {
  JoinMatchMsg m;
  if (!decode(packet.data, m)) {
    protocolError(p, nowNs_);
    return;
  }
  const int have = popcount(p.info.slots);
  const int want = m.requestedSlots;
  if (want > have) {
    const sim::PlayerMask free = freeSlots();
    if (popcount(free) < want - have) return;  // nothing changes; the client sees no PlayerJoined
    assignSlots(p, lowestBits(free, want - have));
  } else if (want < have) {
    const sim::PlayerMask drop = highestBits(p.info.slots, have - want);
    p.info.slots = static_cast<sim::PlayerMask>(p.info.slots & ~drop);
    releaseSlots(drop, p.info.peer);
  }
}

void NetServer::onResync(Peer& p, const Packet& packet) {
  ResyncRequestMsg m;
  if (!decode(packet.data, m)) {
    protocolError(p, nowNs_);
    return;
  }
  if (p.hasResynced && nowNs_ - p.lastResyncNs < config_.minResyncIntervalNs) return;
  p.hasResynced = true;
  p.lastResyncNs = nowNs_;
  sendSnapshot(p);
  ++stats_.resyncsServed;
  ++p.info.resyncsServed;
  emit(ServerEventKind::ResyncServed, p.info.peer, p.info.slots);
}

void NetServer::onRoomMessage(Peer& p, const Packet& packet, MessageType type) {
  const uint64_t now = std::max(packet.receivedNs, p.roomBucketAtNs);
  const uint64_t cap = uint64_t{config_.roomMsgsPerSecond} * kNsPerSecond;
  p.roomBucketNs =
      std::min(cap, p.roomBucketNs + (now - p.roomBucketAtNs) * config_.roomMsgsPerSecond);
  p.roomBucketAtNs = now;
  if (p.roomBucketNs < kNsPerSecond) {
    ++stats_.roomRateLimited;
    return;
  }
  p.roomBucketNs -= kNsPerSecond;
  if (type == MessageType::Start) {
    StartMsg m;
    if (!decode(packet.data, m)) {
      protocolError(p, nowNs_);
      return;
    }
    emit(ServerEventKind::StartRequested, p.info.peer, p.info.slots);
    return;
  }
  RoomSettingsMsg m;
  if (!decode(packet.data, m)) {
    protocolError(p, nowNs_);
    return;
  }
  emit(ServerEventKind::RoomSettingsRequested, p.info.peer, p.info.slots, 0, m.settings);
}

// --- ticking ----------------------------------------------------------------------

void NetServer::tick(uint64_t nowNs) {
  nowNs_ = nowNs;
  expireReservations();
  const Tick t = nextTick();
  if (const std::optional<PendingRestart> r = restart_; r && t >= r->startTick) applyRestart(t, *r);
  const sim::MatchContent content = sim::matchContent(*content_, config_.match);
  if (config_.botInputs) {
    if (bots_ != 0) {
      sim::InputFrame botFrame{};
      botFrame.tick = t;
      botFrame.present = bots_;
      config_.botInputs(world_, content, botFrame);
      for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
        if (has(bots_, s)) {
          static_cast<void>(
              assembler_.put(PlayerId{static_cast<PlayerId::rep>(s)}, t, botFrame.inputs[s]));
        }
      }
    }
  } else {
    for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
      const PlayerId id{static_cast<PlayerId::rep>(s)};
      if (has(bots_, s)) {
        static_cast<void>(assembler_.put(
            id, t, sim::scriptedInput(botRng_, world_, content, id, config_.botConfig)));
      }
    }
  }
  // A reserved slot stands still until its owner is back.
  const sim::PlayerMask reserved = reservedSlots();
  for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
    if (has(reserved, s)) {
      static_cast<void>(assembler_.put(PlayerId{static_cast<PlayerId::rep>(s)}, t, {}));
    }
  }
  assembler_.setPresent(present_);
  sim::PlayerMask substituted = 0;
  const sim::InputFrame frame = assembler_.assembleNext(&substituted);
  stats_.substitutedLastTick = 0;
  for (Peer& p : peers_) {
    if (!p.used) continue;
    const auto n =
        static_cast<uint32_t>(popcount(static_cast<sim::PlayerMask>(substituted & p.info.slots)));
    p.info.substitutions += n;
    stats_.substitutedInputs += n;
    stats_.substitutedLastTick += n;
  }
  sink_.clear();
  sim::step(world_, frame, content, sink_);
  static_cast<void>(frames_.put(frame));
  ++stats_.ticks;
  broadcastAuth();
}

void NetServer::broadcastAuth() {
  const Tick newest = nextTick().plus(-1);
  const bool withChecksum =
      config_.checksumEveryTicks > 0 && world_.tick.value % config_.checksumEveryTicks == 0;
  const uint32_t sum = withChecksum ? sim::checksum(world_) : 0;
  for (Peer& p : peers_) {
    if (!p.used || p.info.state != PeerState::Active) continue;
    const int32_t redundancy = std::max<int32_t>(config_.authRedundancy, 1);
    int32_t count = redundancy;
    if (p.info.lastAuthAck) {
      count = std::clamp<int32_t>(newest.minus(*p.info.lastAuthAck), 1, redundancy);
    }
    std::optional<AuthInputsMsg> m = frames_.makeAuthInputsMsg(newest, static_cast<uint8_t>(count));
    if (!m) continue;
    if (p.marginSinceSend) {
      m->arrivalMargin = p.marginSinceSend;
    } else if (p.info.lastInputTick) {
      m->arrivalMargin = p.info.lastMargin;
    }
    p.marginSinceSend.reset();
    if (withChecksum) {
      m->checksumTick = world_.tick;
      m->checksum = sum;
    }
    std::array<std::byte, kAuthInputsMsgMaxBytes> out{};
    const size_t n = encode(*m, out);
    if (n > 0) sendUnreliable(p, {out.data(), n});
  }
}

}  // namespace kfm::net
