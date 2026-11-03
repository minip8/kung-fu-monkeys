// NetSession (apps/client network mode) against an in-process NetServer over
// the loopback emulator (architecture §6.1, §6.2, §6.9, §7, §11 M3): the
// connect/sync/running flow, local devices mapped onto server slots, inputs
// from a fake keyboard reaching the authoritative world, rollback smoothing,
// exactly-once events across rollbacks, reject and disconnect.
#include "core/net_session.hpp"

#include "kfm/core/rng.hpp"
#include "kfm/net/loopback.hpp"
#include "kfm/net/net_client.hpp"
#include "kfm/net/net_server.hpp"
#include "kfm/platform/input_event.hpp"
#include "kfm/platform/input_sampler.hpp"
#include "kfm/presentation/builder.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/scripted_inputs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace kfm;
using client::NetPhase;
using client::NetSession;
using client::NetSessionConfig;
using client::SlotOwner;
using platform::InputEvent;
using platform::InputEventKind;
using platform::Key;

namespace {

constexpr uint64_t kMs = 1'000'000;
constexpr uint64_t kSec = 1'000 * kMs;
constexpr uint64_t kFrameNs = 16 * kMs;  // host frames; the network runs in 1 ms steps

[[nodiscard]] net::LinkConditions link(uint64_t oneWayNs) {
  net::LinkConditions c = net::LinkConditions::uniform(oneWayNs);
  c.reliable.retransmitNs = std::max<uint64_t>(2 * oneWayNs, 20 * kMs);
  return c;
}

[[nodiscard]] platform::InputSampler lobby(std::initializer_list<platform::DeviceBinding> devices) {
  platform::InputSampler s;
  uint8_t slot = 0;
  for (const platform::DeviceBinding& d : devices)
    REQUIRE(s.bind(PlayerId{slot++}, d));
  return s;
}

// One NetServer, NetSessions and optional raw scripted NetClients over one
// LoopbackNetwork, stepped in 1 ms; sessions run a host frame every kFrameNs.
struct Rig {
  explicit Rig(net::ServerConfig config = {}, uint64_t seed = 11) : net(seed) {
    server =
        std::make_unique<net::NetServer>(net.server(), sim::defaultContent(), std::move(config));
  }

  struct Session {
    std::unique_ptr<NetSession> s;
    net::LoopbackTransport* transport{nullptr};
    std::unique_ptr<presentation::RenderState> rs = std::make_unique<presentation::RenderState>();
    uint64_t nextFrameNs{0};
    std::function<void()> afterFrame;
  };
  // Scripted: sim::scriptedInput (throws, dashes, aim jitter). Wander: walks
  // in a new direction every 15 ticks and never presses a button, so it
  // mispredicts regularly but never hits anyone.
  enum class Human : uint8_t { Scripted, Wander };
  struct Remote {
    net::LoopbackTransport* transport{nullptr};
    std::unique_ptr<net::NetClient> client;
    Rng rng{};
    Human human{Human::Scripted};
  };

  Session& addSession(const platform::InputSampler& sampler, NetSessionConfig config = {},
                      uint64_t oneWayNs = 0) {
    Session& r = sessions.emplace_back();
    r.transport = &net.connectClient(now, link(oneWayNs), link(oneWayNs));
    if (config.serverLabel.empty()) config.serverLabel = "loopback";
    r.s = std::make_unique<NetSession>(config, sampler, r.transport, now);
    r.nextFrameNs = now;
    return r;
  }

  Remote& addRemote(uint64_t oneWayNs, uint64_t seed, Human human = Human::Scripted) {
    Remote& r = remotes.emplace_back();
    r.human = human;
    r.transport = &net.connectClient(now, link(oneWayNs), link(oneWayNs));
    r.client = std::make_unique<net::NetClient>(sim::defaultContent());
    r.rng = Rng::seeded(seed);
    r.client->connect(*r.transport, now);
    return r;
  }

