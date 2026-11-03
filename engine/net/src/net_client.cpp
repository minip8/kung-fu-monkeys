#include "kfm/net/net_client.hpp"

#include "kfm/core/assert.hpp"
#include "kfm/sim/step.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace kfm::net {
namespace {

[[nodiscard]] constexpr bool has(sim::PlayerMask m, uint16_t slot) {
  return ((static_cast<uint32_t>(m) >> slot) & 1u) != 0;
}

[[nodiscard]] constexpr sim::PlayerMask bit(uint16_t slot) {
  return static_cast<sim::PlayerMask>(1u << slot);
}

[[nodiscard]] constexpr PlayerId slotId(uint16_t s) {
  return PlayerId{static_cast<PlayerId::rep>(s)};
}

}  // namespace

void ClientConfig::setToken(std::span<const std::byte> bytes) {
  tokenLength = static_cast<uint8_t>(std::min<size_t>(bytes.size(), kMaxTokenBytes));
  token = {};
  std::copy_n(bytes.begin(), tokenLength, token.begin());
}

NetClient::NetClient(const sim::ContentDB& content, const ClientConfig& config)
    : content_(&content),
      config_(config),
      contentHash_(sim::contentHash(content)),
      lead_(config.lead) {}

// --- session ------------------------------------------------------------------------

void NetClient::resetSession() {
  state_ = ClientState::Idle;
  disconnectReason_.reset();
  matchConfig_ = {};
  simConfig_ = {};
  localSlots_ = 0;
  haveWelcome_ = false;
  haveSnapshot_ = false;
  helloSent_ = false;
  base_ = Tick{};
  verified_ = Tick{};
  confirmed_.reset();
  basePresent_ = 0;
  marginFrom_ = Tick{};
  awaitingResync_ = false;
  restartTick_.reset();
  resyncSentNs_ = 0;
  nextPingNs_ = 0;
  firstAuthTick_.reset();
  newestAuthPacketTick_.reset();
  authPacketsSinceFirst_ = 0;
  pong_ = {};
  world_ = sim::WorldState{};
  anchor_ = sim::WorldState{};
  for (Saved& s : ring_)
    s.valid = false;
  for (UsedFrame& u : used_)
    u.valid = false;
  auth_.clear();
  local_.clear();
  for (PendingChecksum& c : checksums_)
    c.valid = false;
  journal_.clear();
  rtt_.reset();
  lead_ = LeadController(config_.lead);
  clock_ = TickClock{};
}

void NetClient::connect(ITransport& transport, uint64_t nowNs) {
  resetSession();
  transport_ = &transport;
  state_ = ClientState::Connecting;
  nowNs_ = nowNs;
  lastRecvNs_ = nowNs;
  if (transport_->isConnected(kServerPeer)) sendHello(nowNs);
}

void NetClient::sendHello(uint64_t nowNs) {
  HelloMsg h;
  h.protocolVersion = kProtocolVersion;
  h.contentHash = contentHash_;
  h.buildId = config_.buildId;
  h.tokenLength = config_.tokenLength;
  h.token = config_.token;
  h.requestedSlots = std::min<uint8_t>(config_.requestedSlots, sim::kMaxPlayers);
  sendMsg(Lane::Reliable, h);
  helloSent_ = true;
  sendPing(nowNs);
}

void NetClient::disconnect() {
  if (transport_ == nullptr) return;
  if (transport_->isConnected(kServerPeer)) {
    sendMsg(Lane::Reliable, DisconnectMsg{DisconnectReason::Quit});
    transport_->disconnect(kServerPeer);
  }
  state_ = ClientState::Disconnected;
  disconnectReason_ = DisconnectReason::Quit;
}

void NetClient::beginFrame() {
  report_ = FrameReport{};
}

std::optional<Tick> NetClient::confirmedTick() const {
  return confirmed_;
}

