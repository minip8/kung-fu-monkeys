// NetServer driven by hand-written messages over a zero-latency loopback
// (architecture §6.6, §6.7): handshake checks, input validation and rate
// limits, reconnect reservations, spectators, resync, ping, the bot input
// source, the human lead estimate and the room's pieces (v0.42: nicknames,
// RoomSettings / Start events, RoomState to joiners, releaseReservations).
#include "kfm/net/net_server.hpp"

#include "kfm/core/rng.hpp"
#include "kfm/net/loopback.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/scripted_inputs.hpp"
#include "kfm/sim/step.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace kfm;
using namespace kfm::net;

namespace {

constexpr uint64_t kMs = 1'000'000;

struct Received {
  MessageType type{MessageType::Input};
  Lane lane{};
  std::vector<std::byte> bytes;
};

// A raw client endpoint: sends encoded messages, collects what comes back.
class Raw {
 public:
  explicit Raw(LoopbackTransport& t) : t_(&t) {}

  template <class M>
  void reliable(const M& m) {
    std::vector<std::byte> buf(kMaxReliableBytes);
    const size_t n = encode(m, buf);
    REQUIRE(n > 0);
    t_->sendReliable(kServerPeer, {buf.data(), n});
  }
  template <class M>
  void unreliable(const M& m) {
    std::vector<std::byte> buf(kMaxUnreliableBytes);
    const size_t n = encode(m, buf);
    REQUIRE(n > 0);
    t_->sendUnreliable(kServerPeer, {buf.data(), n});
  }
  void rawReliable(std::span<const std::byte> bytes) const { t_->sendReliable(kServerPeer, bytes); }

  void poll(uint64_t now) {
    ConnectionEvents ev;
    t_->poll(now, ev);
    for (const ConnectionEvent& e : ev.span())
      if (e.kind == ConnectionEventKind::Disconnected) closed = true;
    Packet p;
    while (t_->recv(p)) {
      const auto type = peekType(p.data);
      REQUIRE(type.has_value());
      inbox.push_back(Received{*type, p.lane, {p.data.begin(), p.data.end()}});
    }
  }

  [[nodiscard]] size_t count(MessageType type) const {
    size_t n = 0;
    for (const Received& r : inbox)
      if (r.type == type) ++n;
    return n;
  }
  // The newest message of a type; fails the test when there is none.
  template <class M>
  [[nodiscard]] M last(MessageType type) const {
    M m{};
    bool found = false;
    for (const Received& r : std::views::reverse(inbox)) {
      if (r.type == type && decode(r.bytes, m)) {
        found = true;
        break;
      }
    }
    if (!found) FAIL("no message of type " << static_cast<int>(type));
    return m;
  }

  LoopbackTransport* t_;
  std::vector<Received> inbox;
  bool closed{false};
};

struct Rig {
  explicit Rig(ServerConfig config = {}) : net(5) {
    server = std::make_unique<NetServer>(net.server(), sim::defaultContent(), std::move(config));
  }

  Raw& client() {
    raws.push_back(std::make_unique<Raw>(net.connectClient(now)));
    pump();
    return *raws.back();
  }

  // Advances 1 ms: server pump, then every raw client polls.
  void pump() {
    now += kMs;
    server->pump(now);
    for (auto& r : raws)
      r->poll(now);
  }
  void tick() {
    server->tick(now);
    pump();
  }

  [[nodiscard]] static HelloMsg hello(uint8_t slots, const std::string& token = {}) {
    HelloMsg h;
    h.contentHash = sim::contentHash(sim::defaultContent());
    h.buildId = 0;
    h.requestedSlots = slots;
    h.tokenLength = static_cast<uint8_t>(token.size());
    for (size_t i = 0; i < token.size(); ++i)
      h.token[i] = static_cast<std::byte>(token[i]);
    return h;
  }

  Raw& join(uint8_t slots, const std::string& token = {}) {
    Raw& r = client();
    r.reliable(hello(slots, token));
    pump();
    REQUIRE(r.count(MessageType::Welcome) == 1);
    return r;
  }

  LoopbackNetwork net;
  std::unique_ptr<NetServer> server;
  std::vector<std::unique_ptr<Raw>> raws;
  uint64_t now{0};
};

InputMsg inputFor(sim::PlayerMask slots, Tick start, uint8_t count, sim::PlayerInput in = {}) {
  InputMsg m;
  m.slots = slots;
  m.startTick = start;
  m.count = count;
  for (auto& slot : m.inputs)
    for (uint8_t i = 0; i < count; ++i)
      slot[i] = in;
  return m;
}

}  // namespace

