// kfm_arena's core (apps/arena/arena_core.hpp, architecture §8, §9): a match
// is deterministic (result line and replay bytes), reference bots finish a
// match with a winner, the placement rule and its ties, the result line and
// JSON formats, the summary, rotation (every bot through every slot, totals
// per bot), the replay runMatch writes plays back to the same final state,
// v0.82 a mode's arena pool cycled over the matches (and the arena each
// played, in the line and the JSON), and, with KFM_WASM_BOTS, the module path over the fixtures in
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

#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

// value_or rather than a dereference after the check: clang-tidy cannot see
// that FAIL throws, and MSVC can, so a return after it is unreachable (C4702).
MatchResult play(const MatchSetup& setup, BotFactory& factory, uint32_t index = 0) {
  auto result = runMatch(setup, gameContent().db, factory, index);
  if (!result) FAIL(result.error().message);
  return std::move(result).value_or(MatchResult{});
}

SlotResult slotWith(uint8_t slot, int32_t score, uint32_t deaths) {
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

// A flat scan over the JSON kfm_arena prints, enough to pin its keys without
// a JSON library (no whitespace, keys without escapes). The end of the value
// starting at i, npos when it is malformed.
size_t valueEnd(std::string_view s, size_t i) {
  if (i >= s.size()) return std::string_view::npos;
  if (s[i] == '"') {
    for (++i; i < s.size(); ++i) {
      if (s[i] == '\\') {
        ++i;
      } else if (s[i] == '"') {
        return i + 1;
      }
    }
    return std::string_view::npos;
  }
  if (s[i] == '{' || s[i] == '[') {
    int depth = 0;
    bool inString = false;
    for (; i < s.size(); ++i) {
      const char c = s[i];
      if (inString) {
        if (c == '\\') {
          ++i;
        } else if (c == '"') {
          inString = false;
        }
      } else if (c == '"') {
        inString = true;
      } else if (c == '{' || c == '[') {
        ++depth;
      } else if ((c == '}' || c == ']') && --depth == 0) {
        return i + 1;
      }
    }
    return std::string_view::npos;
  }
  while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']')
    ++i;
  return i;
}

// The JSON type of a value, by its first character.
std::string_view typeOf(std::string_view v) {
  if (v.empty()) return "?";
  switch (v.front()) {
    case '"':
      return "string";
    case '{':
      return "object";
    case '[':
      return "array";
    case 't':
    case 'f':
      return "bool";
    case 'n':
      return "null";
    default:
      return "number";
  }
}

struct Member {
  std::string key;  // empty for an array element
  std::string_view value;
};

// The members of an object, or the elements of an array, in order; nullopt
// unless `text` is exactly one well-formed object or array.
std::optional<std::vector<Member>> membersOf(std::string_view text) {
  if (text.size() < 2 || (text.front() != '{' && text.front() != '[')) return std::nullopt;
  if (valueEnd(text, 0) != text.size()) return std::nullopt;
  const bool object = text.front() == '{';
  std::vector<Member> out;
  const size_t close = text.size() - 1;
  size_t i = 1;
  while (i < close) {
    Member m;
    if (object) {
      const size_t keyEnd = valueEnd(text, i);
      if (text[i] != '"' || keyEnd == std::string_view::npos || keyEnd >= close ||
          text[keyEnd] != ':') {
        return std::nullopt;
      }
      m.key = std::string(text.substr(i + 1, keyEnd - i - 2));
      i = keyEnd + 1;
    }
    const size_t end = valueEnd(text, i);
    if (end == std::string_view::npos || end == i || end > close) return std::nullopt;
    m.value = text.substr(i, end - i);
    out.push_back(std::move(m));
    i = end;
    if (i < close) {
      if (text[i] != ',') return std::nullopt;
      ++i;
    }
  }
  return out;
}

