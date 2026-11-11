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
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <optional>
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
  // Mode 2: the same deathmatch played long (the yield tests, v0.44).
  sim::ModeDef& longDm = db.modes[2];
  longDm = dm;
  longDm.name = sim::ContentName::of("deathmatch_long");
  longDm.scoreToWin = 50;
  longDm.timeLimitTicks = 600 * sim::kTickHz;
  db.modeCount = 3;
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
  CHECK(waiting.botPlacement == 0b100);           // v7: where Start puts the one bot
  CHECK(waiting.maxPlayers == sim::kMaxPlayers);  // v8: the default cap, every slot
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
  CHECK(roomOf(b).botPlacement == 0b1100);  // the settings change re-sent it

  // --- the host starts: bots placed, the match runs --------------------------------
  a.sendStart();
  REQUIRE(rig.runUntil(
      [&] { return roomIs(a, RoomPhase::Playing) && roomIs(b, RoomPhase::Playing); }, kSec));
  CHECK(rig.host->server().bots() == 0b1100);  // the preview's slots
  CHECK(roomOf(b).bots == 0b1100);
  CHECK(roomOf(b).botPlacement == 0b1100);  // Playing: where the bots are
  CHECK(roomOf(b).maxPlayers == sim::kMaxPlayers);
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
  CHECK(roomOf(b).botPlacement == 0b1100);  // Waiting again: the preview
  CHECK(!roomOf(b).winner.isValid());

  // --- the host leaves: the other client becomes host and starts --------------------
  a.disconnect();
  REQUIRE(rig.runUntil([&] { return roomOf(b).host == PlayerId{1}; }, kSec));
  CHECK(roomOf(b).humans == 0b10);
  CHECK(roomOf(b).botPlacement == 0b101);  // the leave re-sent it
  CHECK(rig.room().host() == PlayerId{1});
  b.sendStart();
  REQUIRE(rig.runUntil([&] { return roomIs(b, RoomPhase::Playing) && !b.holding(); }, 2 * kSec));
  CHECK(rig.host->server().bots() == 0b101);  // the lowest free slots, as previewed
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
  // The long deathmatch: the match must still run through the reconnect grace.
  a.sendRoomSettings(RoomSettings{.mode = 2, .bots = 3});
  REQUIRE(rig.runUntil([&] { return roomOf(a).settings.bots == 3; }, kSec));
  CHECK(roomOf(a).botPlacement == 0b1110);
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