TEST_CASE("the handshake welcomes, snapshots and announces a joiner", "[net][server]") {
  Rig rig;
  const Raw& a = rig.join(2);
  const auto w = a.last<WelcomeMsg>(MessageType::Welcome);
  CHECK(w.assignedSlots == 0b11);
  CHECK(w.serverTick == rig.server->nextTick());
  CHECK(a.count(MessageType::Snapshot) == 1);
  CHECK(a.count(MessageType::PlayerJoined) == 2);
  CHECK(a.last<PlayerJoinedMsg>(MessageType::PlayerJoined).yours);
  CHECK(rig.server->present() == 0b11);
  REQUIRE(rig.server->events().size() == 1);
  CHECK(rig.server->events()[0].kind == ServerEventKind::PeerJoined);

  // A second client sees the first one's slots are taken and is told about
  // its own join as "yours", while the first hears it as someone else's.
  const Raw& b = rig.join(1);
  CHECK(b.last<WelcomeMsg>(MessageType::Welcome).assignedSlots == 0b100);
  CHECK(!a.last<PlayerJoinedMsg>(MessageType::PlayerJoined).yours);
  CHECK(a.last<PlayerJoinedMsg>(MessageType::PlayerJoined).slot == PlayerId{2});

  // From the next tick the frames carry the new roster.
  rig.tick();
  const auto auth = b.last<AuthInputsMsg>(MessageType::AuthInputs);
  CHECK(auth.frames[auth.count - 1].present == 0b111);
}

TEST_CASE("Hello is checked: version, content, build, token, room", "[net][server]") {
  ServerConfig cfg;
  cfg.buildId = 7;
  cfg.acceptToken = [](std::span<const std::byte> t) { return t.size() == 3; };
  Rig rig(cfg);
  const auto rejectedWith = [&](const HelloMsg& h) {
    Raw& r = rig.client();
    r.reliable(h);
    rig.pump();
    rig.pump();
    const auto rej = r.last<RejectMsg>(MessageType::Reject);
    CHECK(r.count(MessageType::Welcome) == 0);
    return rej.reason;
  };
  HelloMsg good = Rig::hello(1, "abc");
  good.buildId = 7;

  HelloMsg h = good;
  h.protocolVersion = kProtocolVersion + 1;
  CHECK(rejectedWith(h) == RejectReason::ProtocolMismatch);
  h = good;
  h.contentHash ^= 1;
  CHECK(rejectedWith(h) == RejectReason::ContentMismatch);
  h = good;
  h.buildId = 8;
  CHECK(rejectedWith(h) == RejectReason::BuildMismatch);
  h = Rig::hello(1, "abcd");
  h.buildId = 7;
  CHECK(rejectedWith(h) == RejectReason::BadToken);
  h = good;
  h.requestedSlots = sim::kMaxPlayers;
  Raw& full = rig.client();
  full.reliable(h);
  rig.pump();
  REQUIRE(full.count(MessageType::Welcome) == 1);
  HelloMsg other = Rig::hello(1, "xyz");  // the same token would take the slots over
  other.buildId = 7;
  CHECK(rejectedWith(other) == RejectReason::MatchFull);
  CHECK(rig.server->stats().rejects == 5);
}

TEST_CASE("inputs are accepted only for owned slots and valid ranges", "[net][server]") {
  Rig rig;
  Raw& a = rig.join(1);  // slot 0
  Raw& b = rig.join(1);  // slot 1
  const Tick next = rig.server->nextTick();
  sim::PlayerInput dash{};
  dash.buttons = sim::buttonBit(sim::Button::Dash);

  b.unreliable(inputFor(0b01, next, 1, dash));  // b claims a's slot
  rig.pump();
  CHECK(rig.server->stats().rejectedInputs == 1);
  CHECK(rig.server->stats().inputsAccepted == 0);

  sim::PlayerInput bad{};
  bad.buttons = 0x80;  // no such button
  a.unreliable(inputFor(0b01, next, 1, bad));
  rig.pump();
  CHECK(rig.server->stats().rejectedInputs == 2);

  a.unreliable(inputFor(0b01, next.plus(InputFrameAssembler::kMaxLeadTicks + 1), 1));
  rig.pump();
  CHECK(rig.server->stats().rejectedInputs == 3);

  a.unreliable(inputFor(0b01, next, 1, dash));
  rig.pump();
  CHECK(rig.server->stats().inputsAccepted == 1);
  rig.tick();
  const sim::InputFrame* f = rig.server->frames().get(next);
  REQUIRE(f != nullptr);
  CHECK(f->inputs[0] == dash);
  CHECK(f->inputs[1] == sim::PlayerInput{});            // b's forged dash never landed
  CHECK(rig.server->stats().substitutedLastTick == 1);  // b sent nothing for itself

  // Redundant copies of an assembled tick are late, not errors.
  a.unreliable(inputFor(0b01, next, 2, dash));
  rig.pump();
  CHECK(rig.server->stats().inputsLate == 1);
  CHECK(rig.server->peerInfo(a.t_->self()).value_or(ServerPeerInfo{}).protocolErrors == 2);
}