std::optional<Tick> NetClient::estimatedServerTick(uint64_t nowNs) const {
  if (!pong_.valid) return std::nullopt;
  const uint64_t since = nowNs > pong_.recvNs ? nowNs - pong_.recvNs : 0;
  const uint64_t ahead = since + rtt_.srttNs() / 2;
  const auto ticks =
      static_cast<int32_t>(std::min<uint64_t>(ahead * sim::kTickHz / 1'000'000'000ull, 1u << 30));
  return pong_.tick.plus(ticks);
}

// --- receiving ----------------------------------------------------------------------

void NetClient::pump(uint64_t nowNs) {
  nowNs_ = nowNs;
  if (transport_ == nullptr) return;
  ConnectionEvents events;
  transport_->poll(nowNs, events);
  for (const ConnectionEvent& e : events.span()) {
    if (e.peer != kServerPeer) continue;
    if (e.kind == ConnectionEventKind::Connected) {
      if (state_ == ClientState::Connecting && !helloSent_) sendHello(nowNs);
    } else if (state_ != ClientState::Rejected && state_ != ClientState::Disconnected) {
      state_ = ClientState::Disconnected;
      if (!disconnectReason_) disconnectReason_ = DisconnectReason::Timeout;
    }
  }

  Packet packet;
  while (transport_->recv(packet)) {
    if (packet.peer != kServerPeer) continue;
    ++stats_.packetsReceived;
    stats_.bytesReceived += packet.data.size();
    lastRecvNs_ = std::max(lastRecvNs_, packet.receivedNs);
    onPacket(packet);
  }

  const bool live = state_ == ClientState::Connecting || state_ == ClientState::Syncing ||
                    state_ == ClientState::Running;
  if (!live) return;
  if (nowNs > lastRecvNs_ && nowNs - lastRecvNs_ > config_.serverTimeoutNs) {
    transport_->disconnect(kServerPeer);
    state_ = ClientState::Disconnected;
    disconnectReason_ = DisconnectReason::Timeout;
    return;
  }
  if (helloSent_ && nowNs >= nextPingNs_) sendPing(nowNs);
  if (state_ == ClientState::Syncing) tryStart(nowNs);
  if (awaitingResync_ && nowNs - resyncSentNs_ > config_.resyncRetryNs) {
    awaitingResync_ = false;  // lost or ignored: ask again at the next mismatch
  }
}

void NetClient::onPacket(const Packet& packet) {
  const Decoded<MessageType> type = peekType(packet.data);
  if (!type) {
    ++stats_.decodeErrors;
    return;
  }
  switch (*type) {
    case MessageType::Welcome: {
      WelcomeMsg w;
      if (!decode(packet.data, w)) break;
      if (state_ != ClientState::Connecting) return;
      localSlots_ = w.assignedSlots;
      matchConfig_ = w.config;
      haveWelcome_ = true;
      state_ = ClientState::Syncing;
      return;
    }
    case MessageType::Reject: {
      RejectMsg r;
      if (!decode(packet.data, r)) break;
      rejectReason_ = r.reason;
      state_ = ClientState::Rejected;
      return;
    }
    case MessageType::Snapshot:
      onSnapshot(packet);
      return;
    case MessageType::AuthInputs:
      onAuthInputs(packet);
      return;
    case MessageType::Ping: {
      PingMsg ping;
      if (!decode(packet.data, ping)) break;
      const uint64_t hold = nowNs_ > packet.receivedNs ? nowNs_ - packet.receivedNs : 0;
      sendMsg(Lane::Unreliable,
              PongMsg{ping.sendNs, world_.tick,
                      static_cast<uint32_t>(std::min<uint64_t>(hold, UINT32_MAX))});
      return;
    }
    case MessageType::Pong: {
      PongMsg pong;
      if (!decode(packet.data, pong)) break;
      const uint64_t sent = pong.echoSendNs + pong.holdNs;
      if (packet.receivedNs < sent) return;  // not ours, or a clock we cannot use
      rtt_.addSample(packet.receivedNs - sent);
      stats_.rttNs = rtt_.srttNs();
      stats_.jitterNs = rtt_.jitterNs();
      stats_.minRttNs = rtt_.minNs();
      stats_.maxRttNs = rtt_.maxNs();
      if (!pong_.valid || pong.tick >= pong_.tick)
        pong_ = LastPong{pong.tick, packet.receivedNs, true};
      return;
    }
    case MessageType::PlayerJoined: {
      PlayerJoinedMsg j;
      if (!decode(packet.data, j)) break;
      if (j.yours) localSlots_ = static_cast<sim::PlayerMask>(localSlots_ | bit(j.slot.value));
      return;
    }
    case MessageType::PlayerLeft: {
      PlayerLeftMsg l;
      if (!decode(packet.data, l)) break;
      localSlots_ = static_cast<sim::PlayerMask>(localSlots_ & ~bit(l.slot.value));
      return;
    }
    case MessageType::MatchConfig: {
      MatchConfigMsg m;
      if (!decode(packet.data, m)) break;
      matchConfig_ = m.config;
      // The world from startTick on is the new match: until its snapshot is
      // loaded, server checksums of those ticks cannot match our timeline.
      restartTick_ = m.startTick;
      return;
    }
    case MessageType::Disconnect: {
      DisconnectMsg d;
      if (!decode(packet.data, d)) break;
      disconnectReason_ = d.reason;
      state_ = ClientState::Disconnected;
      return;
    }
    default:
      return;  // client-to-server types are ignored
  }
  ++stats_.decodeErrors;
}

void NetClient::onAuthInputs(const Packet& packet) {
  AuthInputsMsg m;
  if (!decode(packet.data, m)) {
    ++stats_.decodeErrors;
    return;
  }
  ++stats_.authPackets;
  const Tick newest = m.endTick().plus(-1);
  if (!firstAuthTick_) firstAuthTick_ = newest;
  if (!newestAuthPacketTick_ || newest > *newestAuthPacketTick_) newestAuthPacketTick_ = newest;
  ++authPacketsSinceFirst_;
  if (newest >= *firstAuthTick_) {
    const uint64_t expected =
        static_cast<uint64_t>(newestAuthPacketTick_->minus(*firstAuthTick_)) + 1;
    const uint64_t got = std::min(authPacketsSinceFirst_, expected);
    stats_.lossPpm = static_cast<uint32_t>((expected - got) * kRateOnePpm / expected);
  }
  for (uint8_t i = 0; i < m.count; ++i)
    static_cast<void>(auth_.put(m.frames[i]));

  if (m.arrivalMargin) {
    stats_.arrivalMargin = *m.arrivalMargin;
    // The report is about the input of tick ≈ serverTick + margin; reports
    // about inputs sent before the last sync or jump are stale.
    const Tick inputTick = m.endTick().plus(*m.arrivalMargin);
    if (state_ == ClientState::Running && inputTick >= marginFrom_) {
      lead_.onArrivalMargin(*m.arrivalMargin);
      stats_.arrivalMarginQ8 = lead_.smoothedMarginQ8();
    }
  }
  if (m.checksumTick) {
    PendingChecksum* slot = nullptr;
    for (PendingChecksum& c : checksums_) {
      if (c.valid && c.tick == *m.checksumTick) return;  // a redundant copy
      if (!c.valid && slot == nullptr) slot = &c;
    }
    if (slot == nullptr) {
      slot = &*std::ranges::min_element(
          checksums_,
          [](const PendingChecksum& a, const PendingChecksum& b) { return a.tick < b.tick; });
      ++stats_.checksumsSkipped;
    }
    *slot = PendingChecksum{*m.checksumTick, m.checksum, true};
  }
}

void NetClient::onSnapshot(const Packet& packet) {
  if (!decode(packet.data, snapshot_)) {
    ++stats_.decodeErrors;
    return;
  }
  const Tick snapTick = snapshot_.world.tick;
  const bool restart = restartTick_ && snapTick >= *restartTick_;
  if (state_ == ClientState::Syncing) {
    haveSnapshot_ = true;
    matchConfig_ = snapshot_.config;
    simConfig_ = snapshot_.config;
    if (restart) restartTick_.reset();
    tryStart(nowNs_);
    return;
  }
  if (state_ != ClientState::Running) return;
  if (restart) {
    restartTick_.reset();
    matchConfig_ = snapshot_.config;
    simConfig_ = snapshot_.config;
    ++stats_.matchRestarts;
    report_.matchRestarted = true;
  }
  // Resync or restart: hard load, then predict forward to where we were.
  const Tick runTo = spectator() ? snapTick : std::max(world_.tick, snapTick);
  loadSnapshot(runTo);
  awaitingResync_ = false;
}

void NetClient::tryStart(uint64_t nowNs) {
  if (!haveWelcome_ || !haveSnapshot_ || !pong_.valid ||
      rtt_.samples() < std::max<uint32_t>(config_.syncRttSamples, 1)) {
    return;
  }
  const Tick snapTick = snapshot_.world.tick;
  Tick runTo = snapTick;
  if (!spectator()) {
    const Tick server = estimatedServerTick(nowNs).value_or(snapTick);
    const Tick target = server.plus(targetLeadTicks(rtt_) - static_cast<int32_t>(inputDelay()));
    if (target > runTo) runTo = target;
  }
  state_ = ClientState::Running;
  loadSnapshot(runTo);
  marginFrom_ = world_.tick.plus(static_cast<int32_t>(inputDelay()));
  lead_.reset();
  clock_.start(nowNs);
}

void NetClient::loadSnapshot(Tick runTo) {
  world_ = snapshot_.world;
  anchor_ = world_;
  base_ = world_.tick;
  verified_ = base_;
  for (Saved& s : ring_)
    s.valid = false;
  for (UsedFrame& u : used_)
    u.valid = false;
  confirmed_.reset();
  advanceConfirmed();
  basePresent_ = static_cast<sim::PlayerMask>(world_.rules.present | localSlots_);
  for (PendingChecksum& c : checksums_)
    if (c.valid && c.tick < base_) c.valid = false;
  ++stats_.snapshotsLoaded;
  report_.snapshotLoaded = true;
  report_.rolledBack = true;
  while (world_.tick < runTo)
    stepOne();
  advanceVerified();
}

// --- timeline -----------------------------------------------------------------------

NetClient::Saved* NetClient::savedAt(Tick t) {
  Saved& s = ring_[t.value % kRollbackWindow];
  return s.valid && s.state.tick == t && t >= base_ ? &s : nullptr;
}

const NetClient::Saved* NetClient::savedAt(Tick t) const {
  const Saved& s = ring_[t.value % kRollbackWindow];
  return s.valid && s.state.tick == t && t >= base_ ? &s : nullptr;
}

NetClient::UsedFrame* NetClient::usedAt(Tick t) {
  UsedFrame& u = used_[t.value % kInputHistoryTicks];
  return u.valid && u.frame.tick == t ? &u : nullptr;
}

const sim::WorldState* NetClient::stateAt(Tick t) const {
  if (t == world_.tick) return &world_;
  if (t == anchor_.tick && t == verified_) return &anchor_;
  const Saved* s = savedAt(t);
  return s != nullptr ? &s->state : nullptr;
}

const sim::WorldState& NetClient::prevWorld() const {
  if (world_.tick.value == 0) return world_;
  const Saved* s = savedAt(world_.tick.plus(-1));
  return s != nullptr ? s->state : world_;
}

const sim::InputFrame* NetClient::latestAuthBefore(Tick t) const {
  const std::optional<Tick> newest = auth_.newest();
  if (!newest || t.value == 0) return nullptr;
  Tick k = *newest < t ? *newest : t.plus(-1);
  for (uint32_t n = 0; n < kInputHistoryTicks && k >= base_; ++n) {
    if (const sim::InputFrame* f = auth_.get(k)) return f;
    if (k.value == 0) break;
    k = k.plus(-1);
  }
  return nullptr;
}

sim::InputFrame NetClient::buildFrame(Tick t, bool* authoritative) const {
  if (const sim::InputFrame* a = t >= base_ ? auth_.get(t) : nullptr) {
    *authoritative = true;
    return *a;
  }
  *authoritative = false;
  sim::InputFrame f;
  f.tick = t;
  const sim::InputFrame* last = latestAuthBefore(t);
  f.present = last != nullptr ? last->present : basePresent_;
  for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
    if (!has(f.present, s)) continue;
    if (has(localSlots_, s)) {
      f.inputs[s] = local_.getOrRepeat(slotId(s), t);
    } else if (last != nullptr && has(last->present, s)) {
      f.inputs[s] = last->inputs[s];  // repeat last known input
    }
  }
  return f;
}

void NetClient::stepOne() {
  const Tick t = world_.tick;
  bool authoritative = false;
  const sim::InputFrame f = buildFrame(t, &authoritative);
  Saved& saved = ring_[t.value % kRollbackWindow];
  saved.state = world_;
  saved.valid = true;
  UsedFrame& used = used_[t.value % kInputHistoryTicks];
  used.frame = f;
  used.valid = true;
  used.authoritative = authoritative;
  journal_.beginTick(t);
  sink_.clear();
  sim::step(world_, f, sim::matchContent(*content_, simConfig_), sink_);
  for (const sim::SimEvent& e : sink_)
    journal_.record(e);
  journal_.endTick();
}

void NetClient::advanceVerified() {
  const Tick from = verified_;
  while (verified_ < world_.tick) {
    const UsedFrame* u = usedAt(verified_);
    if (u == nullptr || !u->authoritative) break;
    ++verified_;
  }
  if (verified_ == from) return;
  // Keep the anchor at verified_: copy it when the ring still holds that
  // state, else step the old anchor forward over the authoritative frames.
  if (const sim::WorldState* s = stateAt(verified_)) {
    anchor_ = *s;
    return;
  }
  while (anchor_.tick < verified_) {
    const UsedFrame* u = usedAt(anchor_.tick);
    KFM_CHECK(u != nullptr && u->authoritative);
    scratchSink_.clear();
    sim::step(anchor_, u->frame, sim::matchContent(*content_, simConfig_), scratchSink_);
  }
}

void NetClient::advanceConfirmed() {
  Tick next = confirmed_ ? confirmed_->plus(1) : base_;
  while (const sim::InputFrame* a = auth_.get(next)) {
    for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
      if (!has(localSlots_, s) || !has(a->present, s)) continue;
      const std::optional<sim::PlayerInput> sent = local_.slot(slotId(s)).get(next);
      if (sent && *sent != a->inputs[s]) ++stats_.substitutionsSeen;
    }
    confirmed_ = next;
    next = next.plus(1);
  }
}

