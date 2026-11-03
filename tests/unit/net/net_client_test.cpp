// NetClient against a hand-driven server endpoint over a zero-latency
// loopback (architecture §6.1, §6.2, §6.6): the sync handshake, prediction,
// rollback from the saved-state ring, input redundancy, spectating.
#include "kfm/net/net_client.hpp"

#include "kfm/net/loopback.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/step.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

using namespace kfm;
using namespace kfm::net;

namespace {

constexpr uint64_t kMs = 1'000'000;
constexpr sim::MatchConfig kConfig{0x5EED, 0b11};

const sim::PlayerInput kRun{64, 255, 64, 0, 0};

// The server side, written by hand: whatever the test sends.
struct FakeServer {
  FakeServer() : net(3) {
    clientT = &net.connectClient(0);
    client = std::make_unique<NetClient>(sim::defaultContent());
  }

  template <class M>
  void reliable(const M& m) {
    std::vector<std::byte> buf(kMaxReliableBytes);
    const size_t n = encode(m, buf);
    REQUIRE(n > 0);
    net.server().sendReliable(clientT->self(), {buf.data(), n});
  }
  template <class M>
  void unreliable(const M& m) {
    std::vector<std::byte> buf(kMaxUnreliableBytes);
    const size_t n = encode(m, buf);
    REQUIRE(n > 0);
    net.server().sendUnreliable(clientT->self(), {buf.data(), n});
  }

  // 1 ms: the server endpoint polls (and records inputs), then the client
  // pumps and reconciles.
  void pump() {
    now += kMs;
    ConnectionEvents ev;
    net.server().poll(now, ev);
    Packet p;
    while (net.server().recv(p)) {
      const auto type = peekType(p.data);
      REQUIRE(type.has_value());
      if (*type == MessageType::Input) {
        InputMsg m;
        REQUIRE(decode(p.data, m));
        inputs.push_back(m);
      } else if (*type == MessageType::Ping) {
        PingMsg ping;
        REQUIRE(decode(p.data, ping));
        pings.push_back(ping);
      } else if (*type == MessageType::Hello) {
        ++hellos;
      } else if (*type == MessageType::ResyncRequest) {
        ++resyncRequests;
      }
    }
    client->beginFrame();
    client->pump(now);
    client->reconcile();
  }

  // Welcome + snapshot of `world` + three pongs: the client starts running.
  void start(sim::PlayerMask slots, const sim::WorldState& world, Tick serverTick) {
    client->connect(*clientT, now);
    pump();  // the client sees Connected and sends Hello + Ping
    pump();
    REQUIRE(hellos == 1);
    reliable(WelcomeMsg{slots, kConfig, serverTick});
    SnapshotMsg snap;
    snap.config = kConfig;
    snap.world = world;
    reliable(snap);
    for (int i = 0; i < 3; ++i)
      unreliable(PongMsg{pings.at(0).sendNs, serverTick, 0});
    pump();
    REQUIRE(client->running());
  }

  // An AuthInputs message with frames[from..to] of `frames`.
  void auth(const std::vector<sim::InputFrame>& frames, uint32_t from, uint32_t to) {
    AuthInputsMsg m;
    m.startTick = Tick{from};
    m.count = static_cast<uint8_t>(to - from + 1);
    for (uint32_t t = from; t <= to; ++t)
      m.frames[t - from] = frames.at(t);
    unreliable(m);
  }

  LoopbackNetwork net;
  LoopbackTransport* clientT;
  std::unique_ptr<NetClient> client;
  uint64_t now{0};
  std::vector<InputMsg> inputs;
  std::vector<PingMsg> pings;
  int hellos{0};
  int resyncRequests{0};
};

sim::InputFrame frame(uint32_t t, sim::PlayerMask present, sim::PlayerInput s0 = {},
                      sim::PlayerInput s1 = {}) {
  sim::InputFrame f;
  f.tick = Tick{t};
  f.present = present;
  f.inputs[0] = s0;
  f.inputs[1] = s1;
  return f;
}

}  // namespace

