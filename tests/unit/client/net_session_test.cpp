// NetSession (apps/client network mode) against an in-process NetServer over
// the loopback emulator (architecture §6.1, §6.2, §6.9, §7, §11 M3): the
// connect/sync/running flow, local devices mapped onto server slots, inputs
// from a fake keyboard reaching the authoritative world, rollback smoothing,
// exactly-once events across rollbacks, reject and disconnect; v0.42: the
// nickname in Hello and the room view (a scripted host sets RoomState).
#include "core/net_session.hpp"

#include "core/menus.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/net/loopback.hpp"
#include "kfm/net/net_client.hpp"
#include "kfm/net/net_server.hpp"
#include "kfm/platform/input_event.hpp"
#include "kfm/platform/input_sampler.hpp"
#include "kfm/presentation/builder.hpp"
#include "kfm/presentation/camera.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/presentation/world_basis.hpp"
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
    if (!ticking) {
      // v0.42: a waiting room's server pumps but never ticks.
      tickBaseNs = now;
      serverTicks = 0;
    }
    while (ticking && now >= tickBaseNs + serverTickAt(serverTicks)) {
      server->tick(now);
      ++serverTicks;
      for (const sim::SimEvent& e : server->tickEvents())
        serverEvents.push_back(e);
    }
    for (const net::ServerEvent& e : server->events())
      netEvents.push_back(e);
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
  uint64_t tickBaseNs{0};
  bool ticking{true};
  std::vector<sim::SimEvent> serverEvents;
  std::vector<net::ServerEvent> netEvents;
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