  void step() {
    now += kMs;
    server->pump(now);
    while (now >= serverTickAt(serverTicks)) {
      server->tick(now);
      ++serverTicks;
      for (const sim::SimEvent& e : server->tickEvents())
        serverEvents.push_back(e);
    }
    server->clearEvents();
    for (Remote& r : remotes)
      stepRemote(r);
    for (Session& r : sessions) {
      if (now < r.nextFrameNs) continue;
      r.nextFrameNs = now + kFrameNs;
      r.s->advance(now);
      r.s->build(static_cast<float>(kFrameNs) * 1e-9f, *r.rs);
      if (r.afterFrame) r.afterFrame();
    }
  }

  void runFor(uint64_t ns) {
    const uint64_t end = now + ns;
    while (now < end)
      step();
  }

  template <class Pred>
  bool runUntil(Pred pred, uint64_t maxNs) {
    const uint64_t end = now + maxNs;
    while (now < end) {
      if (pred()) return true;
      step();
    }
    return pred();
  }

  void push(NetSession& s, Key k, bool down) const {
    InputEvent e{};
    e.kind = down ? InputEventKind::KeyDown : InputEventKind::KeyUp;
    e.key = k;
    e.timestampNs = now;
    s.push(e);
  }

  // The match is playing and `slot` has a monkey in the server's world.
  [[nodiscard]] bool playing(PlayerId slot) const {
    const sim::WorldState& w = server->world();
    return w.rules.phase == sim::MatchPhase::Playing && w.players.isAliveIndex(slot.value);
  }
  [[nodiscard]] double serverX(PlayerId slot) const {
    return server->world().players.items[slot.value].transform.pos.x.toDouble();
  }

  [[nodiscard]] static uint64_t serverTickAt(uint64_t k) { return k * kSec / sim::kTickHz; }

  void stepRemote(Remote& r) const {
    net::NetClient& c = *r.client;
    c.beginFrame();
    c.pump(now);
    c.reconcile();
    for (uint32_t n = c.ticksDue(now); n > 0; --n) {
      std::array<sim::PlayerInput, sim::kMaxPlayers> inputs{};
      size_t k = 0;
      for (uint8_t s = 0; s < sim::kMaxPlayers; ++s) {
        if (!sim::maskHas(c.localSlots(), PlayerId{s})) continue;
        if (r.human == Human::Wander) {
          const auto dir = static_cast<uint8_t>((c.localTick().value / 15) * 97);
          inputs[k++] = sim::PlayerInput{dir, 255, dir, 0, 0};
        } else {
          inputs[k++] = sim::scriptedInput(r.rng, c.world(), sim::defaultContent(), PlayerId{s});
        }
      }
      c.tick(std::span<const sim::PlayerInput>(inputs.data(), k));
    }
    static_cast<void>(c.drainEvents());
  }

  uint64_t now{0};
  net::LoopbackNetwork net;
  std::unique_ptr<net::NetServer> server;
  uint64_t serverTicks{0};
  std::vector<sim::SimEvent> serverEvents;
  std::deque<Session> sessions;
  std::deque<Remote> remotes;
};

[[nodiscard]] size_t countDashes(std::span<const sim::SimEvent> events, PlayerId slot) {
  return static_cast<size_t>(std::count_if(events.begin(), events.end(), [&](const auto& e) {
    return e.kind == sim::EventKind::PlayerDashed && e.player == slot;
  }));
}

[[nodiscard]] float offsetLength(const presentation::EntityTrack& t) {
  return std::hypot(t.visualOffset.x, t.visualOffset.y);
}

}  // namespace