TEST_CASE("Input messages over the rate limit are dropped", "[net][server]") {
  ServerConfig cfg;
  cfg.inputMsgsPerSecond = 70;
  Rig rig(cfg);
  Raw& a = rig.join(1);
  const Tick next = rig.server->nextTick();
  for (int i = 0; i < 100; ++i)
    a.unreliable(inputFor(0b01, next, 1));
  rig.pump();
  CHECK(rig.server->stats().rateLimited == 30);  // a full bucket holds one second
  CHECK(rig.server->peerInfo(a.t_->self()).value_or(ServerPeerInfo{}).inputMsgs == 70);
  // A second later the bucket has refilled.
  for (int i = 0; i < 1000; ++i)
    rig.pump();
  for (int i = 0; i < 70; ++i)
    a.unreliable(inputFor(0b01, next, 1));
  rig.pump();
  CHECK(rig.server->stats().rateLimited == 30);
}

TEST_CASE("a peer that keeps sending garbage is kicked", "[net][server]") {
  ServerConfig cfg;
  cfg.maxProtocolErrors = 4;
  Rig rig(cfg);
  const Raw& a = rig.join(1);
  const std::array<std::byte, 3> junk{std::byte{0xEE}, std::byte{1}, std::byte{2}};
  for (int i = 0; i < 5; ++i)
    a.rawReliable(junk);
  rig.pump();
  rig.pump();
  CHECK(rig.server->stats().kicks == 1);
  CHECK(a.last<DisconnectMsg>(MessageType::Disconnect).reason == DisconnectReason::ProtocolError);
  CHECK(a.closed);
  CHECK(rig.server->present() == 0);  // a kick releases the slots at once
}

TEST_CASE("a dropped peer's slots are reserved for its token", "[net][server]") {
  ServerConfig cfg;
  cfg.reconnectGraceTicks = 30;
  Rig rig(cfg);
  const Raw& a = rig.join(1, "tok");
  Raw& b = rig.join(1);
  const PeerId first = a.t_->self();
  rig.net.cut(first);
  rig.pump();
  CHECK(rig.server->present() == 0b11);  // reserved, still present
  CHECK(!rig.server->ownerOf(PlayerId{0}).isValid());
  // The reserved slot is fed idle inputs, not counted as a substitution.
  for (int i = 0; i < 10; ++i) {
    b.unreliable(inputFor(0b10, rig.server->nextTick(), 1));
    rig.pump();
    rig.tick();
  }
  CHECK(rig.server->stats().substitutedInputs == 0);

  // Same token: the slot comes back with a fresh snapshot.
  const Raw& again = rig.join(1, "tok");
  CHECK(again.last<WelcomeMsg>(MessageType::Welcome).assignedSlots == 0b01);
  CHECK(again.count(MessageType::Snapshot) == 1);
  CHECK(rig.server->stats().reconnects == 1);
  CHECK(rig.server->ownerOf(PlayerId{0}) == again.t_->self());

  // Dropped again and not back in time: the slot is released.
  rig.net.cut(again.t_->self());
  rig.pump();
  for (int i = 0; i < 31; ++i)
    rig.tick();
  CHECK(rig.server->present() == 0b10);
  CHECK(b.last<PlayerLeftMsg>(MessageType::PlayerLeft).slot == PlayerId{0});
}

TEST_CASE("a token in use takes the slots over from the stale connection", "[net][server]") {
  Rig rig;
  const Raw& a = rig.join(2, "same");
  const Raw& b = rig.join(1, "same");
  CHECK(b.last<WelcomeMsg>(MessageType::Welcome).assignedSlots == 0b11);
  CHECK(a.closed);
  CHECK(rig.server->present() == 0b11);
}

TEST_CASE("spectators get frames but own no slot; JoinMatch adds and drops slots",
          "[net][server]") {
  Rig rig;
  const Raw& p = rig.join(1);
  Raw& s = rig.join(0);
  CHECK(s.last<WelcomeMsg>(MessageType::Welcome).assignedSlots == 0);
  CHECK(s.count(MessageType::Snapshot) == 1);
  rig.tick();
  CHECK(s.count(MessageType::AuthInputs) == 1);
  // A spectator's inputs are someone else's slots.
  s.unreliable(inputFor(0b01, rig.server->nextTick(), 1));
  rig.pump();
  CHECK(rig.server->stats().rejectedInputs == 1);

  s.reliable(JoinMatchMsg{2});
  rig.pump();
  CHECK(rig.server->present() == 0b111);
  CHECK(rig.server->peerInfo(s.t_->self()).value_or(ServerPeerInfo{}).slots == 0b110);
  s.reliable(JoinMatchMsg{0});
  rig.pump();
  CHECK(rig.server->present() == 0b001);
  CHECK(p.count(MessageType::PlayerLeft) == 2);
}