// v0.44 (§6.6): bots yield. A room of four slots, one human and three bots:
// a second human is queued as a spectator, takes the highest bot's slot when
// that bot is off the field (eliminated, or at the round end) one tick after
// the bot left, becomes a player (it runs at the lead and its inputs count)
// and the RoomState shows it; nobody desyncs.
TEST_CASE("The room flow: a bot yields its slot to a joiner in a full room",
          "[server][room][flow][yield]") {
  server::HostConfig cfg;
  cfg.room = true;
  cfg.server.maxPlayers = 4;
  cfg.bots = server::referenceBots(3);
  RoomRig rig(cfg);
  NetClient& a = rig.addClient("first", "First");
  REQUIRE(rig.runUntil([&] { return a.running() && a.roomState(); }, 5 * kSec));
  const bool deathmatch = GENERATE(false, true);
  CAPTURE(deathmatch);
  a.sendRoomSettings(RoomSettings{.mode = static_cast<uint8_t>(deathmatch ? 2 : 0), .bots = 3});
  REQUIRE(rig.runUntil([&] { return roomOf(a).settings.bots == 3; }, kSec));
  CHECK(roomOf(a).botPlacement == 0b1110);
  a.sendStart();
  REQUIRE(rig.runUntil([&] { return roomIs(a, RoomPhase::Playing) && !a.holding(); }, 2 * kSec));
  CHECK(rig.host->server().bots() == 0b1110);
  CHECK(roomOf(a).botPlacement == 0b1110);
  CHECK(rig.host->server().freeSlots() == 0);

  // The room is full: the joiner waits as a spectator, not rejected.
  NetClient& b = rig.addClient("second", "Second");
  REQUIRE(rig.runUntil([&] { return b.running(); }, 5 * kSec));
  CHECK(b.spectator());
  CHECK(b.waitingForSlot());
  CHECK(rig.linesWith("queue peer=") == 1);
  CHECK(rig.host->server().stats().rejects == 0);

  // A bot gives its slot up; the joiner gets it a tick later.
  REQUIRE(rig.runUntil([&] { return b.localSlots() != 0; }, 90 * kSec));
  CHECK(rig.linesWith("yields its slot") == 1);
  CHECK(rig.room().phase() == RoomPhase::Playing);
  CHECK(rig.linesWith("room slot granted") == 1);
  // The highest bot off the field (eliminated, or every bot at a round end):
  // which one depends on how the match plays out.
  const auto got = static_cast<uint32_t>(b.localSlots());
  REQUIRE(std::popcount(got) == 1);
  CHECK((got & 0b1110u) == got);
  const auto y = static_cast<uint8_t>(std::countr_zero(got));
  const auto bots = static_cast<sim::PlayerMask>(0b1110u & ~got);
  const auto humans = static_cast<sim::PlayerMask>(0b0001u | got);
  CHECK_FALSE(b.waitingForSlot());
  CHECK(rig.host->server().bots() == bots);
  CHECK(rig.host->botSpec(PlayerId{y}) == std::nullopt);
  REQUIRE(
      rig.runUntil([&] { return roomOf(a).humans == humans && roomOf(b).humans == humans; }, kSec));
  CHECK(roomOf(b).bots == bots);
  CHECK(roomOf(b).botPlacement == bots);  // the yielded bot left the placement too
  CHECK(roomOf(b).names[y].view() == "Second");
  CHECK(roomOf(b).host == PlayerId{0});
  // The slot left for a tick: a deathmatch forfeits the bot's score and
  // deaths; the joiner's monkey spawns (after respawnTicks, or at the next
  // round start).
  if (deathmatch) {
    CHECK(rig.host->server().world().rules.scores[y] == 0);
    CHECK(rig.host->server().world().rules.deaths[y] == 0);
  }
  REQUIRE(
      rig.runUntil([&] { return rig.host->server().world().players.isAliveIndex(y); }, 30 * kSec));

  // It plays: ahead of the server, its inputs accepted, no desync.
  const uint64_t compared = b.netStats().checksumsCompared;
  rig.runFor(5 * kSec);
  CHECK(b.running());
  CHECK(b.netStats().leadTicks > 0);
  CHECK(b.netStats().checksumsCompared > compared + 10);
  CHECK(b.netStats().checksumMismatches == 0);
  CHECK(a.netStats().checksumMismatches == 0);
  CHECK(b.netStats().resyncRequests == 0);
  const ServerPeerInfo info = rig.host->server()
                                  .peerInfo(rig.host->server().ownerOf(PlayerId{y}))
                                  .value_or(ServerPeerInfo{});
  CHECK(info.inputMsgs > 200);
  CHECK(info.substitutions < 30);
}

// Outside Playing the swap is immediate: a joiner of a full room that shows
// its match over gets the highest bot's slot at once. Play again then places
// only the bots there is room for.
TEST_CASE("The room flow: in Over a bot yields at once; Start places the bots that fit",
          "[server][room][flow][yield]") {
  server::HostConfig cfg;
  cfg.room = true;
  cfg.server.maxPlayers = 3;
  cfg.bots = server::referenceBots(2);
  RoomRig rig(cfg);
  NetClient& a = rig.addClient("host", "Host");
  REQUIRE(rig.runUntil([&] { return a.running() && a.roomState(); }, 5 * kSec));
  a.sendRoomSettings(RoomSettings{.mode = 1, .bots = 2});
  REQUIRE(rig.runUntil([&] { return roomOf(a).settings.mode == 1; }, kSec));
  a.sendStart();
  REQUIRE(rig.runUntil([&] { return rig.room().phase() == RoomPhase::Over; }, 120 * kSec));
  CHECK(rig.host->server().bots() == 0b110);

  NetClient& b = rig.addClient("late", "Late");
  REQUIRE(rig.runUntil([&] { return b.running() && b.localSlots() != 0; }, 5 * kSec));
  CHECK(b.localSlots() == 0b100);
  CHECK(rig.host->server().bots() == 0b010);
  REQUIRE(rig.runUntil([&] { return roomOf(a).humans == 0b101; }, kSec));
  CHECK(roomOf(a).phase == RoomPhase::Over);
  CHECK(roomOf(a).botPlacement == 0b010);  // Over: the bot that stayed
  CHECK(roomOf(a).maxPlayers == 3);        // v8: the cap, in every phase
  CHECK(roomOf(b).maxPlayers == 3);

  // Play again, then Start: one bot fits beside the two humans.
  a.sendStart();
  REQUIRE(rig.runUntil([&] { return roomIs(a, RoomPhase::Waiting); }, kSec));
  CHECK(rig.host->server().bots() == 0);
  // Issue #68: two bots asked for, one free slot under the cap of three.
  CHECK(roomOf(a).settings.bots == 2);
  CHECK(roomOf(a).botPlacement == 0b010);
  CHECK(roomOf(b).botPlacement == 0b010);
  CHECK(roomOf(b).maxPlayers == 3);
  a.sendStart();
  REQUIRE(rig.runUntil([&] { return roomIs(b, RoomPhase::Playing) && !b.holding(); }, 2 * kSec));
  CHECK(rig.host->server().bots() == 0b010);
  CHECK(roomOf(b).botPlacement == 0b010);
  CHECK(roomOf(b).maxPlayers == 3);
  CHECK(roomOf(b).settings.bots == 2);  // kept: bots when there is room
  rig.runFor(3 * kSec);
  CHECK(b.netStats().checksumMismatches == 0);
  CHECK(b.netStats().leadTicks > 0);
}