TEST_CASE("a NetSession connects, syncs and runs; keyboard input moves its monkey on the server",
          "[client][net]") {
  Rig rig;
  REQUIRE(rig.server->setBot(PlayerId{0}, true));  // a second monkey so rounds start
  Rig::Session& r = rig.addSession(lobby({platform::DeviceBinding::keyboard(0)}), {}, 10 * kMs);
  NetSession& s = *r.s;
  CHECK(s.phase() == NetPhase::Connecting);
  CHECK(s.statusText().find("Connecting to loopback") != std::string::npos);
  CHECK(s.requestedSlots() == 1);

  REQUIRE(rig.runUntil([&] { return s.running(); }, 2 * kSec));
  CHECK(s.localSlots() == 0b10);  // the bot holds slot 0: the server assigned slot 1
  const PlayerId me = s.slotOfDevice(0);
  CHECK(me == PlayerId{1});
  CHECK(s.owner(me) == SlotOwner::Local);
  CHECK(s.statusText().find("Online") != std::string::npos);

  REQUIRE(rig.runUntil([&] { return rig.playing(me); }, 6 * kSec));
  CHECK(s.owner(PlayerId{0}) == SlotOwner::Remote);
  const double x0 = rig.serverX(me);
  rig.push(s, Key::D, true);  // keyboard profile 0: D is right (+x)
  rig.runFor(300 * kMs);
  rig.push(s, Key::D, false);
  rig.runFor(100 * kMs);
  CHECK(rig.serverX(me) > x0 + 0.5);
  // The predicted world agrees with where the server put the monkey.
  CHECK(s.client().netStats().checksumMismatches == 0);
  CHECK(s.client().netStats().checksumsCompared > 0);
  CHECK(s.client().confirmedTick().has_value());
}

TEST_CASE("two local devices request two slots, mapped in ascending order", "[client][net]") {
  Rig rig;
  REQUIRE(rig.server->setBot(PlayerId{0}, true));
  Rig::Session& r = rig.addSession(
      lobby({platform::DeviceBinding::keyboard(0), platform::DeviceBinding::keyboard(1)}));
  NetSession& s = *r.s;
  CHECK(s.requestedSlots() == 2);
  REQUIRE(rig.runUntil([&] { return s.running(); }, 2 * kSec));
  CHECK(s.localSlots() == 0b110);
  const PlayerId a = s.slotOfDevice(0);
  const PlayerId b = s.slotOfDevice(1);
  CHECK(a == PlayerId{1});
  CHECK(b == PlayerId{2});
  REQUIRE(rig.runUntil([&] { return rig.playing(a) && rig.playing(b); }, 6 * kSec));
  const double ax = rig.serverX(a);
  const double bx = rig.serverX(b);
  rig.push(s, Key::D, true);     // device 0 (WASD) right
  rig.push(s, Key::Left, true);  // device 1 (arrows) left
  rig.runFor(300 * kMs);
  rig.push(s, Key::D, false);
  rig.push(s, Key::Left, false);
  rig.runFor(100 * kMs);
  CHECK(rig.serverX(a) > ax + 0.5);
  CHECK(rig.serverX(b) < bx - 0.5);
}

TEST_CASE("a remote client's mispredictions engage rollback smoothing, which decays",
          "[client][net]") {
  Rig rig;
  Rig::Session& r = rig.addSession(lobby({platform::DeviceBinding::keyboard(0)}), {}, 40 * kMs);
  NetSession& s = *r.s;
  rig.addRemote(40 * kMs, 0xF00D, Rig::Human::Wander);
  REQUIRE(rig.runUntil([&] { return s.running(); }, 2 * kSec));
  REQUIRE(rig.runUntil([&] { return rig.playing(s.slotOfDevice(0)); }, 6 * kSec));

  // The remote slot: present and not ours.
  std::optional<uint8_t> remote;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i)
    if (s.owner(PlayerId{i}) == SlotOwner::Remote) remote = i;
  REQUIRE(remote.has_value());
  const uint8_t other = remote.value_or(0);

  float maxOffset = 0.0f;
  std::optional<presentation::PresentationState> snapshot;
  r.afterFrame = [&] {
    const float o = offsetLength(s.presentation().players[other].motion);
    if (s.client().frameReport().rolledBack && o > maxOffset) {
      maxOffset = o;
      snapshot = s.presentation();
    }
  };
  const uint64_t rollbacksBefore = s.rollbackFrames();
  rig.runFor(3 * kSec);
  r.afterFrame = nullptr;
  CHECK(s.rollbackFrames() > rollbacksBefore);
  CHECK(s.client().netStats().mispredictedRemote > 0);
  REQUIRE(snapshot.has_value());
  CHECK(maxOffset > 1e-3f);
  CHECK(maxOffset <= presentation::kSmoothingSnapDistance);

  // Without further corrections the offset decays (τ = 120 ms for remotes).
  presentation::PresentationState ps = snapshot.value_or(presentation::PresentationState{});
  presentation::RenderState rs;
  const sim::WorldState& w = s.curr();
  const float before = offsetLength(ps.players[other].motion);
  presentation::buildRenderState(ps, w, w, 1.0f, {}, s.content(), 0.05f, rs);
  const float after = offsetLength(ps.players[other].motion);
  CHECK(after < before);
  for (int i = 0; i < 40; ++i)
    presentation::buildRenderState(ps, w, w, 1.0f, {}, s.content(), 0.05f, rs);
  CHECK(offsetLength(ps.players[other].motion) == 0.0f);
  CHECK(s.client().netStats().checksumMismatches == 0);
}

