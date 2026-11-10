// The local client's bots (architecture §8): MatchSession's bot slots are
// agents in an ai::AgentHost (the reference bot at the lobby's tier, or a
// .wasm module natively), plus the flags that pick them.
#include "core/bot_roster.hpp"
#include "core/client_options.hpp"
#include "core/match_session.hpp"
#include "kfm/ai/bot_spec.hpp"
#include "kfm/platform/input_event.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace kfm;
using client::MatchSession;
using client::SessionConfig;
using client::SlotOwner;

namespace {

constexpr uint64_t kT0 = 1'000'000'000ULL;

[[nodiscard]] constexpr uint64_t timeAtTick(uint64_t base, uint64_t k) {
  return base + ((k * 1'000'000'000ULL) + 59) / 60;
}

// A session stepped one tick per frame on an exact clock, counted from the
// latest (re)start so rounding never skips a tick.
struct Bots {
  std::unique_ptr<MatchSession> s;
  uint64_t base{kT0};
  uint64_t ticks{0};

  explicit Bots(const SessionConfig& c) : s(std::make_unique<MatchSession>(c)) {}
  [[nodiscard]] uint64_t now() const { return timeAtTick(base, ticks); }
  void rebase() {
    base = now();
    ticks = 0;
  }
  void start() {
    REQUIRE(s->startMatch(now()));
    rebase();
  }
  void tick() {
    ++ticks;
    s->advance(now());
  }
  void restart() {
    s->restart(now(), false);
    rebase();
  }
};

[[nodiscard]] SessionConfig botsOnly(uint8_t bots, ai::ReferenceTier tier) {
  return SessionConfig{.seed = 7,
                       .botSeed = 0xB07,
                       .bots = bots,
                       .botSpec = {.kind = ai::BotKind::Reference, .tier = tier, .path = {}},
                       .bindKeyboardToSlot0 = false};
}

// The first tick on which any slot of `mask` presses a button edge, or -1.
[[nodiscard]] int firstPress(Bots& b, sim::PlayerMask mask, int maxTicks) {
  std::array<uint8_t, sim::kMaxPlayers> prev{};
  for (int t = 0; t < maxTicks; ++t) {
    b.tick();
    const sim::InputFrame& f = b.s->lastFrame();
    for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
      if ((mask & (1u << i)) == 0) continue;
      const uint8_t edges = static_cast<uint8_t>(f.inputs[i].buttons & ~prev[i]);
      prev[i] = f.inputs[i].buttons;
      if (edges != 0) return t;
    }
  }
  return -1;
}

[[nodiscard]] bool parse(std::vector<const char*> args, client::ClientOptions& o,
                         std::string& error) {
  args.insert(args.begin(), "kfm_client");
  return client::parseClientOptions(std::span<const char* const>(args), o, error);
}

}  // namespace

TEST_CASE("bots: the casual default is the medium reference bot", "[client][bots]") {
  const MatchSession s{SessionConfig{}};
  CHECK(s.botSpec().kind == ai::BotKind::Reference);
  CHECK(s.botSpec().tier == ai::ReferenceTier::Medium);
  CHECK(s.botRoster().describe() == "ref:medium");
  CHECK(s.botSpecError().empty());
  CHECK(client::ClientOptions{}.botDifficulty == ai::ReferenceTier::Medium);
}

TEST_CASE("bots: a four-bot match throws, dashes and ends a round", "[client][bots]") {
  Bots b(botsOnly(4, ai::ReferenceTier::Hard));
  b.start();
  REQUIRE(b.s->botMask() == 0b1111);
  REQUIRE(b.s->localMask() == 0);
  for (uint8_t i = 0; i < 4; ++i)
    CHECK(b.s->agents().has(PlayerId{i}));
  CHECK_FALSE(b.s->agents().has(PlayerId{4}));

  uint32_t throws = 0;
  uint32_t dashes = 0;
  bool roundEnded = false;
  for (int t = 0; t < 60 * 60 && !roundEnded; ++t) {
    b.tick();
    for (const sim::SimEvent& e : b.s->frameEvents()) {
      if (e.kind == sim::EventKind::BoomerangThrown) ++throws;
      if (e.kind == sim::EventKind::PlayerDashed) ++dashes;
      if (e.kind == sim::EventKind::RoundEnded) roundEnded = true;
    }
  }
  CHECK(throws > 0);
  CHECK(dashes > 0);
  CHECK(roundEnded);
  const client::BotSlotStats st = b.s->botStats(PlayerId{0});
  // One act per tick, but for the ticks it sat out eliminated (v0.35).
  CHECK(st.acts + st.paused == b.s->curr().tick.value);
  CHECK(st.substitutions == 0);
  CHECK_FALSE(st.module);
  CHECK(b.s->botStats(PlayerId{5}).acts == 0);  // not a bot slot
}

TEST_CASE("bots: the same seeds give the same match", "[client][bots]") {
  Bots a(botsOnly(5, ai::ReferenceTier::Medium));
  Bots b(botsOnly(5, ai::ReferenceTier::Medium));
  a.start();
  b.start();
  for (int t = 0; t < 900; ++t) {
    a.tick();
    b.tick();
    REQUIRE(a.s->checksum() == b.s->checksum());
  }
  // Another bot seed plays another match.
  SessionConfig other = botsOnly(5, ai::ReferenceTier::Medium);
  other.botSeed = 0xD1FF;
  Bots c(other);
  c.start();
  for (int t = 0; t < 900; ++t)
    c.tick();
  CHECK(c.s->checksum() != a.s->checksum());
}

TEST_CASE("bots: difficulty tiers differ", "[client][bots]") {
  Bots easy(botsOnly(3, ai::ReferenceTier::Easy));
  Bots hard(botsOnly(3, ai::ReferenceTier::Hard));
  easy.start();
  hard.start();
  const int easyFirst = firstPress(easy, 0b111, 1200);
  const int hardFirst = firstPress(hard, 0b111, 1200);
  REQUIRE(hardFirst >= 0);
  REQUIRE(easyFirst >= 0);
  // Easy reacts kMaxReactionDelay-ish ticks late and hesitates to take its shots.
  CHECK(easyFirst > hardFirst);

  // setBotTier() applies at the next start: the same seeds, another match.
  Bots tiered(botsOnly(3, ai::ReferenceTier::Easy));
  tiered.s->setBotTier(ai::ReferenceTier::Hard);
  CHECK(tiered.s->botSpec().tier == ai::ReferenceTier::Hard);
  tiered.start();
  CHECK(firstPress(tiered, 0b111, 1200) == hardFirst);
}

TEST_CASE("bots: a restart re-initialises the agents", "[client][bots]") {
  Bots b(botsOnly(3, ai::ReferenceTier::Medium));
  b.start();
  std::vector<uint32_t> first;
  for (int t = 0; t < 400; ++t) {
    b.tick();
    first.push_back(b.s->checksum());
  }
  CHECK(b.s->botStats(PlayerId{1}).acts + b.s->botStats(PlayerId{1}).paused == 400);
  b.restart();
  CHECK(b.s->botStats(PlayerId{1}).acts == 0);
  for (int t = 0; t < 400; ++t) {
    b.tick();
    REQUIRE(b.s->checksum() == first[static_cast<std::size_t>(t)]);
  }
  // Back in the lobby no agent is left; the next start attaches fresh ones.
  b.s->returnToLobby();
  CHECK(b.s->agents().slots() == 0);
  b.start();
  CHECK(b.s->agents().slots() == 0b111);
}

TEST_CASE("bots: a dropped pad's slot becomes a bot on restart", "[client][bots]") {
  const SessionConfig cfg = botsOnly(1, ai::ReferenceTier::Hard);
  Bots b(cfg);
  platform::InputEvent e{};
  e.kind = platform::InputEventKind::PadAdded;
  e.deviceId = 9;
  b.s->push(e);
  e.kind = platform::InputEventKind::PadButtonDown;
  e.button = platform::PadButton::South;
  e.timestampNs = b.now();
  b.s->push(e);
  b.tick();
  REQUIRE(b.s->sampler().isBound(PlayerId{0}));
  b.start();
  REQUIRE(b.s->botMask() == 0b10);
  CHECK_FALSE(b.s->agents().has(PlayerId{0}));
  b.tick();
  e = platform::InputEvent{};
  e.kind = platform::InputEventKind::PadRemoved;
  e.deviceId = 9;
  b.s->push(e);
  b.tick();
  REQUIRE(b.s->owner(PlayerId{0}) == SlotOwner::Dropped);
  CHECK_FALSE(b.s->agents().has(PlayerId{0}));

  b.restart();
  CHECK(b.s->owner(PlayerId{0}) == SlotOwner::Bot);
  CHECK(b.s->agents().slots() == 0b11);
  bool moved = false;
  for (int t = 0; t < 300 && !moved; ++t) {
    b.tick();
    moved = b.s->lastFrame().inputs[0].moveMag != 0 || b.s->lastFrame().inputs[0].buttons != 0;
  }
  CHECK(moved);
  CHECK(b.s->botStats(PlayerId{0}).acts > 0);
}

TEST_CASE("bots: a module that cannot be loaded falls back to the reference bot",
          "[client][bots]") {
  SessionConfig cfg = botsOnly(2, ai::ReferenceTier::Easy);
  cfg.botSpec = {.kind = ai::BotKind::Module,
                 .tier = ai::ReferenceTier::Easy,
                 .path = "/nonexistent/bot.wasm"};
  Bots b(cfg);
  CHECK_FALSE(b.s->botSpecError().empty());
  CHECK_FALSE(b.s->botRoster().moduleActive());
  CHECK(b.s->botRoster().describe() == "ref:easy");
  b.start();
  for (int t = 0; t < 120; ++t)
    b.tick();
  const client::BotSlotStats st = b.s->botStats(PlayerId{0});
  CHECK(st.acts == 120);
  CHECK_FALSE(st.module);
  // A reference spec clears the error.
  CHECK(b.s->setBotSpec(ai::BotSpec{}));
  CHECK(b.s->botSpecError().empty());
}

#ifdef KFM_CLIENT_WASM_BOTS
TEST_CASE("bots: a wasm module plays as local bots", "[client][bots][wasm]") {
  REQUIRE(client::moduleBotsSupported());
  const std::string dir = KFM_BOT_FIXTURES_DIR;
  SECTION("echo.wasm acts every tick") {
    SessionConfig cfg = botsOnly(3, ai::ReferenceTier::Medium);
    cfg.botSpec = {
        .kind = ai::BotKind::Module, .tier = ai::ReferenceTier::Medium, .path = dir + "/echo.wasm"};
    Bots b(cfg);
    REQUIRE(b.s->botSpecError().empty());
    REQUIRE(b.s->botRoster().moduleActive());
    CHECK(b.s->botRoster().describe() == dir + "/echo.wasm");
    b.start();
    for (int t = 0; t < 120; ++t)
      b.tick();
    for (uint8_t i = 0; i < 3; ++i) {
      const client::BotSlotStats st = b.s->botStats(PlayerId{i});
      CHECK(st.module);
      CHECK(st.alive);
      CHECK(st.acts == 120);
      CHECK(st.substitutions == 0);
      CHECK(st.traps == 0);
      CHECK(st.fuelMax > 0);
    }
    // echo: stick at 64 full tilt, aim = tick & 255.
    const sim::PlayerInput in = b.s->lastFrame().inputs[1];
    CHECK(in.moveDir == 64);
    CHECK(in.moveMag == 255);
    CHECK(in.aimDir == static_cast<uint8_t>(b.s->lastFrame().tick.value & 255u));
    // A restart gives every slot a fresh instance.
    b.restart();
    CHECK(b.s->botStats(PlayerId{0}).acts == 0);
    b.tick();
    CHECK(b.s->botStats(PlayerId{0}).acts == 1);
  }
  SECTION("trap.wasm: every act is substituted and the match keeps stepping") {
    SessionConfig cfg = botsOnly(2, ai::ReferenceTier::Medium);
    cfg.botSpec = {
        .kind = ai::BotKind::Module, .tier = ai::ReferenceTier::Medium, .path = dir + "/trap.wasm"};
    Bots b(cfg);
    REQUIRE(b.s->botRoster().moduleActive());
    b.start();
    for (int t = 0; t < 90; ++t)
      b.tick();
    CHECK(b.s->curr().tick.value == 90);
    const client::BotSlotStats st = b.s->botStats(PlayerId{1});
    CHECK(st.module);
    CHECK(st.acts == 90);
    CHECK(st.substitutions == st.acts);
    CHECK(st.traps == st.acts);
    CHECK(b.s->lastFrame().inputs[1] == sim::PlayerInput{});  // repeat-last of nothing: idle
  }
  SECTION("a file that is not a bot module falls back") {
    SessionConfig cfg = botsOnly(1, ai::ReferenceTier::Hard);
    cfg.botSpec = {
        .kind = ai::BotKind::Module, .tier = ai::ReferenceTier::Hard, .path = dir + "/no_act.wasm"};
    Bots b(cfg);
    CHECK_FALSE(b.s->botRoster().moduleActive());
    CHECK(b.s->botSpecError().find("refused") != std::string::npos);
    CHECK(b.s->botRoster().describe() == "ref:hard");
  }
}
#else
TEST_CASE("bots: without the wasm runtime a module spec falls back", "[client][bots]") {
  CHECK_FALSE(client::moduleBotsSupported());
  SessionConfig cfg = botsOnly(1, ai::ReferenceTier::Hard);
  cfg.botSpec = {.kind = ai::BotKind::Module, .tier = ai::ReferenceTier::Hard, .path = "x.wasm"};
  const Bots b(cfg);
  CHECK(b.s->botSpecError().find("not supported") != std::string::npos);
  CHECK(b.s->botRoster().describe() == "ref:hard");
}
#endif

TEST_CASE("client flags: --bot-difficulty, --bot-module and --bot", "[client][bots][options]") {
  std::string error;
  {
    client::ClientOptions o;
    REQUIRE(parse({"--bot-difficulty", "hard"}, o, error));
    CHECK(o.botDifficulty == ai::ReferenceTier::Hard);
    CHECK(o.botModule.empty());
    const ai::BotSpec spec = client::botSpec(o);
    CHECK(spec.kind == ai::BotKind::Reference);
    CHECK(spec.tier == ai::ReferenceTier::Hard);
  }
  {
    client::ClientOptions o;
    REQUIRE(parse({"--bot-module", "bots/mine.wasm", "--bot-difficulty", "easy"}, o, error));
    const ai::BotSpec spec = client::botSpec(o);
    CHECK(spec.kind == ai::BotKind::Module);
    CHECK(spec.path == "bots/mine.wasm");
    CHECK(spec.tier == ai::ReferenceTier::Easy);  // the fallback's tier
  }
  {
    // The browser page turns ?bots=3&bot=easy into these.
    client::ClientOptions o;
    REQUIRE(parse({"--bots", "3", "--bot", "easy"}, o, error));
    CHECK(o.bots == 3);
    CHECK(o.botDifficulty == ai::ReferenceTier::Easy);
    REQUIRE(parse({"--bot", "ref:hard"}, o, error));
    CHECK(o.botDifficulty == ai::ReferenceTier::Hard);
    REQUIRE(parse({"--bot", "a.wasm"}, o, error));
    CHECK(o.botModule == "a.wasm");
    REQUIRE(parse({"--bot", "medium"}, o, error));  // a tier drops the module
    CHECK(o.botModule.empty());
    CHECK(o.botDifficulty == ai::ReferenceTier::Medium);
  }
  client::ClientOptions bad;
  CHECK_FALSE(parse({"--bot-difficulty", "insane"}, bad, error));
  CHECK(error.find("easy, medium or hard") != std::string::npos);
  CHECK_FALSE(parse({"--bot-module", "bot.so"}, bad, error));
  CHECK(error.find(".wasm") != std::string::npos);
  CHECK_FALSE(parse({"--bot", "ref:nightmare"}, bad, error));
  CHECK_FALSE(parse({"--bot-module"}, bad, error));
  CHECK(error == "--bot-module needs a value");
}