// v7 (issue #68): the Waiting RoomState's botPlacement is where Start puts
// the bots, under the server's player cap, re-sent on every join, leave and
// settings change; Start places exactly the previewed slots. A cap of four
// with three bots asked for: the client's old copy of the rule ignored the
// cap and showed a bot in slots the server does not have.
TEST_CASE("The room flow: the waiting room previews the bots' placement under the player cap",
          "[server][room][flow]") {
  CHECK(server::roomBotSlots(0b11'1111'1100, 3) == 0b1'1100);
  CHECK(server::roomBotSlots(0b1010, 3) == 0b1010);  // fewer free than asked for
  CHECK(server::roomBotSlots(0b1010, 0) == 0);
  CHECK(server::roomBotSlots(0, 3) == 0);

  server::HostConfig cfg;
  cfg.room = true;
  cfg.server.maxPlayers = 4;
  cfg.bots = server::referenceBots(3);
  RoomRig rig(cfg);
  NetClient& a = rig.addClient("one", "One");
  REQUIRE(rig.runUntil([&] { return a.running() && roomOf(a).humans == 0b1; }, 5 * kSec));
  CHECK(roomOf(a).settings.bots == 3);
  CHECK(roomOf(a).botPlacement == 0b1110);
  CHECK(roomOf(a).maxPlayers == 4);  // v8 (issue #83): the cap the screen ends at

  // A join: two humans leave room for two of the three bots.
  NetClient& b = rig.addClient("two", "Two");
  REQUIRE(rig.runUntil([&] { return b.running() && roomOf(a).humans == 0b11; }, 5 * kSec));
  CHECK(roomOf(a).botPlacement == 0b1100);
  CHECK(roomOf(b).botPlacement == 0b1100);
  CHECK(roomOf(b).maxPlayers == 4);

  // Settings: fewer bots, then as many as before.
  a.sendRoomSettings(RoomSettings{.bots = 1});
  REQUIRE(rig.runUntil([&] { return roomOf(b).settings.bots == 1; }, kSec));
  CHECK(roomOf(b).botPlacement == 0b100);
  a.sendRoomSettings(RoomSettings{.bots = 3});
  REQUIRE(rig.runUntil([&] { return roomOf(b).settings.bots == 3; }, kSec));
  CHECK(roomOf(b).botPlacement == 0b1100);
  // v8: more bots than the cap leaves room for (cap − 1, the limit the
  // client's Bots row stops at): refused, the RoomState re-sent as it was.
  const uint32_t updates = a.roomUpdates();
  a.sendRoomSettings(RoomSettings{.bots = 4});
  REQUIRE(rig.runUntil([&] { return a.roomUpdates() > updates; }, kSec));
  CHECK(roomOf(a).settings.bots == 3);
  CHECK(roomOf(a).maxPlayers == 4);
  CHECK(rig.room().settings().bots == 3);

  // A third human: one slot is left.
  NetClient& c = rig.addClient("three", "Three");
  REQUIRE(rig.runUntil([&] { return c.running() && roomOf(a).humans == 0b111; }, 5 * kSec));
  CHECK(roomOf(a).botPlacement == 0b1000);

  // A leave: the gap is the lowest free slot again.
  b.disconnect();
  REQUIRE(
      rig.runUntil([&] { return roomOf(a).humans == 0b101 && roomOf(c).humans == 0b101; }, kSec));
  const sim::PlayerMask preview = roomOf(a).botPlacement;
  CHECK(preview == 0b1010);
  CHECK(roomOf(c).botPlacement == preview);

  // Start places exactly the previewed slots; Playing shows where they are.
  a.sendStart();
  REQUIRE(rig.runUntil(
      [&] { return roomIs(a, RoomPhase::Playing) && roomIs(c, RoomPhase::Playing); }, kSec));
  CHECK(rig.host->server().bots() == preview);
  CHECK(roomOf(a).bots == preview);
  CHECK(roomOf(a).botPlacement == preview);
  CHECK(roomOf(c).botPlacement == preview);
  CHECK(roomOf(c).maxPlayers == 4);
}
