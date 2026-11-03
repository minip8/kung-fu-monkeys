// ServerHost over the loopback emulator (apps/server): a real NetClient
// plays against server-side bots through a whole match, sees the restart and
// keeps playing; stats lines; shutdown; --exit-after-matches. Time is a
// deterministic 1 ms step loop, as in tests/netcode.
#include "server_host.hpp"

#include "kfm/core/rng.hpp"
#include "kfm/net/loopback.hpp"
#include "kfm/net/net_client.hpp"
#include "kfm/net/netsim.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/scripted_inputs.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace kfm;
using namespace kfm::net;
using kfm::net::netsim::kMs;
using kfm::net::netsim::kSec;

namespace {

struct Rig {
  explicit Rig(server::HostConfig config, const sim::ContentDB& db = sim::defaultContent())
      : content(db), net(77) {
    config.log = [this](std::string_view line) { lines.emplace_back(line); };
    host = std::make_unique<server::ServerHost>(net.server(), content, std::move(config), now);
  }

  NetClient& addClient(const std::string& token) {
    const LinkConditions l = netsim::link(40 * kMs, 5 * kMs, 10'000);
    LoopbackTransport& t = net.connectClient(now, l, l);
    clients.push_back(std::make_unique<NetClient>(content, netsim::tokenConfig(token)));
    clients.back()->connect(t, now);
    rngs.push_back(Rng::seeded(0xFEED + clients.size()));
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

  sim::ContentDB content;  // before host and clients, which keep a reference
  uint64_t now{0};
  LoopbackNetwork net;
  std::vector<std::string> lines;
  std::unique_ptr<server::ServerHost> host;
  std::vector<std::unique_ptr<NetClient>> clients;
  std::vector<Rng> rngs;
};

[[nodiscard]] server::HostConfig botConfig(uint8_t bots) {
  server::HostConfig c;
  c.server.match.seed = 0xAB;
  c.bots = bots;
  c.restartDelayTicks = 2 * sim::kTickHz;
  return c;
}

}  // namespace

TEST_CASE("ServerHost ticks at exactly 60 Hz on its own clock", "[server]") {
  Rig rig(botConfig(0));
  CHECK(rig.host->nsUntilNextTick(rig.now) == 0);  // tick 0 is due at start
  rig.runFor(10 * kSec);
  // Ticks 0..600 are due in [0, 10 s]; the loop stops right at 10 s.
  CHECK(rig.host->stats().ticks == 601);
  CHECK(rig.host->server().nextTick() == Tick{601});
  CHECK(rig.host->stats().droppedTicks == 0);
  CHECK(rig.host->nsUntilNextTick(rig.now) == 10 * kSec + kSec / 60 - rig.now);

  // A one-second stall runs at most maxCatchUpTicks and drops the rest.
  rig.now += kSec;
  rig.host->frame(rig.now);
  CHECK(rig.host->stats().ticks == 609);
  CHECK(rig.host->stats().droppedTicks > 40);
  CHECK(rig.linesWith("clock stalled") == 1);
}

TEST_CASE("ServerHost plays a match with bots to the end, restarts it and keeps playing",
          "[server][restart]") {
  Rig rig(botConfig(3));
  CHECK(rig.host->server().bots() == 0b111);
  NetClient& c = rig.addClient("human");
  REQUIRE(rig.runUntil([&] { return c.running(); }, 3 * kSec));
  CHECK(c.localSlots() == 0b1000);  // bots took the lowest slots
  CHECK(rig.linesWith("join peer=1 slots=3") == 1);

  // Default content: first to three rounds.
  REQUIRE(rig.runUntil([&] { return rig.host->stats().matchesCompleted == 1; }, 600 * kSec));
  const uint64_t oldSeed = 0xAB;
  CHECK(rig.host->server().pendingRestart().has_value());
  CHECK(rig.linesWith("match 1 won by slot") == 1);
  CHECK(rig.linesWith("next match seed=") == 1);
  CHECK(rig.host->stats().roundsCompleted >= 3);

  REQUIRE(rig.runUntil([&] { return c.netStats().matchRestarts == 1; }, 5 * kSec));
  CHECK(rig.host->server().matchIndex() == 1);
  CHECK(rig.host->server().matchConfig().seed == server::nextMatchSeed(oldSeed));
  CHECK(c.matchConfig().seed == server::nextMatchSeed(oldSeed));
  CHECK(rig.linesWith("match 2 starts") == 1);
  CHECK(rig.host->server().present() == 0b1111);  // same roster

  rig.runFor(10 * kSec);
  CHECK(c.running());
  CHECK(c.netStats().checksumMismatches == 0);
  CHECK(c.netStats().resyncRequests == 0);
  CHECK(c.netStats().checksumsCompared > 100);
  const sim::WorldState& w = rig.host->server().world();
  CHECK(w.rules.phase != sim::MatchPhase::MatchEnded);
  CHECK(w.rules.round >= 1);
  CHECK(!rig.host->done());
}

TEST_CASE("ServerHost restarts a deathmatch and a survival match the same way",
          "[server][restart][modes]") {
  // Short modes so a match ends within seconds of simulated time.
  sim::ContentDB db = sim::defaultContent();
  sim::ModeDef& dm = db.modes[1];
  dm.name = sim::ContentName::of("deathmatch");
  dm.kind = sim::ModeKind::Deathmatch;
  dm.scoreToWin = 4;
  dm.timeLimitTicks = 40 * sim::kTickHz;
  dm.respawnTicks = 60;
  dm.respawnInvulnTicks = 30;
  sim::ModeDef& sv = db.modes[2];
  sv.name = sim::ContentName::of("survival");
  sv.kind = sim::ModeKind::Survival;
  sv.roundsToWin = 2;
  sv.timeLimitTicks = 20 * sim::kTickHz;
  db.modeCount = 3;

  const uint8_t mode = GENERATE(uint8_t{1}, uint8_t{2});
  server::HostConfig cfg = botConfig(3);
  cfg.server.match.mode = mode;
  Rig rig(cfg, db);
  NetClient& c = rig.addClient("human");
  REQUIRE(rig.runUntil([&] { return c.running(); }, 3 * kSec));
  CHECK(c.simConfig().mode == mode);

  REQUIRE(rig.runUntil([&] { return rig.host->stats().matchesCompleted == 1; }, 300 * kSec));
  CHECK(rig.linesWith("match 1 won by slot") + rig.linesWith("match 1 drawn") == 1);
  REQUIRE(rig.runUntil([&] { return c.netStats().matchRestarts == 1; }, 5 * kSec));
  CHECK(rig.host->server().matchConfig().mode == mode);  // a restart keeps the mode
  CHECK(c.simConfig().mode == mode);
  const sim::WorldState& w = rig.host->server().world();
  CHECK_FALSE(w.rules.hasModeBlock());  // a fresh match: nothing carried over
  CHECK(w.rules.scores == std::array<uint8_t, sim::kMaxPlayers>{});

  rig.runFor(10 * kSec);
  CHECK(c.running());
  CHECK(c.netStats().checksumMismatches == 0);
  CHECK(c.netStats().resyncRequests == 0);
  CHECK(rig.host->server().matchIndex() >= 1);
}

TEST_CASE("ServerHost reports done after --exit-after-matches instead of restarting",
          "[server][restart]") {
  server::HostConfig cfg = botConfig(4);
  cfg.exitAfterMatches = 1;
  Rig rig(cfg);
  REQUIRE(rig.runUntil([&] { return rig.host->done(); }, 600 * kSec));
  CHECK(rig.host->stats().matchesCompleted == 1);
  CHECK(!rig.host->server().pendingRestart());
  CHECK(rig.linesWith("exit after 1 matches") == 1);
}

TEST_CASE("ServerHost prints one stats line per period", "[server][stats]") {
  server::HostConfig cfg = botConfig(1);
  cfg.statsEveryNs = kSec;
  Rig rig(cfg);
  NetClient& c = rig.addClient("watcher");
  REQUIRE(rig.runUntil([&] { return c.running(); }, 3 * kSec));
  const size_t before = rig.linesWith("] stats ");
  rig.runFor(3 * kSec);
  CHECK(rig.linesWith("] stats ") == before + 3);
  const auto it = std::ranges::find_if(std::views::reverse(rig.lines),
                                       [](const std::string& l) { return l.contains("] stats "); });
  REQUIRE(it != std::views::reverse(rig.lines).end());
  INFO(*it);
  CHECK(it->contains("peers=1 players=1 bots=1 ticks=60 "));
  CHECK(it->contains("protocol_errors=+0"));
}

TEST_CASE("ServerHost shutdown disconnects clients with ServerShuttingDown", "[server]") {
  Rig rig(botConfig(1));
  NetClient& c = rig.addClient("a");
  REQUIRE(rig.runUntil([&] { return c.running(); }, 3 * kSec));
  rig.host->beginShutdown();
  CHECK(rig.host->shuttingDown());
  const Tick frozen = rig.host->server().nextTick();
  REQUIRE(rig.runUntil([&] { return c.state() == ClientState::Disconnected; }, kSec));
  CHECK(c.disconnectReason() == DisconnectReason::ServerShuttingDown);
  CHECK(rig.host->server().nextTick() == frozen);  // no ticks while lingering
  rig.host->finishShutdown();
  CHECK(rig.host->server().peers().empty());
  CHECK(rig.linesWith("shutting down") == 1);
  CHECK(rig.linesWith("leave peer=1") == 1);
}
