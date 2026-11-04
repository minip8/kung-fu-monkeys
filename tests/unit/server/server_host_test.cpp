// ServerHost over the loopback emulator (apps/server): a real NetClient
// plays against server-side bots through a whole match, sees the restart and
// keeps playing; stats lines; shutdown; --exit-after-matches; the bots
// (reference bots and, with KFM_WASM_BOTS, a fixture module) writing the
// authoritative frames, re-initialised per match, delayed by the humans'
// lead. Time is a deterministic 1 ms step loop, as in tests/netcode.
#include "server_host.hpp"

#include "kfm/ai/bot_spec.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/net/loopback.hpp"
#include "kfm/net/messages.hpp"
#include "kfm/net/net_client.hpp"
#include "kfm/net/netsim.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/scripted_inputs.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
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

// A hand-driven client on a zero-latency link: joins with one slot, then
// every tick sends an idle input for nextTick + margin acknowledging the
// newest authoritative frame it holds, so the server measures its lead as
// exactly `margin` ticks (rtt 0).
struct FakeHuman {
  LoopbackTransport* t{nullptr};
  int32_t margin{0};
  bool welcomed{false};
  std::optional<Tick> newestAuth;
  std::optional<Tick> sentFor;  // the server tick of the last input sent

  void hello(const sim::ContentDB& content) const {
    HelloMsg h;
    h.contentHash = sim::contentHash(content);
    h.requestedSlots = 1;
    std::vector<std::byte> buf(kMaxReliableBytes);
    const size_t n = encode(h, buf);
    REQUIRE(n > 0);
    t->sendReliable(kServerPeer, {buf.data(), n});
  }
  void poll(uint64_t now, Tick serverNext) {
    ConnectionEvents ev;
    t->poll(now, ev);
    Packet p;
    while (t->recv(p)) {
      const auto type = peekType(p.data);
      if (!type) continue;
      WelcomeMsg w;
      if (*type == MessageType::Welcome && decode(p.data, w)) {
        welcomed = true;
        mySlots = w.assignedSlots;
      }
      AuthInputsMsg a;
      if (*type == MessageType::AuthInputs && decode(p.data, a)) {
        const Tick end = a.endTick().plus(-1);
        if (!newestAuth || end > *newestAuth) newestAuth = end;
      }
    }
    if (!welcomed || !newestAuth || sentFor == serverNext) return;
    sentFor = serverNext;
    InputMsg m;
    m.startTick = serverNext.plus(margin);
    m.count = 1;
    m.lastAuthTick = newestAuth;
    m.slots = mySlots;
    std::array<std::byte, kMaxUnreliableBytes> buf{};
    const size_t n = encode(m, buf);
    if (n > 0) t->sendUnreliable(kServerPeer, {buf.data(), n});
  }
  sim::PlayerMask mySlots{0};
};

struct Rig {
  explicit Rig(server::HostConfig config, const sim::ContentDB& db = sim::defaultContent())
      : content(db), net(77) {
    config.log = [this](std::string_view line) { lines.emplace_back(line); };
    auto created = server::ServerHost::create(net.server(), content, std::move(config), now);
    if (!created) FAIL("ServerHost::create: " << created.error());
    host = std::move(*created);
  }

