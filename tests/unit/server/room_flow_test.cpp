// The v0.42 room flow end to end (architecture §6.6, §11 "M6b design"):
// ServerHost with HostConfig::room over the loopback emulator, two real
// NetClients scripted as players. Two clients join a waiting room (the
// first is host, names reach everyone, nothing ticks while the links stay
// alive), a non-host's Start is ignored, the host's invalid settings are
// refused and its valid ones applied, the host starts a short deathmatch
// with bots, it plays to MatchEnded, the room is Over (nothing ticks), the
// host plays again (Waiting, bots gone), the host leaves and the other client
// becomes host and starts the next match. Time is a deterministic 1 ms step
// loop, as in server_host_test.cpp.
#include "kfm/core/rng.hpp"
#include "kfm/net/loopback.hpp"
#include "kfm/net/messages.hpp"
#include "kfm/net/net_client.hpp"
#include "kfm/net/netsim.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/scripted_inputs.hpp"
#include "room.hpp"
#include "server_host.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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
using kfm::net::netsim::kMs;
using kfm::net::netsim::kSec;

namespace {

// Mode 1: a deathmatch short enough to end within seconds of simulated time.
sim::ContentDB shortContent() {
  sim::ContentDB db = sim::defaultContent();
  sim::ModeDef& dm = db.modes[1];
  dm.name = sim::ContentName::of("deathmatch");
  dm.kind = sim::ModeKind::Deathmatch;
  dm.scoreToWin = 2;
  dm.timeLimitTicks = 30 * sim::kTickHz;
  dm.respawnTicks = 60;
  dm.respawnInvulnTicks = 30;
  db.modeCount = 2;
  return db;
}

struct RoomRig {
  explicit RoomRig(server::HostConfig config) : content(shortContent()), net(91) {
    config.log = [this](std::string_view line) { lines.emplace_back(line); };
    auto created = server::ServerHost::create(net.server(), content, std::move(config), now);
    if (!created) FAIL("ServerHost::create: " << created.error());
    host = std::move(*created);
  }

  NetClient& addClient(const std::string& token, std::string_view nick) {
    const LinkConditions l = netsim::link(30 * kMs, 3 * kMs);
    LoopbackTransport& t = net.connectClient(now, l, l);
    ClientConfig cc = netsim::tokenConfig(token);
    cc.nickname = Nickname::from(nick);
    clients.push_back(std::make_unique<NetClient>(content, cc));
    clients.back()->connect(t, now);
    rngs.push_back(Rng::seeded(0xACE + clients.size()));
    return *clients.back();
  }