// Checks that `object` has exactly the frozen members, in order: each entry
// is "key:type", or "key:type|null" for a key that may be null.
void checkShape(std::string_view object, std::span<const std::string_view> frozen) {
  INFO(object);
  const std::optional<std::vector<Member>> members = membersOf(object);
  if (!members || object.front() != '{') {
    FAIL("not one JSON object");
    return;
  }
  std::vector<std::string> keys;
  std::vector<std::string> frozenKeys;
  keys.reserve(members->size());
  frozenKeys.reserve(frozen.size());
  for (const Member& m : *members)
    keys.push_back(m.key);
  for (const std::string_view f : frozen)
    frozenKeys.emplace_back(f.substr(0, f.find(':')));
  CHECK(keys == frozenKeys);
  for (size_t i = 0; i < members->size() && i < frozen.size(); ++i) {
    const std::string_view type = typeOf((*members)[i].value);
    const std::string_view types = frozen[i].substr(frozen[i].find(':') + 1);
    INFO("key " << keys[i] << " is " << type << ", frozen as " << types);
    CHECK((types == type || types == std::format("{}|null", type) ||
           (type == "null" && types.ends_with("|null"))));
  }
}

// A member's value (empty when absent) and an array's elements.
std::string_view valueOf(std::string_view object, std::string_view key) {
  const std::optional<std::vector<Member>> members = membersOf(object);
  if (!members) return {};
  for (const Member& m : *members) {
    if (m.key == key) return m.value;
  }
  return {};
}

std::vector<std::string_view> elementsOf(std::string_view array) {
  std::vector<std::string_view> out;
  const std::optional<std::vector<Member>> members = membersOf(array);
  if (!members || array.front() != '[') return out;
  for (const Member& m : *members)
    out.push_back(m.value);
  return out;
}

// The frozen shapes of docs/arena-result.md, in its order. A new key is
// appended to the doc's table and to the end of its list here, with the
// version that added it; a key is never inserted, renamed, removed or
// retyped (the ladder parses these objects across releases).
constexpr std::string_view kMatchHead[] = {"match:number",  "seed:number",
                                           "ticks:number",  "checksum:string",
                                           "result:string", "winner:number|null"};
constexpr std::string_view kMatchTeams[] = {"winner_team:number|null", "teams:array"};
constexpr std::string_view kMatchTail[] = {
    "slots:array",
    "content_hash:string",  // v0.60
    "build_hash:string",    // v0.60
    "arena:string",         // v0.82
};
constexpr std::string_view kTeam[] = {"team:number",  "entrant:number", "spec:string",
                                      "place:number", "score:number",   "kills:number",
                                      "deaths:number"};
constexpr std::string_view kSlotHead[] = {"slot:number", "bot:number", "spec:string",
                                          "module:bool"};
constexpr std::string_view kSlotTeam[] = {"team:number"};
constexpr std::string_view kSlotTail[] = {"place:number",  "score:number", "kills:number",
                                          "deaths:number", "acts:number",  "subs:number"};
constexpr std::string_view kSlotModule[] = {
    "traps:number",    "over:number",      "bad:number", "fuel_mean:number",
    "fuel_max:number", "fuel_init:number", "log:number", "dead:string|null"};
constexpr std::string_view kSummary[] = {"summary:bool", "matches:number", "ended:number",
                                         "draws:number", "cap:number",     "ticks_mean:number",
                                         "slots:array"};
constexpr std::string_view kSummaryBots[] = {"bots:array"};
constexpr std::string_view kSummaryEntrants[] = {"entrants:array"};
constexpr std::string_view kSummarySlot[] = {"slot:number"};
constexpr std::string_view kSummaryBot[] = {"bot:number"};
constexpr std::string_view kSummaryTotals[] = {"spec:string",       "module:bool",  "wins:number",
                                               "mean_place:number", "score:number", "kills:number",
                                               "deaths:number",     "acts:number",  "subs:number"};
constexpr std::string_view kSummaryModule[] = {"traps:number", "over:number", "bad:number",
                                               "fuel_max:number"};
constexpr std::string_view kEntrant[] = {"entrant:number",    "spec:string",  "wins:number",
                                         "mean_place:number", "score:number", "kills:number",
                                         "deaths:number"};