TEST_CASE("events reach the presentation exactly once across rollbacks", "[client][net]") {
  Rig rig;
  Rig::Session& r = rig.addSession(lobby({platform::DeviceBinding::keyboard(0)}), {}, 30 * kMs);
  NetSession& s = *r.s;
  rig.addRemote(30 * kMs, 0xBEE5, Rig::Human::Wander);
  REQUIRE(rig.runUntil([&] { return s.running(); }, 2 * kSec));
  const PlayerId me = s.slotOfDevice(0);
  REQUIRE(rig.runUntil([&] { return rig.playing(me); }, 6 * kSec));

  size_t delivered = 0;
  r.afterFrame = [&] { delivered += countDashes(s.frameEvents(), me); };
  const size_t serverBefore = countDashes(rig.serverEvents, me);
  const uint64_t rollbacksBefore = s.client().netStats().rollbacks;
  rig.runFor(200 * kMs);
  rig.push(s, Key::K, true);  // keyboard profile 0: K dashes
  rig.runFor(50 * kMs);
  rig.push(s, Key::K, false);
  rig.runFor(1500 * kMs);  // past confirmation: the dash event is final
  r.afterFrame = nullptr;

  const size_t onServer = countDashes(rig.serverEvents, me) - serverBefore;
  CHECK(s.client().netStats().rollbacks > rollbacksBefore);  // re-simulations happened
  CHECK(onServer == 1);
  CHECK(delivered == onServer);
  CHECK(s.client().journal().stats().deduplicated > 0);
}

TEST_CASE("a server that refuses the build shows the reject reason", "[client][net]") {
  net::ServerConfig sc;
  sc.buildId = 42;
  Rig rig(sc);
  NetSessionConfig cfg;
  cfg.buildId = 7;
  Rig::Session& r = rig.addSession(lobby({platform::DeviceBinding::keyboard(0)}), cfg);
  NetSession& s = *r.s;
  REQUIRE(rig.runUntil([&] { return s.phase() == NetPhase::Rejected; }, 2 * kSec));
  CHECK(s.client().rejectReason() == net::RejectReason::BuildMismatch);
  CHECK(s.statusText().find("Rejected") != std::string::npos);
  CHECK(s.statusText().find("build mismatch") != std::string::npos);
  CHECK(!s.everRan());
  CHECK(s.ticksThisFrame() == 0);
}