  void step() {
    now += kMs;
    host->frame(now);
    for (size_t i = 0; i < clients.size(); ++i) {
      NetClient& c = *clients[i];
      c.beginFrame();
      c.pump(now);
      c.reconcile();
      for (uint32_t n = c.ticksDue(now); n > 0; --n) {
        std::array<sim::PlayerInput, sim::kMaxPlayers> in{};
        size_t k = 0;
        for (uint8_t s = 0; s < sim::kMaxPlayers; ++s) {
          if (sim::maskHas(c.localSlots(), PlayerId{s}))
            in[k++] = sim::scriptedInput(rngs[i], c.world(), c.matchContent(), PlayerId{s});
        }
        c.tick(std::span<const sim::PlayerInput>(in.data(), k));
      }
      static_cast<void>(c.drainEvents());
    }
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
  void runFor(uint64_t ns) {
    const uint64_t end = now + ns;
    while (now < end)
      step();
  }
  [[nodiscard]] size_t linesWith(std::string_view what) const {
    return static_cast<size_t>(
        std::ranges::count_if(lines, [&](const std::string& l) { return l.contains(what); }));
  }
  [[nodiscard]] const server::Room& room() const { return *host->room(); }

  sim::ContentDB content;  // before host and clients, which keep a reference
  uint64_t now{0};
  LoopbackNetwork net;
  std::vector<std::string> lines;
  std::unique_ptr<server::ServerHost> host;
  std::vector<std::unique_ptr<NetClient>> clients;
  std::vector<Rng> rngs;
};

// The client's newest RoomState (a default one before any arrived).
[[nodiscard]] RoomStateMsg roomOf(const NetClient& c) {
  return c.roomState().value_or(RoomStateMsg{});
}

[[nodiscard]] bool roomIs(const NetClient& c, RoomPhase phase) {
  return c.roomState().has_value() && roomOf(c).phase == phase;
}

}  // namespace

TEST_CASE("The room flow: wait, settings, start, play, over, play again, host hand-over",
          "[server][room][flow]") {
  server::HostConfig cfg;
  cfg.room = true;
  cfg.server.match.seed = 0x5EED;
  cfg.bots = server::referenceBots(1);  // the room's default: one bot
  RoomRig rig(cfg);
  REQUIRE(rig.host->room() != nullptr);
  CHECK(rig.room().phase() == RoomPhase::Waiting);
  CHECK(rig.room().settings().bots == 1);
  CHECK(rig.host->server().bots() == 0);  // no bots until Start

  // --- two clients join a waiting room -----------------------------------------
  NetClient& a = rig.addClient("alpha", "Alpha");
  NetClient& b = rig.addClient("beta", "Beta");
  REQUIRE(rig.runUntil([&] { return a.running() && b.running() && roomOf(b).humans == 0b11; },
                       5 * kSec));
  CHECK(a.localSlots() == 0b01);
  CHECK(b.localSlots() == 0b10);
  const RoomStateMsg waiting = roomOf(b);
  CHECK(waiting.phase == RoomPhase::Waiting);
  CHECK(waiting.host == PlayerId{0});
  CHECK(waiting.names[0].view() == "Alpha");
  CHECK(waiting.names[1].view() == "Beta");
  CHECK(waiting.bots == 0);
  CHECK(waiting.settings.bots == 1);
  CHECK(a.holding());
  CHECK(b.holding());

  // Nothing ticks, the links stay alive past every timeout (5 s).
  const Tick frozen = rig.host->server().nextTick();
  const Tick aTick = a.localTick();
  rig.runFor(7 * kSec);
  CHECK(rig.host->server().nextTick() == frozen);
  CHECK(rig.host->stats().ticks == 0);
  CHECK(a.localTick() == aTick);
  CHECK(a.running());
  CHECK(b.running());
  CHECK(rig.host->server().peers().size() == 2);
  CHECK(rig.host->server().stats().protocolErrors == 0);

  // --- a non-host's Start is ignored ---------------------------------------------
  b.sendStart();
  rig.runFor(200 * kMs);
  CHECK(rig.room().phase() == RoomPhase::Waiting);
  CHECK(rig.host->server().nextTick() == frozen);

  // --- the host's settings: an invalid set is refused, a valid one applied ---------
  const size_t refused = rig.linesWith("room settings refused");
  a.sendRoomSettings(RoomSettings{.arena = 7, .mode = 1, .loadout = 0, .bots = 2});
  rig.runFor(200 * kMs);
  CHECK(rig.linesWith("room settings refused") == refused + 1);
  CHECK(rig.room().settings().mode == 0);
  const RoomSettings wanted{.arena = 0, .mode = 1, .loadout = 0, .powerUps = 0, .bots = 2};
  b.sendRoomSettings(wanted);  // not the host
  rig.runFor(200 * kMs);
  CHECK(rig.room().settings().mode == 0);
  a.sendRoomSettings(wanted);
  REQUIRE(rig.runUntil([&] { return roomOf(b).settings == wanted; }, kSec));
  CHECK(rig.room().settings() == wanted);

  // --- the host starts: bots placed, the match runs --------------------------------
  a.sendStart();
  REQUIRE(rig.runUntil(
      [&] { return roomIs(a, RoomPhase::Playing) && roomIs(b, RoomPhase::Playing); }, kSec));
  CHECK(rig.host->server().bots() == 0b1100);
  CHECK(roomOf(b).bots == 0b1100);
  REQUIRE(rig.runUntil([&] { return !a.holding() && !b.holding(); }, 2 * kSec));
  CHECK(a.netStats().matchRestarts == 1);
  CHECK(a.simConfig().mode == 1);
  CHECK(a.simConfig().powerUps == 0);
  CHECK(rig.host->server().matchConfig().mode == 1);
  CHECK(rig.linesWith("room start") == 1);

  // --- it plays to the end: Over, nothing ticks ------------------------------------
  REQUIRE(rig.runUntil([&] { return rig.room().phase() == RoomPhase::Over; }, 120 * kSec));
  CHECK(rig.host->stats().matchesCompleted == 1);
  CHECK(!rig.host->server().pendingRestart());  // no auto-restart
  REQUIRE(
      rig.runUntil([&] { return roomIs(a, RoomPhase::Over) && roomIs(b, RoomPhase::Over); }, kSec));
  CHECK(a.holding());
  const Tick ended = rig.host->server().nextTick();
  rig.runFor(2 * kSec);
  CHECK(rig.host->server().nextTick() == ended);
  CHECK(a.netStats().checksumMismatches == 0);
  CHECK(b.netStats().checksumMismatches == 0);
  CHECK(a.netStats().checksumsCompared > 10);

  // --- play again: Waiting with the same settings, bots gone ------------------------
  b.sendStart();  // not the host
  rig.runFor(200 * kMs);
  CHECK(rig.room().phase() == RoomPhase::Over);
  a.sendStart();
  REQUIRE(rig.runUntil([&] { return roomIs(b, RoomPhase::Waiting); }, kSec));
  CHECK(rig.room().settings() == wanted);
  CHECK(rig.host->server().bots() == 0);
  CHECK(roomOf(b).bots == 0);
  CHECK(!roomOf(b).winner.isValid());

  // --- the host leaves: the other client becomes host and starts --------------------
  a.disconnect();
  REQUIRE(rig.runUntil([&] { return roomOf(b).host == PlayerId{1}; }, kSec));
  CHECK(roomOf(b).humans == 0b10);
  CHECK(rig.room().host() == PlayerId{1});
  b.sendStart();
  REQUIRE(rig.runUntil([&] { return roomIs(b, RoomPhase::Playing) && !b.holding(); }, 2 * kSec));
  CHECK(rig.host->server().bots() == 0b101);  // the lowest free slots
  const uint64_t compared = b.netStats().checksumsCompared;
  rig.runFor(5 * kSec);
  CHECK(b.running());
  CHECK(b.netStats().checksumsCompared > compared + 10);
  CHECK(b.netStats().checksumMismatches == 0);
  CHECK(b.netStats().resyncRequests == 0);
  CHECK(b.netStats().matchRestarts == 2);
}

TEST_CASE("The room flow: the last human leaving returns the room to Waiting; openRoom resets",
          "[server][room][flow]") {
  server::HostConfig cfg;
  cfg.room = true;
  cfg.bots = server::referenceBots(2);
  RoomRig rig(cfg);
  NetClient& a = rig.addClient("solo", "Solo");
  REQUIRE(rig.runUntil([&] { return a.running() && a.roomState(); }, 5 * kSec));
  a.sendRoomSettings(RoomSettings{.mode = 1, .bots = 3});
  REQUIRE(rig.runUntil([&] { return roomOf(a).settings.bots == 3; }, kSec));
  a.sendStart();
  REQUIRE(rig.runUntil([&] { return roomIs(a, RoomPhase::Playing) && !a.holding(); }, 2 * kSec));
  CHECK(rig.host->server().bots() == 0b1110);
  rig.runFor(kSec);
  const uint64_t ticks = rig.host->stats().ticks;
  CHECK(ticks > 50);

  // Gone mid-match: the slot is reserved for the reconnect grace (10 s),
  // the bots play on, then the room is empty.
  a.disconnect();
  rig.runFor(2 * kSec);
  CHECK(rig.room().phase() == RoomPhase::Playing);
  CHECK(rig.host->server().humanSlots() == 0b1);
  REQUIRE(rig.runUntil([&] { return rig.room().phase() == RoomPhase::Waiting; }, 12 * kSec));
  CHECK(rig.host->server().bots() == 0);
  CHECK(rig.room().settings().bots == 3);  // the room keeps its settings
  const uint64_t stopped = rig.host->stats().ticks;
  CHECK(stopped > ticks);
  rig.runFor(kSec);
  CHECK(rig.host->stats().ticks == stopped);
  CHECK(rig.linesWith("room empty") == 1);

  // A new lobby room: the server's defaults again.
  rig.host->openRoom("room ABCDE");
  CHECK(rig.room().settings() == server::roomSettingsOf(sim::MatchConfig{}, 2));
  CHECK(rig.linesWith("room reset: room ABCDE") == 1);
}