void NetClient::rollbackTo(Tick t) {
  const Tick end = world_.tick;
  if (const Saved* saved = savedAt(t)) {
    world_ = saved->state;
  } else {
    // Older than the ring: re-simulate from the verified anchor.
    world_ = anchor_;
    ++stats_.anchorRollbacks;
  }
  const auto depth = static_cast<uint32_t>(end.minus(world_.tick));
  while (world_.tick < end)
    stepOne();
  ++stats_.rollbacks;
  stats_.resimulatedTicks += depth;
  ++stats_.rollbackDepth[std::min<uint32_t>(depth, kRollbackDepthBuckets - 1)];
  stats_.maxRollbackDepth = std::max(stats_.maxRollbackDepth, depth);
  report_.rolledBack = true;
  ++report_.rollbacks;
  report_.maxDepth = std::max(report_.maxDepth, depth);
}

void NetClient::reconcile() {
  if (state_ != ClientState::Running) return;
  advanceConfirmed();
  std::optional<Tick> first;
  for (Tick t = verified_; t < world_.tick; t = t.plus(1)) {
    const sim::InputFrame* a = auth_.get(t);
    if (a == nullptr) continue;
    UsedFrame* u = usedAt(t);
    if (u == nullptr) {
      if (!first) first = t;
      continue;
    }
    if (u->authoritative) continue;
    if (*a == u->frame) {
      u->authoritative = true;
      continue;
    }
    ++stats_.mispredictedTicks;
    if (a->present != u->frame.present) ++stats_.mispredictedPresent;
    for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
      if (!has(a->present, s) || !has(u->frame.present, s)) continue;
      if (a->inputs[s] == u->frame.inputs[s]) continue;
      if (has(localSlots_, s)) {
        ++stats_.mispredictedLocal;
      } else {
        ++stats_.mispredictedRemote;
      }
    }
    if (!first) first = t;
  }
  if (first) {
    // The anchor reaches back to verified_, the used-frame history 128
    // ticks; beyond that only a snapshot can repair the timeline.
    if (savedAt(*first) != nullptr ||
        world_.tick.minus(verified_) < static_cast<int32_t>(kInputHistoryTicks)) {
      rollbackTo(*first);
    } else {
      ++stats_.rollbackOverflows;
      requestResync(*first, 0);
    }
  }
  advanceVerified();
  checkChecksums();
  // Events of verified ticks, and of ticks no rollback can reach, are final.
  const int64_t ringFloor = int64_t{world_.tick.value} - 1 - kRollbackWindow;
  const int64_t fin = std::max<int64_t>(int64_t{verified_.value} - 1, ringFloor);
  if (fin >= 0) journal_.confirm(Tick{static_cast<Tick::rep>(fin)});
}