  FakeHuman& addFakeHuman(int32_t margin) {
    fakes.push_back(std::make_unique<FakeHuman>());
    FakeHuman& f = *fakes.back();
    f.t = &net.connectClient(now);
    f.margin = margin;
    step();
    f.hello(content);
    return f;
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
    for (auto& f : fakes)
      f->poll(now, host->server().nextTick());
    record();
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

  // Every authoritative frame since the last call.
  void record() {
    const std::optional<Tick> newest = host->server().frames().newest();
    if (!newest) return;
    Tick t = recorded.empty() ? *newest : recorded.back().tick.plus(1);
    for (; t <= *newest; t = t.plus(1)) {
      if (const sim::InputFrame* f = host->server().frames().get(t)) recorded.push_back(*f);
    }
  }
  [[nodiscard]] const sim::InputFrame* recordedAt(Tick t) const {
    for (const sim::InputFrame& f : std::views::reverse(recorded))
      if (f.tick == t) return &f;
    return nullptr;
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
  std::vector<std::unique_ptr<FakeHuman>> fakes;
  std::vector<sim::InputFrame> recorded;
};

[[nodiscard]] server::HostConfig botConfig(uint8_t bots) {
  server::HostConfig c;
  c.server.match.seed = 0xAB;
  c.bots = server::referenceBots(bots);
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
  CHECK_FALSE(w.rules.hasModeCounters());  // a fresh match: nothing carried over
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

TEST_CASE("ServerHost's reference bots throw and dash in the authoritative frames",
          "[server][bots]") {
  server::HostConfig cfg = botConfig(3);
  cfg.statsEveryNs = kSec;
  Rig rig(cfg);
  CHECK(rig.host->server().bots() == 0b111);
  for (uint8_t s = 0; s < 3; ++s) {
    const std::optional<ai::BotSpec> spec = rig.host->botSpec(PlayerId{s});
    CHECK(spec == ai::BotSpec{});  // ref:hard
  }
  CHECK_FALSE(rig.host->botSpec(PlayerId{3}).has_value());
  CHECK(rig.linesWith("bot_specs=ref:hard,ref:hard,ref:hard bot_delay=auto") == 1);
  rig.runFor(20 * kSec);
  std::array<uint32_t, 3> throws{};
  std::array<uint32_t, 3> dashes{};
  std::array<uint32_t, 3> moving{};
  for (size_t i = 1; i < rig.recorded.size(); ++i) {
    for (uint8_t s = 0; s < 3; ++s) {
      const sim::PlayerInput& in = rig.recorded[i].inputs[s];
      const sim::PlayerInput& was = rig.recorded[i - 1].inputs[s];
      const auto pressed = [&](sim::Button b) {
        const auto bit = static_cast<uint8_t>(b);
        return (in.buttons & bit) != 0 && (was.buttons & bit) == 0;
      };
      if (pressed(sim::Button::Throw)) ++throws[s];
      if (pressed(sim::Button::Dash)) ++dashes[s];
      if (in.moveMag != 0) ++moving[s];
    }
  }
  for (uint8_t s = 0; s < 3; ++s) {
    INFO("slot " << int{s});
    CHECK(throws[s] > 0);
    CHECK(dashes[s] > 0);
    CHECK(moving[s] > 60);
  }
  CHECK(rig.host->botDelayTicks() == 0);  // bot versus bot: no delay
  const auto it = std::ranges::find_if(std::views::reverse(rig.lines),
                                       [](const std::string& l) { return l.contains("] stats "); });
  REQUIRE(it != std::views::reverse(rig.lines).end());
  INFO(*it);
  // Every bot plays every tick of the last second, or sits it out eliminated
  // (v0.35: paused=+P, printed when non-zero).
  CHECK(it->contains(" bot_delay=0 | bot 0 ref:hard acts=+"));
  for (uint8_t s = 0; s < 3; ++s) {
    INFO("slot " << int{s});
    const std::string tag = std::format(" | bot {} ref:hard acts=+", s);
    const size_t at = it->find(tag);
    REQUIRE(at != std::string::npos);
    const std::string rest = it->substr(at + tag.size());
    const uint32_t acts = static_cast<uint32_t>(std::stoul(rest));
    uint32_t paused = 0;
    const size_t next = rest.find(" | ");
    const size_t p = rest.find(" paused=+");
    if (p != std::string::npos && (next == std::string::npos || p < next)) {
      paused = static_cast<uint32_t>(std::stoul(rest.substr(p + 9)));
    }
    CHECK(acts + paused == 60);
    CHECK(rest.contains(" subst=+0"));
  }
}

TEST_CASE("ServerHost's bots take the humans' lead as their delay, once a second",
          "[server][bots][delay]") {
  SECTION("auto: the median lead of the peers holding slots") {
    Rig rig(botConfig(2));
    FakeHuman& h = rig.addFakeHuman(4);
    REQUIRE(rig.runUntil([&] { return h.welcomed && h.mySlots != 0; }, kSec));
    CHECK(h.mySlots == 0b100);
    rig.runFor(3 * kSec);
    CHECK(rig.host->server().humanLeadTicks() == 4);
    CHECK(rig.host->botDelayTicks() == 4);
    // The second fake human's lead is 7: the lower median of 4 and 7 is 4;
    // with a third at 9 the median is 7.
    rig.addFakeHuman(7);
    rig.runFor(3 * kSec);
    CHECK(rig.host->botDelayTicks() == 4);
    rig.addFakeHuman(9);
    rig.runFor(3 * kSec);
    CHECK(rig.host->server().humanLeadTicks() == 7);
    CHECK(rig.host->botDelayTicks() == 7);
  }
  SECTION("a fixed delay ignores the lead") {
    server::HostConfig cfg = botConfig(1);
    cfg.botDelayTicks = 2;
    Rig rig(cfg);
    CHECK(rig.host->botDelayTicks() == 2);
    rig.addFakeHuman(6);
    rig.runFor(3 * kSec);
    CHECK(rig.host->server().humanLeadTicks() == 6);
    CHECK(rig.host->botDelayTicks() == 2);
  }
  SECTION("a real client over a 40 ms link") {
    Rig rig(botConfig(2));
    NetClient& c = rig.addClient("human");
    REQUIRE(rig.runUntil([&] { return c.running(); }, 3 * kSec));
    rig.runFor(5 * kSec);
    // lead ≈ one-way 40 ms (2.4 ticks) + a margin of 1..2 ticks.
    const uint8_t lead = rig.host->server().humanLeadTicks();
    CHECK(lead >= 3);
    CHECK(lead <= 6);
    CHECK(rig.host->botDelayTicks() == lead);
    CHECK(c.netStats().checksumMismatches == 0);
  }
}

TEST_CASE("ServerHost refuses a bot module that does not load", "[server][bots]") {
  server::HostConfig cfg = botConfig(1);
  cfg.bots.push_back(
      ai::BotSpec{ai::BotKind::Module, ai::ReferenceTier::Hard, "/nonexistent/x.wasm"});
  LoopbackNetwork net(3);
  auto created = server::ServerHost::create(net.server(), sim::defaultContent(), cfg, 0);
  REQUIRE_FALSE(created.has_value());
  CHECK(created.error().contains("/nonexistent/x.wasm"));
}

#ifdef KFM_SERVER_TEST_WASM

namespace {

[[nodiscard]] ai::BotSpec fixture(std::string_view name) {
  return ai::BotSpec{ai::BotKind::Module, ai::ReferenceTier::Hard,
                     std::string(KFM_BOT_FIXTURES_DIR) + "/" + std::string(name)};
}

}  // namespace

TEST_CASE("ServerHost runs a wasm bot module, its inputs delayed by the humans' lead",
          "[server][bots][wasm]") {
  // echo.wasm answers aimDir = the observed tick's low byte, so the frame of
  // tick T carries (T − delay) & 255.
  // An idle module in slot 2, not a reference bot: nobody is eliminated, so
  // no slot is ever paused (v0.35) and every frame carries the echo.
  server::HostConfig cfg = botConfig(0);
  cfg.bots = {fixture("echo.wasm"), fixture("echo.wasm"), fixture("idle.wasm")};
  cfg.statsEveryNs = kSec;
  Rig rig(cfg);
  CHECK(rig.linesWith("bot_specs=echo.wasm,echo.wasm,idle.wasm") == 1);
  rig.runFor(2 * kSec);
  const auto lagOf = [&](const sim::InputFrame& f, uint8_t slot) {
    return static_cast<uint8_t>((f.tick.value - f.inputs[slot].aimDir) & 0xFFu);
  };
  CHECK(lagOf(rig.recorded.back(), 0) == 0);  // bot versus bot
  CHECK(rig.recorded.back().inputs[0].moveMag == 255);
  CHECK(rig.recorded.back().inputs[1].moveDir == 64);

  rig.addFakeHuman(5);
  rig.runFor(3 * kSec);
  REQUIRE(rig.host->botDelayTicks() == 5);
  for (size_t i = rig.recorded.size() - 30; i < rig.recorded.size(); ++i) {
    CHECK(lagOf(rig.recorded[i], 0) == 5);
    CHECK(lagOf(rig.recorded[i], 1) == 5);
  }
  const auto it = std::ranges::find_if(std::views::reverse(rig.lines),
                                       [](const std::string& l) { return l.contains("] stats "); });
  REQUIRE(it != std::views::reverse(rig.lines).end());
  INFO(*it);
  CHECK(
      it->contains(" bot_delay=5 | bot 0 echo.wasm acts=+60 subst=+0 traps=+0 over_budget=+0 "
                   "fuel_mean="));
  CHECK(it->contains(" | bot 2 idle.wasm acts=+60 subst=+0"));
}

TEST_CASE("ServerHost re-initialises its wasm bots with the next match's seeds",
          "[server][bots][wasm][restart]") {
  // counter.wasm answers moveDir = its act count since kfm_init (a fresh
  // instance counts from 1), moveMag = its slot, aimDir = its seed's low byte.
  server::HostConfig cfg = botConfig(3);
  cfg.bots.push_back(fixture("counter.wasm"));
  cfg.bots.push_back(fixture("counter.wasm"));
  cfg.server.botSeed = 0x1234;
  Rig rig(cfg);
  rig.runFor(kSec);
  const sim::InputFrame* first = rig.recordedAt(Tick{0});
  if (first == nullptr) {
    FAIL("no frame 0");
    return;
  }
  for (uint8_t s = 3; s < 5; ++s) {
    INFO("slot " << int{s});
    CHECK(first->inputs[s].moveDir == 1);
    CHECK(first->inputs[s].moveMag == s);
    CHECK(first->inputs[s].aimDir == (server::agentSeed(0x1234, PlayerId{s}) & 0xFFu));
  }
  // One act per tick while its monkey stands (v0.35: an eliminated slot is
  // paused, and the reference bots get the first counter after tick 200).
  const sim::InputFrame* early = rig.recordedAt(Tick{50});
  if (early == nullptr) {
    FAIL("no frame 50");
    return;
  }
  CHECK(early->inputs[3].moveDir == 51);

  REQUIRE(rig.runUntil([&] { return rig.host->server().matchIndex() == 1; }, 900 * kSec));
  const uint64_t seed2 = server::nextMatchSeed(0x1234);
  CHECK(rig.host->server().botSeed() == seed2);
  const sim::InputFrame& start = rig.recorded.back();  // the restart tick: match 2's first
  const sim::InputFrame* before = rig.recordedAt(start.tick.plus(-1));
  if (before == nullptr) {
    FAIL("no frame before the restart");
    return;
  }
  for (uint8_t s = 3; s < 5; ++s) {
    INFO("slot " << int{s});
    CHECK(before->inputs[s].moveDir != 1);
    CHECK(start.inputs[s].moveDir == 1);  // a fresh instance
    CHECK(start.inputs[s].aimDir == (server::agentSeed(seed2, PlayerId{s}) & 0xFFu));
  }
  CHECK(server::agentSeed(seed2, PlayerId{3}) != server::agentSeed(0x1234, PlayerId{3}));
}

TEST_CASE("ServerHost counts a trapping module's substitutions and refuses a bad one",
          "[server][bots][wasm]") {
  server::HostConfig cfg = botConfig(0);
  cfg.bots = {fixture("trap.wasm")};
  cfg.statsEveryNs = kSec;
  Rig rig(cfg);
  rig.runFor(2 * kSec);
  const auto it = std::ranges::find_if(std::views::reverse(rig.lines),
                                       [](const std::string& l) { return l.contains("] stats "); });
  REQUIRE(it != std::views::reverse(rig.lines).end());
  INFO(*it);
  CHECK(it->contains("bot 0 trap.wasm acts=+60 subst=+60 traps=+60 over_budget=+0"));

  cfg.bots = {fixture("abi_mismatch.wasm")};
  LoopbackNetwork net(3);
  auto created = server::ServerHost::create(net.server(), sim::defaultContent(), cfg, 0);
  REQUIRE_FALSE(created.has_value());
  CHECK(created.error().contains("abi_mismatch.wasm"));
}

#endif