TEST_CASE("bots fill slots with scripted inputs every tick", "[net][server]") {
  Rig rig;
  REQUIRE(rig.server->setBot(PlayerId{4}, true));
  const Raw& a = rig.join(1);
  CHECK(a.last<WelcomeMsg>(MessageType::Welcome).assignedSlots == 0b1);
  CHECK(!rig.server->setBot(PlayerId{0}, true));  // owned
  for (int i = 0; i < 200; ++i)
    rig.tick();
  CHECK(sim::maskHas(rig.server->world().rules.present, PlayerId{4}));
  CHECK(rig.server->stats().substitutedInputs == 200);  // a never sent anything; bots never count
}

TEST_CASE("arrival margins and checksums ride in AuthInputs", "[net][server]") {
  ServerConfig cfg;
  cfg.checksumEveryTicks = 2;
  Rig rig(cfg);
  Raw& a = rig.join(1);
  rig.tick();
  CHECK(!a.last<AuthInputsMsg>(MessageType::AuthInputs).arrivalMargin);  // no input yet
  const Tick next = rig.server->nextTick();
  a.unreliable(inputFor(0b1, next.plus(3), 1));  // three ticks early
  rig.pump();
  rig.tick();
  const auto auth = a.last<AuthInputsMsg>(MessageType::AuthInputs);
  CHECK(auth.arrivalMargin == int8_t{3});
  REQUIRE(auth.checksumTick.has_value());  // world.tick is 2
  const Tick sumTick = auth.checksumTick.value_or(Tick{1});
  CHECK(sumTick.value % 2 == 0);
  CHECK(sumTick == rig.server->world().tick);
  CHECK(auth.checksum == sim::checksum(rig.server->world()));
}

TEST_CASE("a resync request is answered with a snapshot, rate-limited", "[net][server]") {
  Rig rig;
  Raw& a = rig.join(1);
  a.reliable(ResyncRequestMsg{rig.server->nextTick(), 1});
  rig.pump();
  CHECK(a.count(MessageType::Snapshot) == 2);
  a.reliable(ResyncRequestMsg{rig.server->nextTick(), 1});
  rig.pump();
  CHECK(a.count(MessageType::Snapshot) == 2);  // within minResyncIntervalNs
  CHECK(rig.server->stats().resyncsServed == 1);
}

TEST_CASE("Ping is answered with the server tick and the hold time", "[net][server]") {
  Rig rig;
  Raw& a = rig.client();
  a.unreliable(PingMsg{12345, Tick{0}});
  rig.pump();
  const auto pong = a.last<PongMsg>(MessageType::Pong);
  CHECK(pong.echoSendNs == 12345);
  CHECK(pong.tick == rig.server->nextTick());
}

TEST_CASE("a silent peer times out and its slots are reserved", "[net][server]") {
  ServerConfig cfg;
  cfg.peerTimeoutNs = 100 * kMs;
  Rig rig(cfg);
  const Raw& a = rig.join(1, "quiet");
  for (int i = 0; i < 101; ++i)
    rig.pump();
  CHECK(a.closed);
  CHECK(rig.server->present() == 0b1);
  CHECK(!rig.server->ownerOf(PlayerId{0}).isValid());
}

TEST_CASE("restartMatch announces MatchConfig now and snapshots the new world at its tick",
          "[net][server][restart]") {
  ServerConfig cfg;
  cfg.match.seed = 0x1111;
  Rig rig(cfg);
  REQUIRE(rig.server->setBot(PlayerId{3}, true));
  Raw& a = rig.join(1);
  for (int i = 0; i < 30; ++i)
    rig.tick();
  const Tick start = rig.server->nextTick().plus(5);
  rig.server->restartMatch(0x2222, start, 99);
  rig.pump();
  REQUIRE(a.count(MessageType::MatchConfig) == 1);
  const auto mc = a.last<MatchConfigMsg>(MessageType::MatchConfig);
  CHECK(mc.config.seed == 0x2222);
  CHECK(mc.config.players == 0b1001);
  CHECK(mc.startTick == start);
  CHECK(rig.server->pendingRestart() == start);

  // A joiner before the restart hears about it after its snapshot.
  Raw& b = rig.join(1);
  REQUIRE(b.count(MessageType::MatchConfig) == 1);
  CHECK(b.inbox.back().type != MessageType::Welcome);
  CHECK(b.last<MatchConfigMsg>(MessageType::MatchConfig).startTick == start);

  const size_t snapsA = a.count(MessageType::Snapshot);
  while (rig.server->nextTick() < start)
    rig.tick();
  CHECK(a.count(MessageType::Snapshot) == snapsA);
  CHECK(rig.server->matchIndex() == 0);
  rig.tick();  // assembles `start`: the restart takes effect first
  CHECK(rig.server->matchIndex() == 1);
  CHECK(!rig.server->pendingRestart());
  CHECK(rig.server->stats().matchRestarts == 1);
  CHECK(rig.server->matchConfig().seed == 0x2222);
  REQUIRE(a.count(MessageType::Snapshot) == snapsA + 1);
  SnapshotMsg snap;
  for (const Received& r : std::views::reverse(a.inbox)) {
    if (r.type != MessageType::Snapshot) continue;
    REQUIRE(decode(r.bytes, snap));
    break;
  }
  CHECK(snap.world.tick == start);
  CHECK(snap.config.seed == 0x2222);
  CHECK(snap.config.players == 0b1011);  // the roster at the restart, b included
  // The snapshot is the fresh match: initialWorld at `start`, before stepping it.
  sim::WorldState expect = sim::initialWorld(snap.config);
  expect.tick = start;
  CHECK(sim::checksum(snap.world) == sim::checksum(expect));
  CHECK(rig.server->world().tick == start.plus(1));
  CHECK(rig.server->world().rules.round == 1);
  bool restarted = false;
  for (const ServerEvent& e : rig.server->events())
    restarted = restarted || e.kind == ServerEventKind::MatchRestarted;
  CHECK(restarted);
}