TEST_CASE("the client syncs on Welcome, snapshot and RTT, then leads the server", "[net][client]") {
  FakeServer fs;
  const sim::WorldState w0 = sim::initialWorld(kConfig);
  fs.start(0b01, w0, Tick{0});
  CHECK(fs.client->localSlots() == 0b01);
  // RTT ≈ 0 and the server at tick 0: the client starts at the target lead.
  CHECK(fs.client->localTick().value == static_cast<uint32_t>(targetLeadTicks(fs.client->rtt())));
  CHECK(fs.client->localTick() <= Tick{2});
  CHECK(fs.client->netStats().snapshotsLoaded == 1);
  CHECK(fs.client->frameReport().snapshotLoaded);
  CHECK(fs.client->confirmedTick() == std::nullopt);
}

TEST_CASE("a misprediction restores the saved state and re-simulates with the corrected inputs",
          "[net][client]") {
  FakeServer fs;
  sim::WorldState ref = sim::initialWorld(kConfig);
  fs.start(0b01, ref, Tick{0});

  // Ten local ticks, local slot idle; the remote slot is predicted idle.
  for (int i = 0; i < 10; ++i)
    fs.client->tick(std::vector<sim::PlayerInput>{sim::PlayerInput{}});
  const Tick l = fs.client->localTick();
  REQUIRE(l >= Tick{10});

  // The server's frames 0..7: the remote player started running at tick 3.
  std::vector<sim::InputFrame> frames;
  for (uint32_t t = 0; t <= 7; ++t)
    frames.push_back(frame(t, 0b11, {}, t >= 3 ? kRun : sim::PlayerInput{}));
  fs.auth(frames, 0, 7);
  fs.pump();

  const FrameReport& rep = fs.client->frameReport();
  CHECK(rep.rolledBack);
  CHECK(rep.rollbacks == 1);
  CHECK(rep.maxDepth == static_cast<uint32_t>(l.minus(Tick{3})));  // re-simulated 3..L-1
  CHECK(fs.client->confirmedTick() == Tick{7});
  CHECK(fs.client->netStats().mispredictedRemote == 5);
  CHECK(fs.client->netStats().mispredictedLocal == 0);
  CHECK(fs.client->localTick() == l);

  // Reference: the authoritative frames, then the remote input repeated.
  sim::EventSink sink;
  for (uint32_t t = 0; t < l.value; ++t) {
    const sim::InputFrame f = t <= 7 ? frames[t] : frame(t, 0b11, {}, kRun);
    sink.clear();
    sim::step(ref, f, sim::defaultContent(), sink);
  }
  CHECK(sim::checksum(fs.client->world()) == sim::checksum(ref));
  CHECK(fs.client->prevWorld().tick == l.plus(-1));

  // The same frames again (redundancy) change nothing.
  fs.auth(frames, 4, 7);
  fs.pump();
  CHECK(!fs.client->frameReport().rolledBack);
  CHECK(fs.client->netStats().rollbacks == 1);
}

TEST_CASE("a correct prediction needs no rollback", "[net][client]") {
  FakeServer fs;
  fs.start(0b01, sim::initialWorld(kConfig), Tick{0});
  // The ticks before the first local input were stepped idle at sync.
  const uint32_t first = fs.client->localTick().value;
  for (int i = 0; i < 6; ++i)
    fs.client->tick(std::vector<sim::PlayerInput>{kRun});
  std::vector<sim::InputFrame> frames;
  for (uint32_t t = 0; t <= 5; ++t)
    frames.push_back(frame(t, 0b11, t < first ? sim::PlayerInput{} : kRun, {}));
  fs.auth(frames, 0, 5);
  fs.pump();
  CHECK(!fs.client->frameReport().rolledBack);
  CHECK(fs.client->netStats().rollbacks == 0);
  CHECK(fs.client->confirmedTick() == Tick{5});
}

