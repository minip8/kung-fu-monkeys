// kfm_arena's core (apps/arena/arena_core.hpp, architecture §8, §9): a match
// is deterministic (result line and replay bytes), reference bots finish a
// match with a winner, the placement rule and its ties, the result line and
// JSON formats, the summary, rotation (every bot through every slot, totals
// per bot), the replay runMatch writes plays back to the same final state,
// and, with KFM_WASM_BOTS, the module path over the fixtures in
// tests/fixtures/bots (fuel, traps, over-budget, load errors).
#include "arena_core.hpp"
#include "kfm/ai/bot_spec.hpp"
#include "kfm/content/app_content.hpp"
#include "kfm/core/bitstream.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/replay.hpp"
#include "kfm/sim/step.hpp"
#include "kfm/sim/world_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace kfm;
using namespace kfm::arena;

namespace {

// The embedded content/ (the CLI's default), loaded once.
const content::LoadedContent& gameContent() {
  static const content::LoadedContent loaded = [] {
    content::LoadedContent c;
    content::Diagnostics diag;
    const bool ok = content::loadAppContent("", {}, c, diag);
    REQUIRE(ok);
    return c;
  }();
  return loaded;
}

ai::BotSpec spec(const std::string& text) {
  const std::optional<ai::BotSpec> s = ai::parseBotSpec(text);
  REQUIRE(s.has_value());
  return s.value_or(ai::BotSpec{});
}

MatchSetup setupOf(const std::vector<std::string>& bots, uint64_t seed, uint32_t cap) {
  MatchSetup setup;
  setup.config.seed = seed;
  for (const std::string& b : bots)
    setup.bots.push_back(spec(b));
  setup.tickCap = cap;
  return setup;
}

MatchResult play(const MatchSetup& setup, BotFactory& factory, uint32_t index = 0) {
  auto result = runMatch(setup, gameContent().db, factory, index);
  if (!result) {
    FAIL(result.error().message);
    return {};
  }
  return std::move(*result);
}

SlotResult slotWith(uint8_t slot, uint32_t score, uint32_t deaths) {
  SlotResult s;
  s.slot = slot;
  s.botIndex = slot;
  s.bot = "ref:hard";
  s.score = score;
  s.deaths = deaths;
  return s;
}

bool contains(const std::string& text, const std::string& part) {
  return text.contains(part);
}

}  // namespace

TEST_CASE("slot seeds follow the documented formula", "[arena]") {
  STATIC_REQUIRE(slotSeed(0, 0) == 0x9E3779B97F4A7C15ULL);
  STATIC_REQUIRE(slotSeed(7, 1) == (7ULL ^ (0x9E3779B97F4A7C15ULL * 2)));
  STATIC_REQUIRE(slotSeed(7, 0) != slotSeed(7, 1));
  STATIC_REQUIRE(slotSeed(7, 0) != 7);
}

TEST_CASE("a match is deterministic: result line and replay bytes", "[arena]") {
  MatchSetup setup = setupOf({"ref:hard", "ref:medium", "ref:easy", "ref"}, 11, 2400);
  setup.recordReplay = true;
  BotFactory f1;
  BotFactory f2;
  const MatchResult a = play(setup, f1);
  const MatchResult b = play(setup, f2);
  CHECK(formatResultLine(a) == formatResultLine(b));
  CHECK(formatResultJson(a) == formatResultJson(b));
  CHECK(a.checksum == b.checksum);
  REQUIRE(!a.replay.empty());
  CHECK(a.replay == b.replay);

  // Another seed is another match.
  setup.config.seed = 12;
  const MatchResult c = play(setup, f1);
  CHECK(c.checksum != a.checksum);
}