TEST_CASE("shutdown disconnects every peer and refuses new ones", "[net][server][shutdown]") {
  Rig rig;
  const Raw& a = rig.join(1);
  rig.server->shutdown();
  CHECK(rig.server->shuttingDown());
  rig.pump();
  REQUIRE(a.count(MessageType::Disconnect) == 1);
  CHECK(a.last<DisconnectMsg>(MessageType::Disconnect).reason ==
        DisconnectReason::ServerShuttingDown);
  Raw& late = rig.client();
  late.reliable(Rig::hello(1));
  rig.pump();
  rig.pump();
  CHECK(late.last<RejectMsg>(MessageType::Reject).reason == RejectReason::ServerShuttingDown);
  rig.server->closeAll();
  rig.pump();
  rig.pump();
  CHECK(a.closed);
  CHECK(rig.server->peers().empty());
  CHECK(rig.server->present() == 0);
}

TEST_CASE("maxPlayers caps the slots open to peers and bots", "[net][server]") {
  ServerConfig cfg;
  cfg.maxPlayers = 3;
  Rig rig(cfg);
  CHECK(!rig.server->setBot(PlayerId{3}, true));
  REQUIRE(rig.server->setBot(PlayerId{0}, true));
  const Raw& a = rig.join(2);
  CHECK(a.last<WelcomeMsg>(MessageType::Welcome).assignedSlots == 0b110);
  Raw& b = rig.client();
  b.reliable(Rig::hello(1));
  rig.pump();
  rig.pump();
  CHECK(b.last<RejectMsg>(MessageType::Reject).reason == RejectReason::MatchFull);
}

TEST_CASE("queueForBotSlots: a joiner only bots' slots fit waits as a spectator (v0.44)",
          "[net][server][queue]") {
  ServerConfig cfg;
  cfg.maxPlayers = 3;
  cfg.queueForBotSlots = true;
  Rig rig(cfg);
  REQUIRE(rig.server->setBot(PlayerId{1}, true));
  REQUIRE(rig.server->setBot(PlayerId{2}, true));
  const Raw& a = rig.join(1);
  CHECK(a.last<WelcomeMsg>(MessageType::Welcome).assignedSlots == 0b001);

  // Full of a human and bots: welcomed with no slot, waiting for one.
  rig.server->clearEvents();
  const Raw& b = rig.join(1);
  const PeerId bp = b.t_->self();
  CHECK(b.last<WelcomeMsg>(MessageType::Welcome).assignedSlots == 0);
  CHECK(b.count(MessageType::Reject) == 0);
  CHECK(rig.server->oldestQueued() == bp);
  CHECK(rig.server->queuedSlots(bp) == 1);
  CHECK(std::ranges::any_of(rig.server->events(), [](const ServerEvent& e) {
    return e.kind == ServerEventKind::PeerQueued && e.reason == 1;
  }));
  // More than the bots could ever give up is still MatchFull.
  Raw& c = rig.client();
  c.reliable(Rig::hello(3));
  rig.pump();
  rig.pump();
  CHECK(c.last<RejectMsg>(MessageType::Reject).reason == RejectReason::MatchFull);
  // A second waiter queues behind the first.
  const Raw& d = rig.join(1);
  const PeerId dp = d.t_->self();
  CHECK(rig.server->queuedSlots(dp) == 1);
  CHECK(rig.server->oldestQueued() == bp);

  // Nothing is granted while no slot is free.
  CHECK_FALSE(rig.server->grantQueued(bp));
  // The host frees a bot's slot: the oldest waiter gets it, as a join.
  REQUIRE(rig.server->setBot(PlayerId{2}, false));
  CHECK_FALSE(rig.server->grantQueued(PeerId{99}));
  REQUIRE(rig.server->grantQueued(bp));
  rig.pump();
  CHECK(rig.server->peerInfo(bp).value_or(ServerPeerInfo{}).slots == 0b100);
  CHECK(b.last<PlayerJoinedMsg>(MessageType::PlayerJoined).yours);
  CHECK(b.last<PlayerJoinedMsg>(MessageType::PlayerJoined).slot == PlayerId{2});
  CHECK(rig.server->queuedSlots(bp) == 0);
  CHECK(rig.server->oldestQueued() == dp);
  CHECK(rig.server->present() == 0b111);

  // A waiter that leaves stops waiting; JoinMatch can wait too.
  rig.net.cut(d.t_->self());
  for (int i = 0; i < 3; ++i)
    rig.pump();
  CHECK(!rig.server->oldestQueued().isValid());
  Raw& e = rig.join(0);
  e.reliable(JoinMatchMsg{1});
  rig.pump();
  CHECK(rig.server->queuedSlots(e.t_->self()) == 1);
  e.reliable(JoinMatchMsg{0});  // a new request replaces the waiting one
  rig.pump();
  CHECK(rig.server->queuedSlots(e.t_->self()) == 0);
}