void NetClient::checkChecksums() {
  for (PendingChecksum& c : checksums_) {
    if (!c.valid) continue;
    if (c.tick < base_) {
      c.valid = false;
      continue;
    }
    if (restartTick_ && c.tick >= *restartTick_) {
      c.valid = false;  // a checksum of the announced new match
      ++stats_.checksumsSkipped;
      continue;
    }
    // The state at c.tick follows from the frames before it, which must all
    // be authoritative, and it must have been simulated.
    if (c.tick > verified_ || c.tick > world_.tick) continue;
    c.valid = false;
    if (awaitingResync_) continue;
    const sim::WorldState* s = stateAt(c.tick);
    if (s == nullptr) {
      ++stats_.checksumsSkipped;
      continue;
    }
    const uint32_t local = sim::checksum(*s);
    ++stats_.checksumsCompared;
    if (local == c.sum) continue;
    ++stats_.checksumMismatches;
    if (desyncHook_) desyncHook_(DesyncInfo{c.tick, c.sum, local, s});
    requestResync(c.tick, local);
  }
}

void NetClient::requestResync(Tick tick, uint32_t localSum) {
  if (awaitingResync_) return;
  sendMsg(Lane::Reliable, ResyncRequestMsg{tick, localSum});
  awaitingResync_ = true;
  resyncSentNs_ = nowNs_;
  ++stats_.resyncRequests;
}