std::vector<std::string_view> join(std::initializer_list<std::span<const std::string_view>> parts) {
  std::vector<std::string_view> out;
  for (const std::span<const std::string_view> p : parts)
    out.insert(out.end(), p.begin(), p.end());
  return out;
}

}  // namespace

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
  // A seed whose match both slots lose a round of (paused below): which
  // seeds do moves with the reference bot's play.
  const MatchResult r = play(setupOf({"ref:hard", "ref:medium"}, 2, 36'000), factory);
  REQUIRE(r.outcome == MatchOutcome::Ended);
  REQUIRE(r.winner.isValid());
  REQUIRE(r.slots.size() == 2);
  const SlotResult& w = r.slots[r.winner.value];
  CHECK(w.place == 1);
  CHECK(w.score > r.slots[1 - r.winner.value].score);
  CHECK(r.ticks < 36'000);
  for (const SlotResult& s : r.slots) {
    // Every tick of the match, slot present throughout: acting, or paused
    // while eliminated (v0.35).
    CHECK(s.acts + s.paused == r.ticks);
    CHECK(s.paused > 0);
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
  CHECK(contains(formatResultLine(r), " result cap winner none arena dojo | "));
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
  r.contentHash = 0x5e1f00d2;
  r.buildHash = 0xabcd;
  r.arena = "dojo";
  assignPlaces(r);

  CHECK(formatResultLine(r) ==
        "match 4 seed 7 ticks 5432 checksum 00c0ffee result ended winner 1 arena dojo"
        " | slot 0 bot 0 ref:hard place 2 score 1 kills 3 deaths 2 acts 5432 subs 0"
        R"( | slot 1 bot 1 bots\x "y".wasm place 1 score 3 kills 5 deaths 1 acts 5432 subs 3)"
        " traps 1 over 2 bad 0 fuel_mean 61234 fuel_max 62731 fuel_init 13 log 0 dead no");
  CHECK(formatResultJson(r) ==
        R"({"match":4,"seed":7,"ticks":5432,"checksum":"00c0ffee","result":"ended","winner":1,)"
        R"("slots":[{"slot":0,"bot":0,"spec":"ref:hard","module":false,"place":2,"score":1,)"
        R"("kills":3,"deaths":2,"acts":5432,"subs":0},{"slot":1,"bot":1,)"
        R"("spec":"bots\\x \"y\".wasm","module":true,)"
        R"("place":1,"score":3,"kills":5,"deaths":1,"acts":5432,"subs":3,"traps":1,"over":2,)"
        R"("bad":0,"fuel_mean":61234,"fuel_max":62731,"fuel_init":13,"log":0,"dead":null}],)"
        R"("content_hash":"5e1f00d2","build_hash":"0000abcd","arena":"dojo"})");

  r.winner = PlayerId::invalid();
  r.slots[1].dead = "init-trap";
  CHECK(contains(formatResultLine(r), " result ended winner none arena dojo | "));
  CHECK(contains(formatResultLine(r), " dead init-trap"));
  CHECK(contains(formatResultJson(r), R"("winner":null,)"));
  CHECK(contains(formatResultJson(r), R"("dead":"init-trap"})"));
}

TEST_CASE("the JSON result's keys are the frozen contract (docs/arena-result.md)", "[arena]") {
  // A team match with a module slot (and a reference one), by hand.
  MatchResult r;
  r.index = 2;
  r.seed = 9;
  r.ticks = 100;
  r.outcome = MatchOutcome::Ended;
  r.winner = PlayerId{1};
  r.slots = {slotWith(0, 0, 1), slotWith(1, 2, 0)};
  r.slots[1].team = 1;
  r.slots[1].bot = "bots/x.wasm";
  r.slots[1].module = true;
  r.teams = {TeamResult{0, 1, "ref:hard", 0, 0, 0, 1}, TeamResult{1, 0, "bots/x.wasm", 0, 2, 1, 0}};
  assignPlaces(r);
  assignTeamPlaces(r);

  SECTION("a team match") {
    const std::string json = formatResultJson(r);
    checkShape(json, join({kMatchHead, kMatchTeams, kMatchTail}));
    const std::vector<std::string_view> teams = elementsOf(valueOf(json, "teams"));
    REQUIRE(teams.size() == 2);
    for (const std::string_view t : teams)
      checkShape(t, kTeam);
    const std::vector<std::string_view> slots = elementsOf(valueOf(json, "slots"));
    REQUIRE(slots.size() == 2);
    checkShape(slots[0], join({kSlotHead, kSlotTeam, kSlotTail}));
    checkShape(slots[1], join({kSlotHead, kSlotTeam, kSlotTail, kSlotModule}));
    r.slots[1].dead = "trap";  // dead: a string once the module is dead
    const std::string dead = formatResultJson(r);
    const std::vector<std::string_view> deadSlots = elementsOf(valueOf(dead, "slots"));
    REQUIRE(deadSlots.size() == 2);
    CHECK(valueOf(deadSlots[1], "dead") == R"("trap")");
    checkShape(deadSlots[1], join({kSlotHead, kSlotTeam, kSlotTail, kSlotModule}));
  }
  SECTION("a free-for-all, no winner") {
    r.teams.clear();
    r.outcome = MatchOutcome::Cap;
    r.winner = PlayerId::invalid();
    const std::string json = formatResultJson(r);
    checkShape(json, join({kMatchHead, kMatchTail}));
    CHECK(valueOf(json, "winner") == "null");
    const std::vector<std::string_view> slots = elementsOf(valueOf(json, "slots"));
    REQUIRE(slots.size() == 2);
    checkShape(slots[0], join({kSlotHead, kSlotTail}));
    checkShape(slots[1], join({kSlotHead, kSlotTail, kSlotModule}));
  }
  SECTION("the summary, rotated, with entrants") {
    Summary sum;
    sum.add(r);
    sum.rotated = true;  // as if a later match had rotated the bots
    REQUIRE(sum.slots.size() == 2);
    REQUIRE(sum.bots.size() == 2);
    REQUIRE(sum.entrants.size() == 2);
    const std::string json = formatSummaryJson(sum);
    checkShape(json, join({kSummary, kSummaryBots, kSummaryEntrants}));
    const std::vector<std::string_view> slots = elementsOf(valueOf(json, "slots"));
    REQUIRE(slots.size() == 2);
    checkShape(slots[0], join({kSummarySlot, kSummaryTotals}));
    checkShape(slots[1], join({kSummarySlot, kSummaryTotals, kSummaryModule}));
    const std::vector<std::string_view> bots = elementsOf(valueOf(json, "bots"));
    REQUIRE(bots.size() == 2);
    checkShape(bots[0], join({kSummaryBot, kSummaryTotals}));
    checkShape(bots[1], join({kSummaryBot, kSummaryTotals, kSummaryModule}));
    const std::vector<std::string_view> entrants = elementsOf(valueOf(json, "entrants"));
    REQUIRE(entrants.size() == 2);
    for (const std::string_view e : entrants)
      checkShape(e, kEntrant);
  }
  SECTION("the summary of a plain tournament") {
    r.teams.clear();
    Summary sum;
    sum.add(r);
    checkShape(formatSummaryJson(sum), kSummary);
  }
}

TEST_CASE("every --json line is one complete object with the match's hashes", "[arena]") {
  BotFactory factory;
  MatchSetup setup = setupOf({"ref:hard", "ref:easy"}, 3, 600);
  setup.recordReplay = true;
  Summary sum;
  std::vector<std::string> lines;
  for (uint32_t k = 0; k < 2; ++k) {
    MatchSetup s = setup;
    s.config.seed += k;
    const MatchResult r = play(s, factory, k);
    // The hashes are the replay header's.
    BitReader reader(r.replay);
    sim::ReplayHeader h;
    REQUIRE(sim::readReplayHeader(reader, h));
    CHECK(r.contentHash == gameContent().hash);
    CHECK(h.contentHash == r.contentHash);
    CHECK(h.buildHash == r.buildHash);
    lines.push_back(formatResultJson(r));
    CHECK(valueOf(lines.back(), "content_hash") == std::format("\"{:08x}\"", h.contentHash));
    CHECK(valueOf(lines.back(), "build_hash") == std::format("\"{:08x}\"", h.buildHash));
    sum.add(r);
  }
  lines.push_back(formatSummaryJson(sum));
  for (const std::string& line : lines) {
    INFO(line);
    CHECK(line.starts_with('{'));
    CHECK(line.ends_with('}'));
    CHECK(!line.contains('\n'));
    CHECK(membersOf(line).has_value());
  }
  CHECK(valueOf(lines[0], "summary").empty());  // only the summary carries "summary"
  CHECK(valueOf(lines[2], "summary") == "true");
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
  CHECK(sum.slots[0].acts <= uint64_t{results[0].ticks} + results[1].ticks);
  CHECK(sum.slots[0].acts + results[0].slots[0].paused + results[1].slots[0].paused ==
        uint64_t{results[0].ticks} + results[1].ticks);
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
  CHECK(sum.bots[0].acts + sum.bots[1].acts == sum.slots[0].acts + sum.slots[1].acts);
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

TEST_CASE("a team match fills each team's slots from its entrant (--team)", "[arena][teams]") {
  BotFactory factory;
  const sim::ContentDB& db = gameContent().db;
  MatchSetup setup;
  setup.config.seed = 41;
  const std::optional<uint8_t> mode = db.findMode("team_rounds");
  const std::optional<uint8_t> arena = db.findArena("standoff");
  REQUIRE(mode);
  REQUIRE(arena);
  setup.config.mode = mode.value_or(0);
  setup.config.arena = arena.value_or(0);
  setup.teams = {{spec("ref:easy"), spec("ref:medium")}, {spec("ref:hard")}};
  setup.tickCap = 36'000;
  const MatchResult r = play(setup, factory);
  // Two teams of two: slots 0, 2 are team 0 (entrant 0 in order), 1, 3 team
  // 1 (entrant 1's one spec repeated).
  REQUIRE(r.slots.size() == 4);
  CHECK(r.slots[0].bot == "ref:easy");
  CHECK(r.slots[2].bot == "ref:medium");
  CHECK(r.slots[1].bot == "ref:hard");
  CHECK(r.slots[3].bot == "ref:hard");
  for (const SlotResult& s : r.slots)
    CHECK(s.team == s.slot % 2);
  REQUIRE(r.teams.size() == 2);
  CHECK(r.teams[0].bots == "ref:easy,ref:medium");
  CHECK(r.teams[1].bots == "ref:hard,ref:hard");
  CHECK(r.teams[0].entrant == 0);
  CHECK(r.teams[1].entrant == 1);
  CHECK(r.teams[0].place + r.teams[1].place == 3);
  CHECK(r.teams[0].deaths == r.slots[0].deaths + r.slots[2].deaths);
  if (r.outcome == MatchOutcome::Ended && r.winner.isValid()) {
    const uint8_t won = r.slots[r.winner.value].team;
    CHECK(r.teams[won].place == 1);
    CHECK(r.teams[won].score >= 3);  // first team to 3 round wins
  }
  const std::string line = formatResultLine(r);
  CHECK(contains(line, " winner_team "));
  CHECK(contains(line, " | team 0 entrant 0 ref:easy,ref:medium place "));
  CHECK(contains(line, " | slot 0 bot 0 ref:easy team 0 place "));
  CHECK(contains(line, " | slot 3 bot 3 ref:hard team 1 place "));
  const std::string json = formatResultJson(r);
  CHECK(contains(json, R"("teams":[{"team":0,"entrant":0,"spec":"ref:easy,ref:medium","place":)"));
  CHECK(contains(json, R"({"slot":1,"bot":2,"spec":"ref:hard","module":false,"team":1,"place":)"));

  // Rotated: entrant 0 plays team 1 (the east side) in match 1.
  MatchSetup rotated = setup;
  rotated.rotation = 1;
  const MatchResult q = play(rotated, factory, 1);
  CHECK(q.slots[1].bot == "ref:easy");
  CHECK(q.slots[3].bot == "ref:medium");
  CHECK(q.slots[0].bot == "ref:hard");
  CHECK(q.teams[1].entrant == 0);
  Summary sum;
  sum.add(r);
  sum.add(q);
  REQUIRE(sum.entrants.size() == 2);
  CHECK(sum.entrants[0].bots == "ref:easy,ref:medium");
  CHECK(sum.entrants[0].placeSum + sum.entrants[1].placeSum == 2 * (1 + 2));
  CHECK(sum.entrants[0].wins + sum.entrants[1].wins == sum.ended);
  CHECK(contains(formatSummaryLine(sum), " | entrant 1 ref:hard,ref:hard wins "));
  CHECK(contains(formatSummaryJson(sum), R"("entrants":[{"entrant":0,)"));

  // The entrants must match the mode's teams.
  MatchSetup three = setup;
  three.teams.push_back({spec("ref")});
  CHECK_FALSE(runMatch(three, db, factory).has_value());
  MatchSetup ffa = setup;
  ffa.config.mode = 0;
  CHECK_FALSE(runMatch(ffa, db, factory).has_value());
}

TEST_CASE("reference bots finish grid matches under fog (v0.35)", "[arena][teams][grid]") {
  // Step 4's bots never lost a grid round (they saw every banana and
  // side-stepped it); grid-aware bots under own vision end their matches.
  BotFactory factory;
  const sim::ContentDB& db = gameContent().db;
  const std::optional<uint8_t> mode = db.findMode("grid_rounds_1t");
  REQUIRE(mode);
  for (const uint64_t seed : {uint64_t{1}, uint64_t{3}}) {
    INFO("seed " << seed);
    MatchSetup setup;
    setup.config.seed = seed;
    setup.config.mode = mode.value_or(0);
    setup.config.arena = db.firstCompatibleArena(mode.value_or(0)).value_or(0);
    setup.teams = {{spec("ref:hard")}, {spec("ref:hard")}};
    setup.teams[0].resize(2, spec("ref:hard"));
    setup.tickCap = 36'000;
    const MatchResult r = play(setup, factory);
    CHECK(r.outcome == MatchOutcome::Ended);
    CHECK(r.winner.isValid());
    CHECK(r.teams[0].kills + r.teams[1].kills > 0);
  }
}

TEST_CASE("bots in a squad match hear their team's calls", "[arena][teams][grid][calls]") {
  // runMatch must pass every step to AgentHost::observe (it steps through
  // AgentHost::step): without it no bot ever hears a call.
  BotFactory factory;
  const sim::ContentDB& db = gameContent().db;
  const std::optional<uint8_t> mode = db.findMode("grid_rounds_1t");
  REQUIRE(mode);
  MatchSetup setup;
  setup.config.seed = 1;
  setup.config.mode = mode.value_or(0);
  setup.config.arena = db.firstCompatibleArena(mode.value_or(0)).value_or(0);
  setup.teams = {{spec("ref:hard"), spec("ref:hard")}, {spec("ref:hard"), spec("ref:hard")}};
  setup.tickCap = 3600;
  const MatchResult r = play(setup, factory);
  uint32_t heard = 0;
  for (const SlotResult& s : r.slots)
    heard += s.heard;
  CHECK(heard > 0);
}

TEST_CASE("team places: the winner's team, then team score, then fewer deaths", "[arena][teams]") {
  MatchResult r;
  r.slots = {slotWith(0, 0, 3), slotWith(1, 1, 0), slotWith(2, 0, 1), slotWith(3, 0, 0)};
  for (SlotResult& s : r.slots)
    s.team = static_cast<uint8_t>(s.slot % 2);
  r.teams.resize(2);
  r.teams[0] = TeamResult{0, 0, "a", 0, 2, 0, 4};
  r.teams[1] = TeamResult{1, 1, "b", 0, 2, 0, 1};
  assignTeamPlaces(r);
  CHECK(r.teams[1].place == 1);  // tied score, fewer deaths
  r.winner = PlayerId{2};        // a team-0 slot won the match
  assignTeamPlaces(r);
  CHECK(r.teams[0].place == 1);
  r.winner = PlayerId::invalid();
  r.teams[0].score = 3;
  assignTeamPlaces(r);
  CHECK(r.teams[0].place == 1);
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
    CHECK(s.acts + s.paused == r.ticks);
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
  // Against an idle module nobody is eliminated, so the slot acts every tick.
  const MatchResult r = play(setupOf({fixture("trap"), fixture("idle")}, 2, 600), factory);
  const SlotResult& s = r.slots[0];
  CHECK(s.acts == r.ticks);
  CHECK(s.traps == s.acts);
  CHECK(s.substitutions == s.acts);
  CHECK(s.dead.empty());  // a trap costs the tick, the agent lives on
  CHECK(contains(formatResultLine(r), " traps " + std::to_string(s.acts) + " over 0 "));
}

TEST_CASE("a spinning module runs over its budget on every even tick", "[arena][wasm]") {
  BotFactory factory(5'000);
  const MatchResult r = play(setupOf({fixture("spin"), fixture("idle")}, 2, 301), factory);
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

// v0.82: the arena pool. Hand-built content: four copies of the dojo with two
// spawn groups (arenas 1..4), a team deathmatch of two teams pooling arenas
// 1, 3 and 4 (arena 4 left with one spawn group, so it does not suit the mode
// and drops out: the pool is [1, 3]), and a free-for-all deathmatch pooling
// 2 and 1.
namespace {

constexpr uint8_t kPooledTeams = 1;
constexpr uint8_t kPooledFfa = 2;
constexpr uint8_t kUnpooled = 3;

[[nodiscard]] sim::ContentDB pooledContent() {
  sim::ContentDB c = sim::defaultContent();
  for (uint8_t i = 1; i <= 4; ++i) {
    c.arenas[i] = c.arenas[0];
    c.arenas[i].name = sim::ContentName::of(std::format("pool{}", i));
    c.arenas[i].spawnGroups = i == 4 ? 1 : 2;
  }
  c.arenaCount = 5;
  sim::ModeDef dm = c.modes[0];
  dm.kind = sim::ModeKind::TeamDeathmatch;
  dm.teams = 2;
  dm.scoreToWin = 50;
  dm.timeLimitTicks = 0;
  dm.name = sim::ContentName::of("pooled");
  dm.arenaSet = (1u << 1) | (1u << 3) | (1u << 4);
  c.modes[kPooledTeams] = dm;
  sim::ModeDef ffa = dm;
  ffa.kind = sim::ModeKind::Deathmatch;
  ffa.teams = 0;
  ffa.name = sim::ContentName::of("pooled_ffa");
  ffa.arenaSet = (1u << 2) | (1u << 1);
  c.modes[kPooledFfa] = ffa;
  sim::ModeDef plain = ffa;
  plain.name = sim::ContentName::of("plain");
  plain.arenaSet = sim::kNoArenaSet;
  c.modes[kUnpooled] = plain;
  c.modeCount = 4;
  return c;
}

// Plays `matches` matches of `setup` the way kfm_arena does (seed + k,
// --rotate's rotation, the cycle's arena) and returns each one's result.
std::vector<MatchResult> tournament(const sim::ContentDB& content, MatchSetup setup,
                                    uint32_t matches, bool rotate, bool arenaNamed) {
  BotFactory factory;
  const ArenaCycle cycle = arenaCycle(content, setup, arenaNamed);
  const uint64_t seed = setup.config.seed;
  std::vector<MatchResult> out;
  for (uint32_t k = 0; k < matches; ++k) {
    setup.config.seed = seed + k;
    setup.rotation = rotate ? k : 0;
    setup.config.arena = cycle.arenaFor(k);
    auto r = runMatch(setup, content, factory, k);
    if (!r) FAIL(r.error().message);
    out.push_back(std::move(r).value_or(MatchResult{}));
  }
  return out;
}

}  // namespace

TEST_CASE("a mode's arena pool: its members that suit it, in content order", "[arena][pool]") {
  const sim::ContentDB c = pooledContent();
  CHECK(arenaPool(c, kPooledTeams) == std::vector<uint8_t>{1, 3});
  CHECK(arenaPool(c, kPooledFfa) == std::vector<uint8_t>{1, 2});
  CHECK(arenaPool(c, kUnpooled).empty());
  // The other hosts pick one: the pool's first that suits the mode.
  CHECK(c.firstCompatibleArena(kPooledTeams) == std::optional<uint8_t>(1));

  ArenaCycle cycle;
  cycle.pool = {1, 3};
  cycle.period = 2;
  cycle.fixed = 4;
  const std::vector<uint8_t> want{1, 1, 3, 3, 1, 1, 3};
  for (uint32_t k = 0; k < want.size(); ++k) {
    INFO("match " << k);
    CHECK(cycle.arenaFor(k) == want[k]);
  }
  cycle.period = 0;  // counts as 1
  CHECK(cycle.arenaFor(1) == 3);
  cycle.pool.clear();
  CHECK(cycle.arenaFor(5) == 4);
}

TEST_CASE("a team tournament plays every side of a pool member before the next",
          "[arena][pool][teams]") {
  const sim::ContentDB c = pooledContent();
  MatchSetup setup;
  setup.config.seed = 5;
  setup.config.mode = kPooledTeams;
  setup.config.arena = 1;
  setup.teams = {{spec("ref:hard")}, {spec("ref:easy")}};
  setup.tickCap = 30;

  const ArenaCycle cycle = arenaCycle(c, setup, false);
  CHECK(cycle.pool == std::vector<uint8_t>{1, 3});
  CHECK(cycle.period == 2);

  SECTION("--rotate, matches not a multiple of T x P") {
    const std::vector<MatchResult> rs = tournament(c, setup, 7, true, false);
    const std::vector<std::string> arenas{"pool1", "pool1", "pool3", "pool3",
                                          "pool1", "pool1", "pool3"};
    for (uint32_t k = 0; k < rs.size(); ++k) {
      INFO("match " << k);
      CHECK(rs[k].arena == arenas[k]);
      REQUIRE(rs[k].teams.size() == 2);
      // Entrant e plays team (e + k) mod 2: both sides of each member.
      CHECK(rs[k].teams[0].entrant == (k % 2 == 0 ? 0 : 1));
      CHECK(contains(formatResultLine(rs[k]), " arena " + arenas[k] + " | "));
      CHECK(contains(formatResultJson(rs[k]), R"("arena":")" + arenas[k] + R"("})"));
    }
  }
  SECTION("without --rotate the pool still turns every T matches") {
    const std::vector<MatchResult> rs = tournament(c, setup, 5, false, false);
    const std::vector<std::string> arenas{"pool1", "pool1", "pool3", "pool3", "pool1"};
    for (uint32_t k = 0; k < rs.size(); ++k) {
      INFO("match " << k);
      CHECK(rs[k].arena == arenas[k]);
      CHECK(rs[k].teams[0].entrant == 0);
    }
  }
  SECTION("--arena NAME plays that arena every match") {
    setup.config.arena = 3;
    const std::vector<MatchResult> rs = tournament(c, setup, 4, true, true);
    for (const MatchResult& r : rs)
      CHECK(r.arena == "pool3");
  }
}

TEST_CASE("a free-for-all pool turns once every slot has played every seat", "[arena][pool]") {
  const sim::ContentDB c = pooledContent();
  MatchSetup setup = setupOf({"ref:hard", "ref:easy", "ref"}, 9, 20);
  setup.config.mode = kPooledFfa;
  const ArenaCycle cycle = arenaCycle(c, setup, false);
  CHECK(cycle.period == 3);
  const std::vector<MatchResult> rs = tournament(c, setup, 8, true, false);
  const std::vector<std::string> arenas{"pool1", "pool1", "pool1", "pool2",
                                        "pool2", "pool2", "pool1", "pool1"};
  for (uint32_t k = 0; k < rs.size(); ++k) {
    INFO("match " << k);
    CHECK(rs[k].arena == arenas[k]);
  }

  // A mode without a pool plays its one arena every match.
  setup.config.mode = kUnpooled;
  for (const MatchResult& r : tournament(c, setup, 3, true, false))
    CHECK(r.arena == "dojo");
}