TEST_CASE("disconnects: a cut link, a quit, and the lobby bindings come back", "[client][net]") {
  Rig rig;
  const platform::InputSampler sampler =
      lobby({platform::DeviceBinding::keyboard(0), platform::DeviceBinding::keyboard(1)});
  Rig::Session& a = rig.addSession(sampler);
  Rig::Session& b = rig.addSession(lobby({platform::DeviceBinding::keyboard(0)}));
  REQUIRE(rig.runUntil([&] { return a.s->running() && b.s->running(); }, 2 * kSec));

  // The link dies: the transport reports it, the session shows why.
  rig.net.cut(a.transport->self());
  REQUIRE(rig.runUntil([&] { return a.s->phase() == NetPhase::Disconnected; }, kSec));
  CHECK(a.s->everRan());
  CHECK(a.s->statusText().find("Disconnected from loopback") != std::string::npos);

  // Quitting on purpose.
  b.s->disconnect();
  CHECK(b.s->phase() == NetPhase::Disconnected);
  CHECK(b.s->statusText().find("left the match") != std::string::npos);
  rig.runFor(100 * kMs);
  CHECK(b.s->phase() == NetPhase::Disconnected);

  // Back to the local lobby: the lobby slots get their devices back.
  const platform::InputSampler back = a.s->lobbySampler();
  CHECK(back.binding(PlayerId{0}) == platform::DeviceBinding::keyboard(0));
  CHECK(back.binding(PlayerId{1}) == platform::DeviceBinding::keyboard(1));
  CHECK(back.boundSlots() == 0b11);
}

TEST_CASE("a session with no transport fails with the reason; one that never connects says so",
          "[client][net]") {
  const platform::InputSampler sampler = lobby({platform::DeviceBinding::keyboard(0)});
  NetSessionConfig cfg;
  cfg.serverLabel = "ws://bad";
  NetSession failed(cfg, sampler, nullptr, 0, nullptr, "malformed URL");
  failed.advance(16 * kMs);
  CHECK(failed.phase() == NetPhase::Failed);
  CHECK(failed.statusText() == "Cannot connect to ws://bad: malformed URL");

  // A link cut before it connected: no Welcome ever comes, the client times
  // out (ClientConfig::serverTimeoutNs, 5 s) and nothing ran.
  Rig rig;
  Rig::Session& r = rig.addSession(sampler);
  rig.net.cut(r.transport->self());
  REQUIRE(rig.runUntil([&] { return r.s->phase() == NetPhase::Disconnected; }, 6 * kSec));
  CHECK(r.s->statusText().find("could not reach") != std::string::npos);
}

TEST_CASE("a spectating session requests no slots and sees every player as remote",
          "[client][net]") {
  Rig rig;
  REQUIRE(rig.server->setBot(PlayerId{0}, true));
  REQUIRE(rig.server->setBot(PlayerId{1}, true));
  NetSessionConfig cfg;
  cfg.spectate = true;
  Rig::Session& r = rig.addSession(lobby({platform::DeviceBinding::keyboard(0)}), cfg);
  NetSession& s = *r.s;
  CHECK(s.requestedSlots() == 0);
  REQUIRE(rig.runUntil([&] { return s.running(); }, 2 * kSec));
  rig.runFor(500 * kMs);
  CHECK(s.localSlots() == 0);
  CHECK(s.owner(PlayerId{0}) == SlotOwner::Remote);
  CHECK(s.owner(PlayerId{1}) == SlotOwner::Remote);
  CHECK(s.statusText().find("Spectating") != std::string::npos);
  CHECK(s.client().netStats().ticks > 0);
}

TEST_CASE("the online lobby learns the server's match config, power-up set included",
          "[client][net][lobby]") {
  net::ServerConfig config;
  config.match.seed = 0x5E7;
  config.match.powerUps = 0b0101;
  Rig rig(std::move(config));
  REQUIRE(rig.server->setBot(PlayerId{0}, true));
  Rig::Session& r = rig.addSession(lobby({platform::DeviceBinding::keyboard(0)}));
  NetSession& s = *r.s;
  CHECK_FALSE(s.configKnown());
  REQUIRE(rig.runUntil([&] { return s.configKnown(); }, 2 * kSec));
  CHECK(s.client().matchConfig().powerUps == 0b0101);
  CHECK(s.client().matchConfig().seed == 0x5E7);
  REQUIRE(rig.runUntil([&] { return s.running(); }, 2 * kSec));
  CHECK(s.matchContent().powerUps == 0b0101);  // the client simulates with the server's set
  CHECK((s.client().matchConfig().players & 0b11) == 0b11);
}