// --- ticking ------------------------------------------------------------------------

uint32_t NetClient::inputDelay() const {
  return std::min<uint32_t>(config_.inputDelayTicks, kMaxInputDelayTicks);
}

uint32_t NetClient::spectatorRatePpm() const {
  const int64_t avail = confirmed_ ? int64_t{confirmed_->plus(1).minus(world_.tick)} : 0;
  const int64_t err = avail - config_.spectatorDelayTicks;
  const int64_t dev = config_.lead.maxDeviationPpm;
  return static_cast<uint32_t>(int64_t{kRateOnePpm} +
                               std::clamp(err * config_.lead.gainPpmPerTick, -dev, dev));
}

uint32_t NetClient::ticksDue(uint64_t nowNs) {
  nowNs_ = nowNs;
  if (state_ != ClientState::Running) {
    recordBatch(nowNs, 0, kRateOnePpm);
    return 0;
  }
  if (spectator()) {
    const uint32_t rate = spectatorRatePpm();
    const uint32_t n = clock_.advance(nowNs, rate, config_.maxTicksPerCall);
    int32_t avail = confirmed_ ? confirmed_->plus(1).minus(world_.tick) : 0;
    const int32_t delay = config_.spectatorDelayTicks;
    if (avail > delay + static_cast<int32_t>(kRollbackWindow)) {
      fastForward(avail - delay, false);
      ++stats_.hardJumps;
      avail = delay;
    }
    stats_.clockDroppedTicks = clock_.droppedTicks();
    const uint32_t due = std::min<uint32_t>(n, static_cast<uint32_t>(std::max(avail, 0)));
    recordBatch(nowNs, due, rate);
    return due;
  }
  stats_.ratePpm = lead_.ratePpm();
  const uint32_t n = clock_.advance(nowNs, stats_.ratePpm, config_.maxTicksPerCall);
  stats_.clockDroppedTicks = clock_.droppedTicks();
  const int32_t jump = lead_.takeJumpTicks();
  if (jump > 0) {
    fastForward(jump, true);
    ++stats_.hardJumps;
    marginFrom_ = world_.tick.plus(static_cast<int32_t>(inputDelay()));
  }
  stats_.targetLeadTicks = targetLeadTicks(rtt_);
  if (const std::optional<Tick> server = estimatedServerTick(nowNs)) {
    stats_.leadTicks = world_.tick.minus(*server);
  }
  recordBatch(nowNs, n, stats_.ratePpm);
  return n;
}