TEST_CASE("reference bots finish a match with a winner", "[arena]") {
  BotFactory factory;
  const MatchResult r = play(setupOf({"ref:hard", "ref:medium"}, 1, 36'000), factory);
  REQUIRE(r.outcome == MatchOutcome::Ended);
  REQUIRE(r.winner.isValid());
  REQUIRE(r.slots.size() == 2);
  const SlotResult& w = r.slots[r.winner.value];
  CHECK(w.place == 1);
  CHECK(w.score > r.slots[1 - r.winner.value].score);
  CHECK(r.ticks < 36'000);
  for (const SlotResult& s : r.slots) {
    CHECK(s.acts == r.ticks);  // every tick of the match, slot present throughout
    CHECK(s.substitutions == 0);
    CHECK(!s.module);
  }
  // Kills and deaths balance (no self-eliminations among reference bots is
  // not guaranteed, so only: every kill is someone's death).
  CHECK(r.slots[0].kills + r.slots[1].kills <= r.slots[0].deaths + r.slots[1].deaths);
}

TEST_CASE("the tick cap ends a match without a winner", "[arena]") {
  BotFactory factory;
  const MatchResult r = play(setupOf({"ref", "ref"}, 3, 90), factory);
  CHECK(r.outcome == MatchOutcome::Cap);
  CHECK(r.ticks == 90);
  CHECK(!r.winner.isValid());
  CHECK(contains(formatResultLine(r), " ticks 90 "));
  CHECK(contains(formatResultLine(r), " result cap winner none | "));
}

TEST_CASE("placements: winner, score, fewer deaths, lower slot", "[arena]") {
  MatchResult r;
  r.slots = {slotWith(0, 1, 4), slotWith(1, 3, 9), slotWith(2, 1, 2), slotWith(3, 1, 2),
             slotWith(4, 0, 0)};
  SECTION("no winner") {
    assignPlaces(r);
    CHECK(r.slots[1].place == 1);  // the top score
    CHECK(r.slots[2].place == 2);  // score 1, two deaths, slot 2 before slot 3
    CHECK(r.slots[3].place == 3);
    CHECK(r.slots[0].place == 4);  // score 1, four deaths
    CHECK(r.slots[4].place == 5);  // score 0, whatever the deaths
  }
  SECTION("the MatchEnded winner is always first") {
    r.outcome = MatchOutcome::Ended;
    r.winner = PlayerId{3};
    assignPlaces(r);
    CHECK(r.slots[3].place == 1);
    CHECK(r.slots[1].place == 2);
    CHECK(r.slots[2].place == 3);
  }
}

TEST_CASE("the result line and its JSON", "[arena]") {
  MatchResult r;
  r.index = 4;
  r.seed = 7;
  r.ticks = 5432;
  r.checksum = 0x00c0ffee;
  r.outcome = MatchOutcome::Ended;
  r.winner = PlayerId{1};
  r.slots = {slotWith(0, 1, 2), slotWith(1, 3, 1)};
  r.slots[0].kills = 3;
  r.slots[0].acts = 5432;
  r.slots[1].bot = R"(bots\x "y".wasm)";
  r.slots[1].module = true;
  r.slots[1].kills = 5;
  r.slots[1].acts = 5432;
  r.slots[1].substitutions = 3;
  r.slots[1].traps = 1;
  r.slots[1].overBudget = 2;
  r.slots[1].fuelMean = 61234;
  r.slots[1].fuelMax = 62731;
  r.slots[1].fuelInit = 13;
  assignPlaces(r);

  CHECK(formatResultLine(r) ==
        "match 4 seed 7 ticks 5432 checksum 00c0ffee result ended winner 1"
        " | slot 0 bot 0 ref:hard place 2 score 1 kills 3 deaths 2 acts 5432 subs 0"
        R"( | slot 1 bot 1 bots\x "y".wasm place 1 score 3 kills 5 deaths 1 acts 5432 subs 3)"
        " traps 1 over 2 bad 0 fuel_mean 61234 fuel_max 62731 fuel_init 13 log 0 dead no");
  CHECK(formatResultJson(r) ==
        R"({"match":4,"seed":7,"ticks":5432,"checksum":"00c0ffee","result":"ended","winner":1,)"
        R"("slots":[{"slot":0,"bot":0,"spec":"ref:hard","module":false,"place":2,"score":1,)"
        R"("kills":3,"deaths":2,"acts":5432,"subs":0},{"slot":1,"bot":1,)"
        R"("spec":"bots\\x \"y\".wasm","module":true,)"
        R"("place":1,"score":3,"kills":5,"deaths":1,"acts":5432,"subs":3,"traps":1,"over":2,)"
        R"("bad":0,"fuel_mean":61234,"fuel_max":62731,"fuel_init":13,"log":0,"dead":null}]})");

  r.winner = PlayerId::invalid();
  r.slots[1].dead = "init-trap";
  CHECK(contains(formatResultLine(r), " result ended winner none | "));
  CHECK(contains(formatResultLine(r), " dead init-trap"));
  CHECK(contains(formatResultJson(r), R"("winner":null,)"));
  CHECK(contains(formatResultJson(r), R"("dead":"init-trap"})"));
}

TEST_CASE("the summary totals a tournament", "[arena]") {
  BotFactory factory;
  Summary sum;
  const MatchSetup setup = setupOf({"ref:hard", "ref:easy"}, 20, 36'000);
  std::vector<MatchResult> results;
  for (uint32_t k = 0; k < 2; ++k) {
    MatchSetup s = setup;
    s.config.seed += k;
    results.push_back(play(s, factory, k));
    sum.add(results.back());
  }
  CHECK(sum.matches == 2);
  CHECK(sum.ended + sum.draws + sum.capped == 2);
  REQUIRE(sum.slots.size() == 2);
  CHECK(sum.slots[0].wins + sum.slots[1].wins == sum.ended);
  CHECK(sum.slots[0].placeSum + sum.slots[1].placeSum == 2 * (1 + 2));
  CHECK(sum.slots[0].acts == uint64_t{results[0].ticks} + results[1].ticks);
  const std::string line = formatSummaryLine(sum);
  CHECK(line.starts_with("summary matches 2 "));
  CHECK(contains(line, " | slot 0 ref:hard wins "));
  CHECK(contains(line, " | slot 1 ref:easy wins "));
  CHECK(contains(line, " mean_place "));
  CHECK(!sum.rotated);
  CHECK(!contains(line, " | bot "));  // without rotation the bots are the slots
  const std::string json = formatSummaryJson(sum);
  CHECK(json.starts_with(R"({"summary":true,"matches":2,)"));
  CHECK(contains(json, R"("slots":[{"slot":0,"spec":"ref:hard",)"));
  CHECK(!contains(json, R"("bots":)"));
  CHECK(json.ends_with("}]}"));
}

TEST_CASE("rotation moves bot b to slot (b + rotation) mod n", "[arena]") {
  BotFactory factory;
  MatchSetup setup = setupOf({"ref:hard", "ref:medium", "ref:easy"}, 9, 600);
  const MatchResult plain = play(setup, factory);
  setup.rotation = 1;
  const MatchResult r = play(setup, factory);
  REQUIRE(r.slots.size() == 3);
  CHECK(r.slots[0].botIndex == 2);
  CHECK(r.slots[0].bot == "ref:easy");
  CHECK(r.slots[1].botIndex == 0);
  CHECK(r.slots[1].bot == "ref:hard");
  CHECK(r.slots[2].botIndex == 1);
  CHECK(r.slots[2].bot == "ref:medium");
  CHECK(contains(formatResultLine(r), " | slot 0 bot 2 ref:easy place "));
  CHECK(contains(formatResultJson(r), R"({"slot":1,"bot":0,"spec":"ref:hard",)"));
  // A full turn is no rotation.
  setup.rotation = 3;
  CHECK(formatResultLine(play(setup, factory)) == formatResultLine(plain));
}

TEST_CASE("a rotated tournament totals per slot and per bot", "[arena]") {
  BotFactory factory;
  const MatchSetup base = setupOf({"ref:hard", "ref:easy"}, 30, 36'000);
  Summary sum;
  for (uint32_t k = 0; k < 4; ++k) {
    MatchSetup s = base;
    s.config.seed += k;
    s.rotation = k;
    sum.add(play(s, factory, k));
  }
  CHECK(sum.rotated);
  REQUIRE(sum.slots.size() == 2);
  REQUIRE(sum.bots.size() == 2);
  for (uint8_t i = 0; i < 2; ++i) {
    INFO("index " << int{i});
    CHECK(sum.slots[i].index == i);
    CHECK(sum.slots[i].bot == "mixed");
    CHECK(sum.bots[i].index == i);
  }
  CHECK(sum.bots[0].bot == "ref:hard");
  CHECK(sum.bots[1].bot == "ref:easy");
  // The same matches counted two ways.
  CHECK(sum.bots[0].wins + sum.bots[1].wins == sum.slots[0].wins + sum.slots[1].wins);
  CHECK(sum.bots[0].placeSum + sum.bots[1].placeSum == 4 * (1 + 2));
  CHECK(sum.bots[0].acts == sum.slots[0].acts);  // both slots play every tick
  CHECK(sum.bots[0].kills + sum.bots[1].kills == sum.slots[0].kills + sum.slots[1].kills);
  CHECK(sum.bots[0].wins > sum.bots[1].wins);  // hard beats easy from either slot

  const std::string line = formatSummaryLine(sum);
  CHECK(contains(line, " | slot 0 mixed wins "));
  CHECK(contains(line, " | slot 1 mixed wins "));
  CHECK(contains(line, " | bot 0 ref:hard wins "));
  CHECK(contains(line, " | bot 1 ref:easy wins "));
  CHECK(line.find(" | bot 0 ") > line.find(" | slot 1 "));  // slots first, then bots
  const std::string json = formatSummaryJson(sum);
  CHECK(contains(json, R"("slots":[{"slot":0,"spec":"mixed",)"));
  CHECK(contains(json, R"(],"bots":[{"bot":0,"spec":"ref:hard",)"));
  CHECK(contains(json, R"({"bot":1,"spec":"ref:easy",)"));
  CHECK(json.ends_with("}]}"));
}

TEST_CASE("the replay runMatch writes plays back to the same final state", "[arena]") {
  MatchSetup setup = setupOf({"ref:hard", "ref:medium", "ref:easy"}, 5, 36'000);
  setup.recordReplay = true;
  setup.delayTicks = 6;
  BotFactory factory;
  const MatchResult r = play(setup, factory);
  REQUIRE(!r.replay.empty());

  BitReader reader(r.replay);
  sim::ReplayHeader h;
  REQUIRE(sim::readReplayHeader(reader, h));
  CHECK(h.seed == 5);
  CHECK(h.contentHash == gameContent().hash);
  CHECK(h.players == 0b111);
  CHECK(h.tickCount == r.ticks);
  const sim::MatchContent match = sim::matchContent(gameContent().db, h.matchConfig());
  const auto world = std::make_unique<sim::WorldState>(sim::initialWorld(h.matchConfig()));
  sim::EventSink sink;
  bool ended = false;
  for (uint32_t t = 0; t < h.tickCount; ++t) {
    sim::InputFrame frame;
    REQUIRE(sim::readReplayFrame(reader, frame));
    REQUIRE(frame.tick == world->tick);
    sink.clear();
    sim::step(*world, frame, match, sink);
    for (const sim::SimEvent& e : sink)
      ended = ended || e.kind == sim::EventKind::MatchEnded;
  }
  CHECK(sim::checksum(*world) == r.checksum);
  CHECK(ended == (r.outcome == MatchOutcome::Ended));
  for (const SlotResult& s : r.slots)
    CHECK(world->rules.scores[s.slot] == s.score);
}

TEST_CASE("a setup without bots, or with too many, is refused", "[arena]") {
  BotFactory factory;
  MatchSetup setup;
  CHECK(!runMatch(setup, gameContent().db, factory).has_value());
  setup.bots.assign(sim::kMaxPlayers + 1, ai::BotSpec{});
  CHECK(!runMatch(setup, gameContent().db, factory).has_value());
}

#ifdef KFM_WASM_BOTS

namespace {

std::string fixture(const std::string& name) {
  return std::string{KFM_BOT_FIXTURES_DIR} + "/" + name + ".wasm";
}

}  // namespace

TEST_CASE("an echo module plays a match through the factory", "[arena][wasm]") {
  REQUIRE(wasmBotsAvailable());
  BotFactory factory;
  MatchSetup setup = setupOf({fixture("echo"), "ref:hard", fixture("echo")}, 9, 1200);
  setup.recordReplay = true;
  const MatchResult r = play(setup, factory);
  for (const uint8_t slot : {uint8_t{0}, uint8_t{2}}) {
    const SlotResult& s = r.slots[slot];
    CHECK(s.module);
    CHECK(s.acts == r.ticks);
    CHECK(s.substitutions == 0);
    CHECK(s.traps == 0);
    CHECK(s.overBudget == 0);
    CHECK(s.fuelMean > 0);
    CHECK(s.fuelMax >= s.fuelMean);
    CHECK(s.dead.empty());
  }
  CHECK(!r.slots[1].module);
  const std::string line = formatResultLine(r);
  CHECK(contains(line, " | slot 0 bot 0 " + fixture("echo") + " place "));
  CHECK(contains(line, " dead no"));

  // Two factories (two module caches, fresh instances) replay the same match.
  BotFactory other;
  const MatchResult again = play(setup, other);
  CHECK(formatResultLine(again) == line);
  CHECK(again.replay == r.replay);
}

TEST_CASE("a trapping module is substituted every tick", "[arena][wasm]") {
  BotFactory factory;
  const MatchResult r = play(setupOf({fixture("trap"), "ref:easy"}, 2, 600), factory);
  const SlotResult& s = r.slots[0];
  CHECK(s.acts == r.ticks);
  CHECK(s.traps == s.acts);
  CHECK(s.substitutions == s.acts);
  CHECK(s.dead.empty());  // a trap costs the tick, the agent lives on
  CHECK(contains(formatResultLine(r), " traps " + std::to_string(s.acts) + " over 0 "));
}

TEST_CASE("a spinning module runs over its budget on every even tick", "[arena][wasm]") {
  BotFactory factory(5'000);
  const MatchResult r = play(setupOf({fixture("spin"), "ref:easy"}, 2, 301), factory);
  const SlotResult& s = r.slots[0];
  REQUIRE(s.acts == r.ticks);
  const uint32_t evenTicks = (s.acts + 1) / 2;  // ticks 0, 2, 4, ...
  CHECK(s.overBudget == evenTicks);
  CHECK(s.substitutions == evenTicks);
  CHECK(s.traps == 0);
  CHECK(s.fuelMax == 5'000);  // the --fuel budget
}

TEST_CASE("module load failures are reported, not played", "[arena][wasm]") {
  BotFactory factory;
  SECTION("a missing file is an I/O error") {
    const auto made = factory.make(spec("no/such/bot.wasm"));
    REQUIRE(!made.has_value());
    CHECK(made.error().kind == BotErrorKind::Io);
    CHECK(contains(made.error().message, "no/such/bot.wasm"));
  }
  SECTION("a module the loader refuses carries the LoadError") {
    const auto prepared = factory.prepare(spec(fixture("bad_import")));
    REQUIRE(!prepared.has_value());
    CHECK(prepared.error().kind == BotErrorKind::Load);
    CHECK(contains(prepared.error().message, "bad-import"));
    const auto r =
        runMatch(setupOf({fixture("bad_import"), "ref"}, 1, 60), gameContent().db, factory);
    CHECK(!r.has_value());
  }
  SECTION("a module whose init traps plays dead") {
    const MatchResult r = play(setupOf({fixture("init_trap"), "ref:easy"}, 1, 120), factory);
    CHECK(r.slots[0].dead == "init-trap");
    CHECK(r.slots[0].substitutions == r.slots[0].acts);
    CHECK(contains(formatResultLine(r), " dead init-trap"));
  }
}

#else

TEST_CASE("without KFM_WASM_BOTS a module spec is refused", "[arena]") {
  CHECK(!wasmBotsAvailable());
  BotFactory factory;
  const auto made = factory.make(spec("bot.wasm"));
  REQUIRE(!made.has_value());
  CHECK(made.error().kind == BotErrorKind::Unsupported);
}

#endif