TEST_CASE("without queueForBotSlots a full server still rejects", "[net][server][queue]") {
  ServerConfig cfg;
  cfg.maxPlayers = 2;
  Rig rig(cfg);
  REQUIRE(rig.server->setBot(PlayerId{1}, true));
  static_cast<void>(rig.join(1));
  Raw& b = rig.client();
  b.reliable(Rig::hello(1));
  rig.pump();
  rig.pump();
  CHECK(b.last<RejectMsg>(MessageType::Reject).reason == RejectReason::MatchFull);
  CHECK(!rig.server->oldestQueued().isValid());
}

TEST_CASE("unset, the bot source is the scripted generator over botSeed", "[net][server][bots]") {
  // A server with the default and one whose source replays the scripted
  // generator by hand step identical frames: the default is unchanged.
  ServerConfig plain;
  plain.match.seed = 0x51;
  plain.botSeed = 0x5EED;
  Rig a(plain);
  ServerConfig custom = plain;
  Rng rng = Rng::seeded(plain.botSeed);
  uint32_t calls = 0;
  custom.botInputs = [&](const sim::WorldState& w, const sim::MatchContent& c, sim::InputFrame& f) {
    ++calls;
    CHECK(f.tick == w.tick);
    CHECK(f.present == 0b10010);  // the bot slots, nothing else
    for (uint8_t s = 0; s < sim::kMaxPlayers; ++s) {
      CHECK(f.inputs[s] == sim::PlayerInput{});
      if (sim::maskHas(f.present, PlayerId{s}))
        f.inputs[s] = sim::scriptedInput(rng, w, c, PlayerId{s}, plain.botConfig);
    }
  };
  Rig b(custom);
  for (Rig* r : {&a, &b}) {
    REQUIRE(r->server->setBot(PlayerId{1}, true));
    REQUIRE(r->server->setBot(PlayerId{4}, true));
  }
  for (int i = 0; i < 300; ++i) {
    a.tick();
    b.tick();
  }
  CHECK(calls == 300);
  CHECK(sim::checksum(a.server->world()) == sim::checksum(b.server->world()));
  for (uint32_t t = 200; t < 300; ++t) {
    const sim::InputFrame* fa = a.server->frames().get(Tick{t});
    const sim::InputFrame* fb = b.server->frames().get(Tick{t});
    if (fa == nullptr || fb == nullptr) {
      FAIL("frame " << t << " missing");
      return;
    }
    CHECK(*fa == *fb);
  }
}

TEST_CASE("a custom bot source writes the bot slots of the authoritative frames",
          "[net][server][bots]") {
  ServerConfig cfg;
  cfg.match.seed = 0x1111;
  cfg.botSeed = 7;
  std::vector<std::pair<Tick, uint32_t>> seen;  // (tick, matchIndex) per call
  NetServer* server = nullptr;
  cfg.botInputs = [&](const sim::WorldState& w, const sim::MatchContent& /*c*/,
                      sim::InputFrame& f) {
    REQUIRE(server != nullptr);
    seen.emplace_back(w.tick, server->matchIndex());
    if (server->matchIndex() == 1) {
      CHECK(server->botSeed() == 99);  // restartMatch's bot seed, before the first act
    } else {
      CHECK(server->botSeed() == 7);
    }
    f.inputs[3].moveDir = static_cast<uint8_t>(w.tick.value & 0xFFu);
    f.inputs[3].moveMag = 200;
    f.inputs[3].aimDir = 17;
    f.inputs[5].moveMag = 255;  // not a bot slot: never read
  };
  Rig rig(cfg);
  server = rig.server.get();
  rig.tick();  // no bot: the source is not called
  CHECK(seen.empty());
  REQUIRE(rig.server->setBot(PlayerId{3}, true));
  const Raw& a = rig.join(1);
  for (int i = 0; i < 20; ++i)
    rig.tick();
  REQUIRE(seen.size() == 20);
  for (const auto& [tick, match] : seen) {
    const sim::InputFrame* f = rig.server->frames().get(tick);
    if (f == nullptr) {
      FAIL("frame " << tick.value << " missing");
      return;
    }
    CHECK(f->inputs[3].moveDir == static_cast<uint8_t>(tick.value & 0xFFu));
    CHECK(f->inputs[3].moveMag == 200);
    CHECK(f->inputs[3].aimDir == 17);
    CHECK(f->inputs[0] == sim::PlayerInput{});  // the silent peer's slot
    CHECK(!sim::maskHas(f->present, PlayerId{5}));
    CHECK(match == 0);
  }
  const auto auth = a.last<AuthInputsMsg>(MessageType::AuthInputs);
  REQUIRE(auth.count > 0);
  CHECK(auth.frames[auth.count - 1].inputs[3].moveMag == 200);  // and it reaches the clients
  CHECK(rig.server->stats().substitutedInputs == 20);           // the silent peer only

  // A restart: the source sees the fresh world on the restart tick itself.
  const Tick start = rig.server->nextTick().plus(3);
  rig.server->restartMatch(0x2222, start, 99);
  while (rig.server->nextTick() <= start)
    rig.tick();
  REQUIRE(seen.size() >= 2);
  CHECK(seen.back().first == start);
  CHECK(seen.back().second == 1);
  CHECK(seen[seen.size() - 2].second == 0);
}