void NetClient::recordBatch(uint64_t nowNs, uint32_t ticks, uint32_t ratePpm) {
  dueNowNs_ = nowNs;
  dueTicks_ = ticks;
  dueRatePpm_ = ratePpm;
}

uint64_t NetClient::tickStartNs(uint32_t i) const {
  const uint64_t back = uint64_t{dueTicks_ > i ? dueTicks_ - i : 0} * tickWallNs();
  return dueNowNs_ > back ? dueNowNs_ - back : 0;
}

void NetClient::tick(std::span<const sim::PlayerInput> inputs) {
  if (state_ != ClientState::Running) return;
  if (spectator()) {
    if (auth_.get(world_.tick) == nullptr) return;  // a spectator never predicts
    stepOne();
    advanceVerified();
    ++stats_.ticks;
    return;
  }
  const Tick target = world_.tick.plus(static_cast<int32_t>(inputDelay()));
  size_t k = 0;
  for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
    if (!has(localSlots_, s)) continue;
    const PlayerId id = slotId(s);
    const sim::PlayerInput in = k < inputs.size() ? inputs[k] : local_.getOrRepeat(id, target);
    static_cast<void>(local_.put(id, target, in));
    ++k;
  }
  sendInputs();
  stepOne();
  advanceVerified();
  ++stats_.ticks;
}

