// NetServer driven by hand-written messages over a zero-latency loopback
// (architecture §6.6, §6.7): handshake checks, input validation and rate
// limits, reconnect reservations, spectators, resync, ping.
#include "kfm/net/net_server.hpp"

#include "kfm/net/loopback.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/step.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <string>
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