TEST_CASE("humanLeadTicks is the median lead of the peers that hold slots", "[net][server][bots]") {
  ServerConfig cfg;
  cfg.maxProtocolErrors = 100000;  // the spectator's foreign inputs below
  Rig rig(cfg);
  CHECK(rig.server->humanLeadTicks() == 0);  // nobody
  Raw& a = rig.join(1);
  Raw& b = rig.join(1);
  Raw& c = rig.join(1);
  Raw& spectator = rig.join(0);
  for (int i = 0; i < 5; ++i)
    rig.tick();
  CHECK(rig.server->humanLeadTicks() == 0);  // no input yet

  // Zero latency: a client that acknowledges the newest frame has rtt 0, so
  // its lead is its margin. Every tick each sends its input for nextTick +
  // margin, acknowledging the frame ackLag ticks older than the newest.
  const auto send = [&](Raw& r, sim::PlayerMask slot, int32_t margin, int32_t ackLag) {
    InputMsg m = inputFor(slot, rig.server->nextTick().plus(margin), 1);
    m.lastAuthTick = rig.server->nextTick().plus(-1 - ackLag);
    r.unreliable(m);
  };
  for (int i = 0; i < 120; ++i) {
    send(a, 0b001, 2, 0);
    send(b, 0b010, 9, 0);
    send(c, 0b100, 3, 0);
    send(spectator, 0b001, 30, 0);  // owns nothing: ignored
    rig.pump();
    rig.tick();
  }
  const auto info = rig.server->peerInfo(rig.server->ownerOf(PlayerId{1}));
  if (!info) {
    FAIL("no peer for slot 1");
    return;
  }
  CHECK(info->leadEmaQ8 == 9 * 256);
  CHECK(info->marginEmaQ8 == 9 * 256);
  CHECK(rig.server->humanLeadTicks() == 3);  // the median of 2, 9, 3

  // Half the acknowledgement's round trip counts: margin 2 with frames that
  // took 6 ticks there and back is a lead of 5.
  for (int i = 0; i < 400; ++i) {
    send(a, 0b001, 2, 6);
    send(b, 0b010, 2, 6);
    send(c, 0b100, 2, 6);
    rig.pump();
    rig.tick();
  }
  CHECK(rig.server->humanLeadTicks() == 5);

  // Peers that leave stop counting; a late one (negative lead) clamps to 0.
  b.reliable(DisconnectMsg{DisconnectReason::Quit});
  c.reliable(DisconnectMsg{DisconnectReason::Quit});
  rig.pump();
  for (int i = 0; i < 400; ++i) {
    send(a, 0b001, -4, 0);
    rig.pump();
    rig.tick();
  }
  CHECK(rig.server->humanLeadTicks() == 0);
}

// --- the room (v0.42, protocol 6) ----------------------------------------------------

namespace {

HelloMsg named(uint8_t slots, std::string_view name, const std::string& token = {}) {
  HelloMsg h = Rig::hello(slots, token);
  h.nickname = Nickname::from(name);
  return h;
}

// A pointer, not a reference: GCC's -Wdangling-reference flags a reference
// returned from a function taking temporaries.
Raw* joinNamed(Rig& rig, uint8_t slots, std::string_view name, const std::string& token = {}) {
  Raw& r = rig.client();
  r.reliable(named(slots, name, token));
  rig.pump();
  REQUIRE(r.count(MessageType::Welcome) == 1);
  return &r;
}

}  // namespace