void NetClient::fastForward(int32_t ticks, bool recordLocal) {
  for (int32_t i = 0; i < ticks; ++i) {
    if (recordLocal) {
      const Tick target = world_.tick.plus(static_cast<int32_t>(inputDelay()));
      for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
        if (!has(localSlots_, s)) continue;
        static_cast<void>(local_.put(slotId(s), target, local_.getOrRepeat(slotId(s), target)));
      }
    }
    stepOne();
  }
  advanceVerified();
  if (recordLocal) sendInputs();
  report_.rolledBack = true;
}

std::span<const sim::SimEvent> NetClient::drainEvents() {
  const size_t n = journal_.drain(frameEvents_);
  return {frameEvents_.data(), n};
}

void NetClient::debugCorrupt(const std::function<void(sim::WorldState&)>& f) {
  f(world_);
  f(anchor_);
  for (Saved& s : ring_)
    if (s.valid) f(s.state);
}

// --- sending ------------------------------------------------------------------------

void NetClient::send(Lane lane, std::span<const std::byte> bytes) {
  if (transport_ == nullptr) return;
  if (lane == Lane::Reliable) {
    transport_->sendReliable(kServerPeer, bytes);
  } else {
    transport_->sendUnreliable(kServerPeer, bytes);
  }
  ++stats_.packetsSent;
  stats_.bytesSent += bytes.size();
}

template <class M>
void NetClient::sendMsg(Lane lane, const M& m) {
  const size_t n = encode(m, buf_);
  if (n > 0) send(lane, {buf_.data(), n});
}

void NetClient::sendInputs() {
  if (localSlots_ == 0) return;
  const Tick newest = world_.tick.plus(static_cast<int32_t>(inputDelay()));
  const int32_t redundancy = std::max<int32_t>(config_.inputRedundancy, 1);
  int32_t count = redundancy;
  if (const std::optional<Tick> assembled = auth_.newest()) {
    // The server has assembled every tick up to its newest frame.
    count = std::clamp<int32_t>(newest.minus(*assembled), 1, redundancy);
  }
  sendMsg(Lane::Unreliable,
          local_.makeInputMsg(localSlots_, newest, static_cast<uint8_t>(count), confirmed_));
}

void NetClient::sendPing(uint64_t nowNs) {
  sendMsg(Lane::Unreliable, PingMsg{nowNs, world_.tick});
  nextPingNs_ = nowNs + (state_ == ClientState::Running ? config_.pingIntervalNs
                                                        : config_.syncPingIntervalNs);
}

}  // namespace kfm::net