TEST_CASE("inputs are sent with redundancy back to the newest assembled tick", "[net][client]") {
  FakeServer fs;
  fs.start(0b01, sim::initialWorld(kConfig), Tick{0});
  const uint32_t first = fs.client->localTick().value;
  REQUIRE(first < 5);
  for (int i = 0; i < 20; ++i)
    fs.client->tick(std::vector<sim::PlayerInput>{kRun});
  fs.pump();
  REQUIRE(fs.inputs.size() == 20);
  const InputMsg& last = fs.inputs.back();
  CHECK(last.endTick() == fs.client->localTick());
  CHECK(last.count == kMaxInputRedundancy);  // nothing acknowledged yet
  CHECK(last.inputs[0][last.count - 1] == kRun);

  // Once the server has assembled up to tick 15, only 16..L are resent.
  std::vector<sim::InputFrame> frames;
  for (uint32_t t = 0; t <= 15; ++t)
    frames.push_back(frame(t, 0b11, t == 5 || t < first ? sim::PlayerInput{} : kRun, {}));
  fs.auth(frames, 0, 15);
  fs.pump();
  fs.client->tick(std::vector<sim::PlayerInput>{kRun});
  fs.pump();
  CHECK(fs.inputs.back().startTick == Tick{16});
  CHECK(fs.inputs.back().lastAuthTick == Tick{15});
  // Tick 5 was sent as kRun and the server used something else (it arrived
  // late, say): the client counts the substitution and rolled back for it.
  CHECK(fs.client->netStats().substitutionsSeen == 1);
  CHECK(fs.client->netStats().mispredictedLocal == 1);
}

TEST_CASE("a server checksum that disagrees triggers a resync request", "[net][client]") {
  FakeServer fs;
  fs.start(0b01, sim::initialWorld(kConfig), Tick{0});
  for (int i = 0; i < 5; ++i)
    fs.client->tick(std::vector<sim::PlayerInput>{sim::PlayerInput{}});
  std::vector<sim::InputFrame> frames;
  for (uint32_t t = 0; t <= 3; ++t)
    frames.push_back(frame(t, 0b11));
  AuthInputsMsg m;
  m.startTick = Tick{0};
  m.count = 4;
  for (uint32_t t = 0; t <= 3; ++t)
    m.frames[t] = frames[t];
  m.checksumTick = Tick{4};
  m.checksum = 0xBAD;
  fs.unreliable(m);
  fs.pump();
  fs.pump();
  CHECK(fs.client->netStats().checksumMismatches == 1);
  CHECK(fs.client->netStats().resyncRequests == 1);
  CHECK(fs.resyncRequests == 1);

  // The snapshot that answers it is a hard load; prediction continues at L.
  const Tick l = fs.client->localTick();
  sim::WorldState w = sim::initialWorld(kConfig);
  sim::EventSink sink;
  for (uint32_t t = 0; t <= 3; ++t)
    sim::step(w, frames[t], sim::defaultContent(), sink);
  SnapshotMsg snap;
  snap.config = kConfig;
  snap.world = w;
  fs.reliable(snap);
  fs.pump();
  CHECK(fs.client->frameReport().snapshotLoaded);
  CHECK(fs.client->localTick() == l);
  CHECK(fs.client->netStats().snapshotsLoaded == 2);
}

TEST_CASE("a spectator steps only authoritative frames", "[net][client]") {
  FakeServer fs;
  fs.start(0, sim::initialWorld(kConfig), Tick{0});
  CHECK(fs.client->spectator());
  CHECK(fs.client->localTick() == Tick{0});
  fs.client->tick();
  CHECK(fs.client->localTick() == Tick{0});  // no frame, no step
  std::vector<sim::InputFrame> frames;
  for (uint32_t t = 0; t <= 9; ++t)
    frames.push_back(frame(t, 0b11, kRun, kRun));
  fs.auth(frames, 0, 9);
  fs.pump();
  uint32_t due = 0;
  for (int i = 0; i < 200; ++i) {
    fs.pump();
    const uint32_t n = fs.client->ticksDue(fs.now);
    for (uint32_t k = 0; k < n; ++k)
      fs.client->tick();
    due += n;
  }
  CHECK(due == 10);  // all ten frames, never past the newest
  CHECK(fs.client->localTick() == Tick{10});
  CHECK(fs.client->netStats().rollbacks == 0);
  CHECK(fs.inputs.empty());
}

TEST_CASE("NetClient is large: allocate it on the heap", "[net][client]") {
  INFO("sizeof(NetClient) = " << sizeof(NetClient)
                              << ", sizeof(WorldState) = " << sizeof(sim::WorldState));
  CHECK(sizeof(NetClient) > kRollbackWindow * sizeof(sim::WorldState));
  WARN("sizeof(NetClient) = " << sizeof(NetClient)
                              << " B, sizeof(WorldState) = " << sizeof(sim::WorldState) << " B");
}