TEST_CASE("each slot keeps its peer's nickname; a reconnect keeps it, a release forgets it",
          "[net][server][room]") {
  ServerConfig cfg;
  cfg.reconnectGraceTicks = 30;
  Rig rig(cfg);
  const Raw& a = *joinNamed(rig, 2, "Bonobo", "tok");
  joinNamed(rig, 1, "Gibbon");
  CHECK(rig.server->nickname(PlayerId{0}).view() == "Bonobo");
  CHECK(rig.server->nickname(PlayerId{1}).view() == "Bonobo");
  CHECK(rig.server->nickname(PlayerId{2}).view() == "Gibbon");
  CHECK(rig.server->nickname(PlayerId{3}).empty());
  CHECK(rig.server->humanSlots() == 0b111);

  // Dropped and back with the same token (and another name): the name stays.
  rig.net.cut(a.t_->self());
  rig.pump();
  CHECK(rig.server->nickname(PlayerId{0}).view() == "Bonobo");
  const Raw& again = *joinNamed(rig, 2, "Imposter", "tok");
  CHECK(again.last<WelcomeMsg>(MessageType::Welcome).assignedSlots == 0b011);
  CHECK(rig.server->nickname(PlayerId{1}).view() == "Bonobo");

  // A deliberate quit releases the slots and their names.
  rig.raws.back()->reliable(DisconnectMsg{DisconnectReason::Quit});
  rig.pump();
  CHECK(rig.server->present() == 0b100);
  CHECK(rig.server->nickname(PlayerId{0}).empty());
}

TEST_CASE("releaseReservations frees a dropped peer's slots without a tick",
          "[net][server][room]") {
  Rig rig;
  const Raw& a = rig.join(1, "tok");
  const Tick frozen = rig.server->nextTick();
  rig.net.cut(a.t_->self());
  rig.pump();
  CHECK(rig.server->present() == 0b1);  // reserved
  rig.server->clearEvents();
  rig.server->releaseReservations();
  CHECK(rig.server->present() == 0);
  CHECK(rig.server->nextTick() == frozen);
  REQUIRE(rig.server->events().size() == 1);
  CHECK(rig.server->events()[0].kind == ServerEventKind::SlotsReleased);
  // The token no longer reclaims anything: a fresh join.
  const Raw& b = rig.join(1, "tok");
  CHECK(b.last<WelcomeMsg>(MessageType::Welcome).assignedSlots == 0b1);
  CHECK(rig.server->stats().reconnects == 0);
}

TEST_CASE("RoomSettings and Start become events; RoomState reaches everyone and every joiner",
          "[net][server][room]") {
  Rig rig;
  Raw& a = *joinNamed(rig, 1, "Host");
  CHECK(a.count(MessageType::RoomState) == 0);  // no room set: the M3 flow sends none

  RoomStateMsg state;
  state.host = PlayerId{0};
  state.humans = 0b1;
  state.names[0] = Nickname::from("Host");
  rig.server->setRoomState(state);
  rig.pump();
  CHECK(a.count(MessageType::RoomState) == 1);
  CHECK(a.last<RoomStateMsg>(MessageType::RoomState) == state);

  // A joiner gets the room right after its Welcome and Snapshot.
  const Raw& b = rig.join(1);
  REQUIRE(b.inbox.size() >= 3);
  CHECK(b.inbox[0].type == MessageType::Welcome);
  CHECK(b.inbox[1].type == MessageType::Snapshot);
  CHECK(b.inbox[2].type == MessageType::RoomState);

  rig.server->clearEvents();
  const RoomSettings wanted{.arena = 0, .mode = 0, .loadout = 0, .powerUps = 3, .bots = 2};
  a.reliable(RoomSettingsMsg{wanted});
  a.reliable(StartMsg{});
  rig.pump();
  std::vector<ServerEvent> room;
  for (const ServerEvent& e : rig.server->events())
    if (e.kind == ServerEventKind::RoomSettingsRequested ||
        e.kind == ServerEventKind::StartRequested)
      room.push_back(e);
  REQUIRE(room.size() == 2);
  CHECK(room[0].kind == ServerEventKind::RoomSettingsRequested);
  CHECK(room[0].settings == wanted);
  CHECK(room[0].peer == a.t_->self());
  CHECK(room[0].slots == 0b1);
  CHECK(room[1].kind == ServerEventKind::StartRequested);
  CHECK(rig.server->stats().protocolErrors == 0);
}

TEST_CASE("room messages are rate-limited and refused before the handshake",
          "[net][server][room]") {
  ServerConfig cfg;
  cfg.roomMsgsPerSecond = 5;
  Rig rig(cfg);
  Raw& a = rig.join(1);
  rig.server->clearEvents();
  for (int i = 0; i < 20; ++i)
    a.reliable(StartMsg{});
  rig.pump();
  CHECK(rig.server->events().size() == 5);
  CHECK(rig.server->stats().roomRateLimited == 15);

  Raw& early = rig.client();
  early.reliable(StartMsg{});
  rig.pump();
  CHECK(rig.server->stats().protocolErrors == 1);
}