TEST_CASE("a session queued for a bot's slot plays once the slot is granted (v0.44)",
          "[client][net][yield]") {
  net::ServerConfig cfg;
  cfg.maxPlayers = 2;
  cfg.queueForBotSlots = true;
  Rig rig(cfg);
  REQUIRE(rig.server->setBot(PlayerId{0}, true));
  REQUIRE(rig.server->setBot(PlayerId{1}, true));
  Rig::Session& r = rig.addSession(lobby({platform::DeviceBinding::keyboard(0)}), {}, 20 * kMs);
  NetSession& s = *r.s;
  REQUIRE(rig.runUntil([&] { return s.running(); }, 2 * kSec));
  CHECK(s.localSlots() == 0);
  CHECK(s.client().waitingForSlot());
  CHECK(s.statusText().find("until a bot's slot is free") != std::string::npos);
  rig.runFor(kSec);
  const uint64_t jumps = s.client().netStats().hardJumps;

  // The host retires the bot in slot 1 and grants it.
  REQUIRE(rig.server->setBot(PlayerId{1}, false));
  rig.step();
  const net::PeerId peer = rig.server->oldestQueued();
  REQUIRE(peer.isValid());
  REQUIRE(rig.server->grantQueued(peer));
  REQUIRE(rig.runUntil([&] { return s.localSlots() == 0b10; }, kSec));
  rig.runFor(100 * kMs);
  CHECK(s.slotOfDevice(0) == PlayerId{1});
  CHECK(s.owner(PlayerId{1}) == SlotOwner::Local);
  CHECK_FALSE(s.client().waitingForSlot());
  CHECK(s.client().netStats().hardJumps == jumps + 1);  // ran forward to the lead
  CHECK(s.client().netStats().leadTicks > 0);

  // Its keyboard moves its monkey on the server.
  REQUIRE(rig.runUntil([&] { return rig.playing(PlayerId{1}); }, 10 * kSec));
  const double x0 = rig.serverX(PlayerId{1});
  rig.push(s, Key::D, true);
  rig.runFor(300 * kMs);
  rig.push(s, Key::D, false);
  rig.runFor(100 * kMs);
  CHECK(rig.serverX(PlayerId{1}) > x0 + 0.5);
  CHECK(s.client().netStats().checksumMismatches == 0);
  CHECK(s.client().netStats().resyncRequests == 0);
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

TEST_CASE("reconnect from the UI: a lost session comes back with its slot and nickname (v0.44)",
          "[client][net][reconnect]") {
  Rig rig;
  REQUIRE(rig.server->setBot(PlayerId{0}, true));
  NetSessionConfig cfg;
  cfg.nickname = "Chimp";
  cfg.token = {std::byte{'t'}, std::byte{'o'}, std::byte{'k'}};
  Rig::Session& r = rig.addSession(lobby({platform::DeviceBinding::keyboard(0)}), cfg, 10 * kMs);
  NetSession& s = *r.s;
  REQUIRE(rig.runUntil([&] { return s.running(); }, 2 * kSec));
  const sim::PlayerMask slots = s.localSlots();
  CHECK(slots == 0b10);
  CHECK_FALSE(s.canReconnect());

  // The link dies: both sides time out (5 s); the server reserves the slot.
  rig.net.cut(r.transport->self());
  REQUIRE(rig.runUntil([&] { return s.phase() == NetPhase::Disconnected; }, 7 * kSec));
  CHECK(s.canReconnect());
  const NetSessionConfig again = s.reconnectConfig();
  CHECK(again.token == cfg.token);
  CHECK(again.nickname == "Chimp");
  CHECK_FALSE(again.lobby.has_value());
  rig.runFor(200 * kMs);  // the server's own timeout has passed: the slot is reserved
  CHECK(rig.server->peers().empty());
  CHECK(rig.server->humanSlots() == slots);

  // The button: a new transport, the same token.
  r.transport = &rig.net.connectClient(rig.now, link(10 * kMs), link(10 * kMs));
  s.reconnect(r.transport, rig.now);
  CHECK(s.reconnects() == 1);
  CHECK(s.phase() == NetPhase::Connecting);
  REQUIRE(rig.runUntil([&] { return s.running(); }, 3 * kSec));
  CHECK(s.localSlots() == slots);
  CHECK(rig.server->stats().reconnects == 1);
  CHECK(rig.server->nickname(PlayerId{1}).view() == "Chimp");
  CHECK(s.owner(PlayerId{1}) == SlotOwner::Local);
  CHECK_FALSE(s.canReconnect());

  // Its keyboard drives the reclaimed slot again.
  REQUIRE(rig.runUntil([&] { return rig.playing(PlayerId{1}); }, 10 * kSec));
  const double x0 = rig.serverX(PlayerId{1});
  rig.push(s, Key::D, true);
  rig.runFor(300 * kMs);
  rig.push(s, Key::D, false);
  rig.runFor(100 * kMs);
  CHECK(rig.serverX(PlayerId{1}) > x0 + 0.5);
  CHECK(s.client().netStats().checksumMismatches == 0);

  // Quitting on purpose offers no reconnect.
  s.disconnect();
  CHECK(s.phase() == NetPhase::Disconnected);
  CHECK_FALSE(s.canReconnect());
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

TEST_CASE("the nickname rides in Hello; the room view follows RoomState (v0.42)",
          "[client][net][room]") {
  Rig rig;
  rig.ticking = false;  // the room waits: the server never ticks
  NetSessionConfig cfg;
  cfg.nickname = "  Chimp\tPanzee  ";
  Rig::Session& r = rig.addSession(lobby({platform::DeviceBinding::keyboard(0)}), cfg, 10 * kMs);
  NetSession& s = *r.s;
  REQUIRE(rig.runUntil([&] { return s.synced(); }, 2 * kSec));
  CHECK(s.localSlots() == 0b1);
  CHECK(rig.server->nickname(PlayerId{0}).view() == "ChimpPanzee");
  CHECK(!s.roomState());
  CHECK(s.phase() == NetPhase::Running);  // no room: the M3 flow
  CHECK_FALSE(s.isHost());
  CHECK(s.slotName(PlayerId{0}) == "P1");

  // The scripted host: a waiting room with this session as host and a bot.
  REQUIRE(rig.server->setBot(PlayerId{1}, true));
  net::RoomStateMsg room;
  room.host = PlayerId{0};
  room.humans = 0b1;
  room.bots = 0b10;
  room.settings.bots = 1;
  room.names[0] = rig.server->nickname(PlayerId{0});
  rig.server->setRoomState(room);
  REQUIRE(rig.runUntil([&] { return s.phase() == NetPhase::Waiting; }, kSec));
  CHECK(s.isHost());
  CHECK(s.slotName(PlayerId{0}) == "ChimpPanzee");
  CHECK(s.slotName(PlayerId{1}) == "BOT2");
  CHECK(s.statusText().find("you are the host") != std::string::npos);
  CHECK(std::string(client::netPhaseName(s.phase())) == "waiting");

  // Holding: keys pressed in the waiting room send nothing, no tick runs.
  const Tick held = s.client().localTick();
  const uint64_t accepted = rig.server->stats().inputsAccepted;
  rig.push(s, Key::D, true);
  rig.runFor(500 * kMs);
  rig.push(s, Key::D, false);
  CHECK(s.client().localTick() == held);
  CHECK(rig.server->stats().inputsAccepted == accepted);
  CHECK(s.ticksThisFrame() == 0);

  // The host's controls reach the server as events.
  const net::RoomSettings wanted{.arena = 0, .mode = 0, .loadout = 0, .powerUps = 1, .bots = 2};
  s.sendSettings(wanted);
  s.start();
  rig.runFor(100 * kMs);
  std::vector<net::ServerEventKind> kinds;
  for (const net::ServerEvent& e : rig.netEvents) {
    if (e.kind == net::ServerEventKind::RoomSettingsRequested) CHECK(e.settings == wanted);
    if (e.kind == net::ServerEventKind::RoomSettingsRequested ||
        e.kind == net::ServerEventKind::StartRequested)
      kinds.push_back(e.kind);
  }
  CHECK(kinds == std::vector<net::ServerEventKind>{net::ServerEventKind::RoomSettingsRequested,
                                                   net::ServerEventKind::StartRequested});

  // The host starts: the match restarts at the next tick, the server ticks
  // again and the session resumes running with inputs reaching the server.
  sim::MatchConfig next = rig.server->matchConfig();
  next.seed = 77;
  rig.server->restartMatch(next, rig.server->nextTick(), 5);
  room.phase = net::RoomPhase::Playing;
  rig.server->setRoomState(room);
  rig.ticking = true;
  REQUIRE(rig.runUntil([&] { return s.running() && !s.client().holding(); }, 2 * kSec));
  CHECK(s.client().netStats().matchRestarts == 1);
  CHECK(s.client().matchConfig().seed == 77);
  rig.runFor(2 * kSec);
  CHECK(rig.server->stats().inputsAccepted > accepted + 60);
  CHECK(s.client().netStats().checksumMismatches == 0);
  CHECK(s.client().netStats().checksumsCompared > 0);
  // Lead re-established from a fresh sync: a few ticks, not the paused time.
  CHECK(s.client().netStats().leadTicks < 15);

  // Over: the session shows the match over and holds again.
  room.phase = net::RoomPhase::Over;
  room.winner = PlayerId{0};
  rig.server->setRoomState(room);
  rig.ticking = false;
  REQUIRE(rig.runUntil([&] { return s.phase() == NetPhase::Over; }, kSec));
  CHECK(s.client().holding());
  CHECK(s.statusText().find("match over") != std::string::npos);
}

TEST_CASE("mouse aim online follows the predicted monkey, not the confirmed one (v0.45)",
          "[client][net][mouse]") {
  Rig rig;
  REQUIRE(rig.server->setBot(PlayerId{0}, true));
  // 100 ms each way: the confirmed world trails the predicted one by well
  // over a unit at full speed.
  Rig::Session& r = rig.addSession(lobby({platform::DeviceBinding::keyboard(0)}), {}, 100 * kMs);
  NetSession& s = *r.s;
  constexpr float kW = 1280.0f;
  constexpr float kH = 720.0f;
  s.setAimViewport({.width = kW, .height = kH, .pixelDensity = 1.0f});
  REQUIRE(rig.runUntil([&] { return s.running(); }, 3 * kSec));
  const PlayerId me = s.slotOfDevice(0);
  REQUIRE(rig.runUntil(
      [&] {
        return rig.playing(me) && s.curr().rules.phase == sim::MatchPhase::Playing &&
               s.curr().playerBySlot(me) != nullptr;
      },
      8 * kSec));
  rig.runFor(200 * kMs);  // the round's countdown has passed on the predicted side too

  // The cursor 2.5 units right of the predicted monkey, on the ground.
  const sim::Player* p = s.curr().playerBySlot(me);
  REQUIRE(p != nullptr);
  const presentation::Vec2f start = presentation::toFloat(p->transform.pos);
  const float cx = start.x + 2.5f;
  const presentation::Camera cam = presentation::makeArenaCamera(s.matchContent().arena, kW, kH);
  const presentation::ScreenPoint px =
      presentation::worldToScreen(cam, presentation::simToWorld(presentation::Vec2f{cx, start.y}));
  REQUIRE(px.inFront);
  InputEvent motion{};
  motion.kind = InputEventKind::MouseMotion;
  motion.x = px.x;
  motion.y = px.y;
  motion.timestampNs = rig.now;
  s.push(motion);
  rig.runFor(100 * kMs);
  const auto aimOf = [&](const sim::WorldState& w) {
    const sim::Player* q = w.playerBySlot(me);
    return q != nullptr ? q->intent.aim.bam >> 8 : -1;
  };
  CHECK(aimOf(s.curr()) == 0);  // toward +x

  // Walk right past the cursor. The first frame the predicted monkey is
  // clearly past it, the predicted world already aims back (-x) while the
  // confirmed monkey is still short of the cursor.
  bool checked = false;
  r.afterFrame = [&] {
    if (checked) return;
    const sim::Player* q = s.curr().playerBySlot(me);
    if (q == nullptr || q->transform.pos.x.toFloat() < cx + 0.4f) return;
    checked = true;
    CHECK(aimOf(s.curr()) == 128);
    CHECK(rig.serverX(me) < static_cast<double>(cx));
  };
  rig.push(s, Key::D, true);
  REQUIRE(rig.runUntil([&] { return checked; }, 2 * kSec));
  rig.push(s, Key::D, false);
  rig.runFor(600 * kMs);
  CHECK(aimOf(rig.server->world()) == 128);  // the server got the same aim
  r.afterFrame = nullptr;
}

TEST_CASE("the pause menu online: the session advances, its monkey idles (v0.46)",
          "[client][net][menu]") {
  Rig rig;
  REQUIRE(rig.server->setBot(PlayerId{0}, true));
  Rig::Session& r = rig.addSession(lobby({platform::DeviceBinding::keyboard(0)}), {}, 10 * kMs);
  NetSession& s = *r.s;
  REQUIRE(rig.runUntil([&] { return s.running(); }, 2 * kSec));
  const PlayerId me = s.slotOfDevice(0);
  REQUIRE(rig.runUntil([&] { return rig.playing(me); }, 6 * kSec));

  rig.push(s, Key::D, true);  // running right, and still held under the menu
  rig.runFor(150 * kMs);
  const double x0 = rig.serverX(me);
  client::PauseMenu menu;
  client::openPauseMenu(menu, s);
  CHECK(menu.isOpen());
  CHECK(menu.online());
  CHECK(s.inputSuspended());
  const Tick before = s.client().localTick();
  const uint64_t accepted = rig.server->stats().inputsAccepted;
  rig.runFor(200 * kMs);  // the idle inputs reach the server
  const double x1 = rig.serverX(me);
  rig.runFor(400 * kMs);
  // The match runs on: ticks are stepped and inputs keep reaching the server.
  CHECK(s.running());
  CHECK(s.client().localTick().value > before.value + 30);
  CHECK(rig.server->stats().inputsAccepted > accepted + 30);
  CHECK(rig.serverX(me) > x0);
  CHECK(std::abs(rig.serverX(me) - x1) < 1e-3);  // ... but the monkey stands still

  // Resume: the key still held moves the monkey again.
  client::closePauseMenu(menu, s);
  CHECK_FALSE(s.inputSuspended());
  const double x2 = rig.serverX(me);
  rig.runFor(300 * kMs);
  rig.push(s, Key::D, false);
  CHECK(rig.serverX(me) > x2 + 0.2);
  CHECK(s.client().netStats().checksumMismatches == 0);
}
