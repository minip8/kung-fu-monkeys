#include "kfm/content/loader.hpp"

#include "kfm/content/app_content.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/spaces/grid_cells.hpp"
#include "kfm/sim/visit.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace kfm;
using namespace kfm::content;
using sim::ContentDB;

namespace {

// The canonical serialized form of any def (what contentHash() hashes for
// the whole DB), so two defs compare field by field through the visitor.
template <class T>
std::vector<std::byte> bytesOf(const T& def) {
  std::vector<std::byte> buf(sizeof(T) * 2 + 16);
  BitWriter w(buf);
  REQUIRE(sim::writeTo(w, def));
  buf.resize(w.bytesWritten());
  return buf;
}

// defaultContent() is the minimal fixture and content/ a superset (§5.10):
// the singletons, every ability the fixture defines, arena 0, mode 0 and
// loadout 0 must agree; further abilities, arenas, modes and loadouts are
// TOML-only.
void requireAgreesWithFixture(const ContentDB& loaded) {
  const ContentDB& fixture = sim::defaultContent();
  CHECK(bytesOf(loaded.player) == bytesOf(fixture.player));
  CHECK(bytesOf(loaded.boomerang) == bytesOf(fixture.boomerang));
  CHECK(bytesOf(loaded.pickup) == bytesOf(fixture.pickup));
  CHECK(bytesOf(loaded.dashAttack) == bytesOf(fixture.dashAttack));
  REQUIRE(loaded.abilityCount >= fixture.abilityCount);
  for (uint8_t i = 0; i < fixture.abilityCount; ++i) {
    INFO("ability " << int{i});
    CHECK(bytesOf(loaded.abilities[i]) == bytesOf(fixture.abilities[i]));
  }
  REQUIRE(loaded.arenaCount >= 1);
  REQUIRE(loaded.modeCount >= 1);
  CHECK(bytesOf(loaded.arenas[0]) == bytesOf(fixture.arenas[0]));
  CHECK(bytesOf(loaded.modes[0]) == bytesOf(fixture.modes[0]));
  REQUIRE(loaded.loadoutCount >= 1);
  CHECK(bytesOf(loaded.loadouts[0]) == bytesOf(fixture.loadouts[0]));
}

// A minimal complete content set; tests break one piece at a time.
constexpr std::string_view kMinimal = R"(
[player]
radius = 0.5
move_speed = 7
max_hp = 1
ammo_max = 1
ammo_start = 1
abilities = ["dash", "throw"]

[boomerang]
radius = 0.35
speed = 16
spawn_offset = 1

[pickup]
radius = 0.5

[dash_attack]
min_ammo = 1

[[ability]]
name = "dash"
kind = "dash"
active_ticks = 12
perfect_window_ticks = 4

[[ability]]
name = "throw"
kind = "throw"

[[arena]]
name = "box"
half_extents = [10, 5]
spawn_points = [[-2, 0], [2, 0]]

[[mode]]
name = "rounds"
)";

std::string replaced(std::string_view text, std::string_view from, std::string_view to) {
  std::string s(text);
  const size_t at = s.find(from);
  REQUIRE(at != std::string::npos);
  s.replace(at, from.size(), to);
  return s;
}

// Loads `text` expecting failure; returns the formatted diagnostics.
std::string loadErrors(std::string_view text) {
  ContentDB db{};
  db.arenaCount = 77;  // a sentinel: a failed load must not touch the output
  Diagnostics diag;
  CHECK_FALSE(parseContent(text, "test.toml", db, diag));
  CHECK(diag.hasErrors());
  CHECK(db.arenaCount == 77);
  return diag.format();
}

// Loads `text` expecting success.
bool loads(std::string_view text) {
  ContentDB db{};
  Diagnostics diag;
  const bool ok = parseContent(text, "test.toml", db, diag);
  INFO(diag.format());
  return ok;
}

bool contains(const std::string& haystack, std::string_view needle) {
  return haystack.contains(needle);
}

}  // namespace

TEST_CASE("content/ agrees with defaultContent() on the default match", "[content][loader]") {
  ContentDB loaded{};
  Diagnostics diag;
  const bool ok = loadContentDir(KFM_CONTENT_DIR, loaded, diag);
  INFO(diag.format());
  REQUIRE(ok);
  CHECK(diag.items().empty());
  requireAgreesWithFixture(loaded);
  CHECK(loaded.arenas[0].name.view() == "dojo");
  CHECK(loaded.modes[0].name.view() == "rounds");
}

TEST_CASE("the embedded copy is the content/ directory", "[content][loader]") {
  std::vector<ContentSource> fromDisk;
  Diagnostics diag;
  REQUIRE(readContentDir(KFM_CONTENT_DIR, fromDisk, diag));
  const std::span<const ContentSource> embedded = embeddedContentSources();
  REQUIRE(embedded.size() == fromDisk.size());
  for (size_t i = 0; i < embedded.size(); ++i) {
    CHECK(embedded[i].name == fromDisk[i].name);
    CHECK(embedded[i].text == fromDisk[i].text);
  }
  ContentDB loaded{};
  REQUIRE(loadEmbeddedContent(loaded, diag));
  requireAgreesWithFixture(loaded);
  ContentDB fromDir{};
  REQUIRE(loadContentDir(KFM_CONTENT_DIR, fromDir, diag));
  CHECK(sim::contentHash(loaded) == sim::contentHash(fromDir));
}

TEST_CASE("the fixture comparison ignores content beyond the default match", "[content][loader]") {
  // A superset (an extra arena, as Phase 2 adds) still agrees: only the
  // whole-DB hash differs.
  std::vector<ContentSource> sources;
  Diagnostics diag;
  REQUIRE(readContentDir(KFM_CONTENT_DIR, sources, diag));
  sources.push_back(ContentSource{"zz_extra.toml", R"(
[[arena]]
name = "zz-pit"
half_extents = [6, 4]
spawn_points = [[-2, 0], [2, 0]]
)"});
  ContentDB loaded{};
  const bool ok = parseContent(sources, loaded, diag);
  INFO(diag.format());
  REQUIRE(ok);
  CHECK(loaded.arenaCount >= 2);
  requireAgreesWithFixture(loaded);
  CHECK(sim::contentHash(loaded) != sim::contentHash(sim::defaultContent()));
}

TEST_CASE("defaultContent() passes the loader's validation", "[content][loader]") {
  Diagnostics diag;
  CHECK(validateContent(sim::defaultContent(), diag));
  INFO(diag.format());
  CHECK(diag.items().empty());
}

TEST_CASE("a minimal content set loads, names resolve to indices", "[content][loader]") {
  ContentDB db{};
  Diagnostics diag;
  const bool ok = parseContent(kMinimal, "min.toml", db, diag);
  INFO(diag.format());
  REQUIRE(ok);
  CHECK(db.abilityCount == 3);
  CHECK(db.abilities[0].name.view() == "none");
  CHECK(db.abilities[0].kind == sim::AbilityKind::None);
  CHECK(db.player.abilities[sim::kAbilitySlotDash] == 1);
  CHECK(db.player.abilities[sim::kAbilitySlotThrow] == 2);
  CHECK(db.abilityForSlot(sim::kAbilitySlotThrow).kind == sim::AbilityKind::Throw);
  CHECK(db.player.moveSpeed == Fixed::fromRatio(7, 60));
  CHECK(db.boomerang.radius == Fixed::fromRatio(35, 100));
  REQUIRE(db.arenaCount == 1);
  CHECK(db.arenas[0].spawnPointCount == 2);
  CHECK(db.arenas[0].spawnPoints[1] == Vec2{Fixed::fromInt(2), Fixed::zero()});
  CHECK(db.findArena("box") == std::optional<uint8_t>{0});
  CHECK_FALSE(db.findArena("dojo").has_value());
  CHECK(db.modes[0].roundsToWin == sim::ModeDef{}.roundsToWin);  // optional keys keep defaults
}

TEST_CASE("loadouts: [[loadout]] defs, loadout 0 and MatchContent::loadout (v0.31)",
          "[content][loader][loadout]") {
  ContentDB db{};
  Diagnostics diag;
  const std::string text = std::string(kMinimal) +
                           "\n[[loadout]]\nname = \"swap\"\nabilities = [\"throw\", \"dash\", "
                           "\"dash\"]\n";
  const bool ok = parseContent(text, "min.toml", db, diag);
  INFO(diag.format());
  REQUIRE(ok);
  // The base kit leaves the ability button's slot on none and is loadout 0.
  CHECK(db.player.abilities[sim::kAbilitySlotExtra] == 0);
  REQUIRE(db.loadoutCount == 2);
  CHECK(db.loadouts[0].name.view() == sim::kBaseLoadoutName);
  CHECK(db.loadouts[0].abilities == db.player.abilities);
  CHECK(db.loadouts[1].name.view() == "swap");
  CHECK(db.loadouts[1].abilities == std::array<uint8_t, sim::kAbilitySlots>{2, 1, 1});
  CHECK(db.findLoadout("base") == std::optional<uint8_t>{0});
  CHECK(db.findLoadout("swap") == std::optional<uint8_t>{1});
  CHECK_FALSE(db.findLoadout("zap").has_value());
  const sim::MatchContent base(db, 0, 0);
  const sim::MatchContent swap = sim::matchContent(db, sim::MatchConfig{.loadout = 1});
  CHECK(base.loadout() == db.player.abilities);
  CHECK(swap.loadout() == db.loadouts[1].abilities);
  // An index past the content falls back to the base kit.
  CHECK(sim::matchContent(db, sim::MatchConfig{.loadout = 9}).loadout() == db.player.abilities);

  CHECK(contains(
      loadErrors(text + "\n[[loadout]]\nname = \"bad\"\nabilities = [\"dash\", \"none\"]\n"),
      "loadout \"bad\" abilities: the throw slot must hold an ability"));
  CHECK(contains(
      loadErrors(text + "\n[[loadout]]\nname = \"bad\"\nabilities = [\"none\", \"throw\"]\n"),
      "loadout \"bad\" abilities: the dash slot must hold an ability"));
  CHECK(contains(
      loadErrors(text + "\n[[loadout]]\nname = \"bad\"\nabilities = [\"dash\", \"zap\"]\n"),
      "names unknown ability \"zap\""));
  CHECK(contains(loadErrors(text + "\n[[loadout]]\nname = \"bad\"\nabilities = [\"dash\"]\n"),
                 "'abilities' must list 2 or 3 ability names"));
  CHECK(contains(loadErrors(text + "\n[[loadout]]\nname = \"bad\"\n"),
                 "missing required key 'abilities'"));
  CHECK(contains(loadErrors(text + "\n[[loadout]]\nname = \"swap\"\nabilities = [\"dash\", "
                                   "\"throw\"]\n"),
                 "loadout \"swap\" is defined twice"));
  CHECK(contains(loadErrors(text + "\n[[loadout]]\nname = \"base\"\nabilities = [\"dash\", "
                                   "\"throw\"]\n"),
                 "\"base\" is the built-in loadout of [player] abilities"));
  CHECK(contains(loadErrors(text + "\n[[loadout]]\nname = \"x\"\nabilities = [\"dash\", "
                                   "\"throw\"]\ncolor = 3\n"),
                 "unknown key 'color'"));
  // v0.31: a mode no longer carries a loadout.
  CHECK(contains(loadErrors(std::string(kMinimal) +
                            "\n[[mode]]\nname = \"m\"\nabilities = [\"dash\", \"throw\"]\n"),
                 "a mode no longer carries a loadout"));

  // At most kMaxLoadouts in all: base plus 15 [[loadout]] tables.
  std::string many(kMinimal);
  for (uint32_t i = 1; i < sim::kMaxLoadouts; ++i) {
    many +=
        "\n[[loadout]]\nname = \"l" + std::to_string(i) + "\"\nabilities = [\"dash\", \"throw\"]\n";
  }
  ContentDB full{};
  REQUIRE(parseContent(many, "many.toml", full, diag));
  CHECK(full.loadoutCount == sim::kMaxLoadouts);
  CHECK(contains(loadErrors(many + "\n[[loadout]]\nname = \"one_more\"\nabilities = [\"dash\", "
                                   "\"throw\"]\n"),
                 "too many loadouts (at most 15 besides base, kMaxLoadouts)"));

  // validateContent() holds loadout 0 to the [player] abilities.
  ContentDB broken = db;
  broken.loadouts[0].abilities[sim::kAbilitySlotExtra] = 2;
  Diagnostics bad;
  CHECK_FALSE(validateContent(broken, bad));
  CHECK(contains(bad.format(), "loadout 0 must be \"base\" with the [player] abilities"));
}

TEST_CASE("--loadout names resolve to MatchConfig::loadout", "[content][app][loadout]") {
  sim::ContentDB db{};
  Diagnostics diag;
  REQUIRE(loadEmbeddedContent(db, diag));
  sim::MatchConfig config{.seed = 3, .arena = 1, .loadout = 7};
  std::string error;
  REQUIRE(resolveLoadoutName(db, "", config, error));
  CHECK(config.loadout == 0);
  CHECK(config.arena == 1);  // the rest of the config is left alone
  REQUIRE(resolveLoadoutName(db, "slam", config, error));
  CHECK(config.loadout == db.findLoadout("slam").value_or(0xFF));
  REQUIRE(resolveLoadoutName(db, "base", config, error));
  CHECK(config.loadout == 0);
  CHECK_FALSE(resolveLoadoutName(db, "zap", config, error));
  CHECK(contains(error, "unknown loadout 'zap'"));
  CHECK(contains(error, "base, blink, slam, parry"));
}

TEST_CASE("documents merge in order; lists append", "[content][loader]") {
  const std::string more = R"(
[[arena]]
name = "pit"
half_extents = [4, 4]
spawn_ellipse = { radii = [2, 2], step_degrees = 90, order = [0, 2] }

[[mode]]
name = "sudden"
rounds_to_win = 1
)";
  const std::array<ContentSource, 2> sources = {
      ContentSource{"a.toml", std::string(kMinimal)},
      ContentSource{"b.toml", more},
  };
  ContentDB db{};
  Diagnostics diag;
  const bool ok = parseContent(sources, db, diag);
  INFO(diag.format());
  REQUIRE(ok);
  REQUIRE(db.arenaCount == 2);
  CHECK(db.arenas[0].name.view() == "box");
  CHECK(db.arenas[1].name.view() == "pit");
  CHECK(db.arenas[1].spawnPointCount == 2);
  CHECK(db.arenas[1].spawnPoints[0] == Vec2{Fixed::fromInt(2), Fixed::zero()});
  CHECK(db.arenas[1].spawnPoints[1] == Vec2{-Fixed::fromInt(2), Fixed::zero()});
  REQUIRE(db.modeCount == 2);
  CHECK(db.findMode("sudden") == std::optional<uint8_t>{1});
  CHECK(db.modes[1].roundsToWin == 1);

  sim::MatchConfig config{.seed = 5, .players = 3};
  std::string error;
  REQUIRE(resolveMatchNames(db, "pit", "sudden", config, error));
  CHECK(config.arena == 1);
  CHECK(config.mode == 1);
  CHECK(config.seed == 5);
  REQUIRE(resolveMatchNames(db, "", "", config, error));
  CHECK(config.arena == 0);
  CHECK(config.mode == 0);
  CHECK_FALSE(resolveMatchNames(db, "lava", "", config, error));
  CHECK(contains(error, "unknown arena 'lava'"));
  CHECK(contains(error, "box, pit"));
  CHECK_FALSE(resolveMatchNames(db, "", "ctf", config, error));
  CHECK(contains(error, "rounds, sudden"));

  const sim::MatchContent m = sim::matchContent(db, sim::MatchConfig{.arena = 1, .mode = 1});
  CHECK(&m.arena == &db.arenas[1]);
  CHECK(&m.mode == &db.modes[1]);
}

TEST_CASE("--powerups lists resolve to a MatchConfig::powerUps set and back",
          "[content][app][powerup]") {
  sim::ContentDB db{};
  Diagnostics diag;
  REQUIRE(loadEmbeddedContent(db, diag));
  REQUIRE(db.powerUpCount >= 3);
  const std::string first(db.powerUps[0].name.view());
  const std::string third(db.powerUps[2].name.view());
  const std::string thirdFirst = third + ',' + first;
  const std::string firstThird = first + ',' + third;
  const std::string firstLava = first + ",lava";
  const std::string firstEmpty = first + ',';
  uint16_t set = 0x1234;
  std::string error;
  REQUIRE(resolvePowerUpList(db, "", set, error));
  CHECK(set == sim::kAllPowerUps);
  REQUIRE(resolvePowerUpList(db, "none", set, error));
  CHECK(set == 0);
  REQUIRE(resolvePowerUpList(db, "all", set, error));
  CHECK(set == sim::kAllPowerUps);
  REQUIRE(resolvePowerUpList(db, thirdFirst, set, error));
  CHECK(set == 0b101);
  CHECK(powerUpListText(db, set) == firstThird);
  CHECK(powerUpListText(db, sim::kAllPowerUps) == "all");
  CHECK(powerUpListText(db, 0) == "none");
  // Every power-up named one by one is "all" too (bits past the content
  // do not matter).
  const auto every = static_cast<uint16_t>((1u << db.powerUpCount) - 1u);
  CHECK(powerUpListText(db, every) == "all");

  set = 7;
  CHECK_FALSE(resolvePowerUpList(db, firstLava, set, error));
  CHECK(contains(error, "unknown power-up 'lava'"));
  CHECK(contains(error, first));
  CHECK(set == 7);  // unchanged on failure
  CHECK_FALSE(resolvePowerUpList(db, firstEmpty, set, error));
  CHECK(contains(error, "unknown power-up ''"));
}

TEST_CASE("syntax errors report file and line", "[content][loader]") {
  const std::string errors = loadErrors("[player]\nradius = = 1\n");
  CHECK(contains(errors, "test.toml:2: error: TOML syntax"));
}

TEST_CASE("unknown keys and sections are errors with lines", "[content][loader]") {
  CHECK(contains(loadErrors(replaced(kMinimal, "max_hp = 1", "max_hp = 1\nmax_hpp = 2")),
                 "test.toml:6: error: [player]: unknown key 'max_hpp'"));
  CHECK(contains(loadErrors(std::string(kMinimal) + "\n[shop]\nprice = 1\n"),
                 "unknown top-level key 'shop'"));
  CHECK(contains(loadErrors(replaced(kMinimal, "name = \"box\"", "name = \"box\"\nportals = []")),
                 "arena \"box\": unknown key 'portals'"));
}

TEST_CASE("arena walls and hazards load and are validated", "[content][loader][arena]") {
  const std::string withStatics =
      replaced(kMinimal, "name = \"box\"",
               "name = \"box\"\n"
               "walls = [{ min = [-1, -1], max = [1, 1] }, { min = [5, 2], max = [6, 5] }]\n"
               "hazards = [{ kind = \"pit\", min = [-10, -5], max = [-8, 5] },\n"
               "           { kind = \"slow\", min = [3, -5], max = [4, 5], speed_mul = 0.5 }]");
  ContentDB db{};
  Diagnostics diag;
  REQUIRE(parseContent(withStatics, "test.toml", db, diag));
  const sim::ArenaDef& a = db.arenas[0];
  REQUIRE(a.wallCount == 2);
  CHECK(a.walls[0].min == Vec2{Fixed::fromInt(-1), Fixed::fromInt(-1)});
  CHECK(a.walls[1].max == Vec2{Fixed::fromInt(6), Fixed::fromInt(5)});
  REQUIRE(a.hazardCount == 2);
  CHECK(a.hazards[0].kind == sim::HazardKind::Pit);
  CHECK(a.hazards[0].speedMul == Fixed::one());
  CHECK(a.hazards[1].kind == sim::HazardKind::Slow);
  CHECK(a.hazards[1].speedMul == Fixed::half());

  const auto with = [&](std::string_view from, std::string_view to) {
    return loadErrors(replaced(withStatics, from, to));
  };
  // A spawn point that puts a monkey (radius 0.5) inside or overlapping a wall.
  CHECK(contains(with("[[-2, 0], [2, 0]]", "[[-2, 0], [1.25, 0]]"),
                 "arena \"box\": spawn point 1 (1.250, 0.000) puts a monkey inside wall 0"));
  CHECK(contains(with("[[-2, 0], [2, 0]]", "[[-2, 0], [0, 0]]"), "inside wall 0"));
  // Touching is fine: 1.5 is exactly one radius from the wall face.
  {
    ContentDB ok{};
    Diagnostics okDiag;
    CHECK(parseContent(replaced(withStatics, "[[-2, 0], [2, 0]]", "[[-2, 0], [1.5, 0]]"),
                       "test.toml", ok, okDiag));
  }
  // A spawn point in a pit; a slow patch is allowed.
  CHECK(contains(with("[[-2, 0], [2, 0]]", "[[-9, 0], [2, 0]]"),
                 "spawn point 0 (-9.000, 0.000) is in pit hazard 0"));
  {
    ContentDB ok{};
    Diagnostics okDiag;
    CHECK(parseContent(replaced(withStatics, "[[-2, 0], [2, 0]]", "[[-2, 0], [3.5, 0]]"),
                       "test.toml", ok, okDiag));
  }
  // Boxes: empty, outside the arena.
  CHECK(contains(with("min = [5, 2], max = [6, 5]", "min = [6, 2], max = [5, 5]"),
                 "wall 1 needs min < max on both axes"));
  CHECK(contains(with("min = [5, 2], max = [6, 5]", "min = [5, 2], max = [11, 5]"),
                 "wall 1 reaches outside the arena box"));
  // Kinds and keys.
  CHECK(contains(with("kind = \"pit\"", "kind = \"lava\""), "unknown hazard kind \"lava\""));
  CHECK(contains(with("max = [-8, 5] }", "max = [-8, 5], speed_mul = 0.5 }"),
                 "hazard 0: unknown key 'speed_mul'"));
  CHECK(contains(with(", speed_mul = 0.5", ""), "hazard 1: missing required key 'speed_mul'"));
  CHECK(contains(with("speed_mul = 0.5", "speed_mul = 0"), "'speed_mul' = 0.000 is outside"));
  CHECK(contains(with("{ min = [-1, -1], max = [1, 1] }", "{ min = [-1, -1] }"),
                 "wall 0: missing required key 'max'"));
}

TEST_CASE("content/ arenas: dojo stays first, the walled arenas load", "[content][loader][arena]") {
  ContentDB db{};
  Diagnostics diag;
  REQUIRE(loadContentDir(KFM_CONTENT_DIR, db, diag));
  REQUIRE(db.arenaCount >= 4);
  CHECK(db.arenas[0].name.view() == "dojo");
  CHECK(db.arenas[0].wallCount == 0);
  CHECK(db.arenas[0].hazardCount == 0);
  // content/arenas/NN-name.toml load after arenas.toml, in prefix order.
  CHECK(db.findArena("pillars") == std::optional<uint8_t>{1});
  CHECK(db.findArena("corridors") == std::optional<uint8_t>{2});
  CHECK(db.findArena("island") == std::optional<uint8_t>{3});
  for (const std::string_view name : {"pillars", "corridors", "island"}) {
    INFO(name);
    REQUIRE(db.findArena(name).has_value());
    const sim::ArenaDef& a = db.arenas[db.findArena(name).value_or(0)];
    CHECK(a.wallCount > 0);
    CHECK(a.spawnPointCount == sim::kMaxPlayers);
  }
  const sim::ArenaDef& island = db.arenas[db.findArena("island").value_or(0)];
  CHECK(island.hazardCount == 6);
  CHECK(island.hazards[0].kind == sim::HazardKind::Pit);
}

TEST_CASE("names: format, duplicates and references", "[content][loader]") {
  CHECK(contains(loadErrors(replaced(kMinimal, "name = \"box\"", "name = \"Big Box\"")),
                 "'name' must be 1 to 15 characters"));
  CHECK(contains(loadErrors(replaced(kMinimal, "name = \"box\"", "name = \"a_very_long_arena\"")),
                 "'name' must be 1 to 15 characters"));
  CHECK(contains(loadErrors(std::string(kMinimal) + "[[mode]]\nname = \"rounds\"\n"),
                 "mode \"rounds\": mode \"rounds\" is defined twice"));
  CHECK(contains(loadErrors(replaced(kMinimal, "[\"dash\", \"throw\"]", "[\"dash\", \"toss\"]")),
                 "names unknown ability \"toss\""));
  CHECK(contains(loadErrors(replaced(kMinimal, "name = \"throw\"", "name = \"none\"")),
                 "built-in inert ability"));
  CHECK(contains(loadErrors(replaced(kMinimal, "kind = \"throw\"", "kind = \"yeet\"")),
                 "unknown kind \"yeet\""));
  // Buttons belong to slots (v0.21): any kind in any slot, but the dash and
  // throw buttons must fire something and a loadout lists 2 or 3 names.
  CHECK(loads(replaced(kMinimal, "[\"dash\", \"throw\"]", "[\"throw\", \"dash\"]")));
  CHECK(loads(replaced(kMinimal, "[\"dash\", \"throw\"]", "[\"dash\", \"throw\", \"none\"]")));
  CHECK(contains(loadErrors(replaced(kMinimal, "[\"dash\", \"throw\"]", "[\"none\", \"throw\"]")),
                 "the dash slot must hold an ability"));
  CHECK(contains(loadErrors(replaced(kMinimal, "[\"dash\", \"throw\"]", "[\"dash\"]")),
                 "'abilities' must list 2 or 3 ability names"));
  CHECK(contains(loadErrors(replaced(kMinimal, "[\"dash\", \"throw\"]",
                                     "[\"dash\", \"throw\", \"dash\", \"dash\"]")),
                 "'abilities' must list 2 or 3 ability names"));
  // Within one document TOML itself forbids it; across documents the loader does.
  CHECK(contains(loadErrors(std::string(kMinimal) + "\n[player]\nradius = 1\n"), "TOML syntax"));
  const std::array<ContentSource, 2> twice = {
      ContentSource{"a.toml", std::string(kMinimal)},
      ContentSource{"b.toml", "[player]\nradius = 1\n"},
  };
  ContentDB db{};
  Diagnostics diag;
  CHECK_FALSE(parseContent(twice, db, diag));
  CHECK(contains(diag.format(), "b.toml:1: error: [player] is defined again (first in a.toml:2)"));
}

TEST_CASE("values: types, ranges and capacities", "[content][loader]") {
  CHECK(contains(loadErrors(replaced(kMinimal, "max_hp = 1", "max_hp = 0")),
                 "'max_hp' must be an integer in [1, 255]"));
  CHECK(contains(loadErrors(replaced(kMinimal, "max_hp = 1", "max_hp = 1.5")),
                 "'max_hp' must be an integer"));
  CHECK(contains(loadErrors(replaced(kMinimal, "radius = 0.35", "radius = 3.5e-1")),
                 "'radius' = 3.5e-1: write a plain decimal"));
  CHECK(contains(loadErrors(replaced(kMinimal, "radius = 0.35", "radius = \"big\"")),
                 "'radius' must be a number"));
  CHECK(contains(loadErrors(replaced(kMinimal, "[10, 5]", "[2000, 5]")),
                 "'half_extents' must be in (0, 1024]"));
  CHECK(contains(loadErrors(replaced(kMinimal, "[[-2, 0], [2, 0]]", "[[-12, 0], [2, 0]]")),
                 "spawn point 0 (-12.000, 0.000) puts a monkey outside the arena"));
  CHECK(contains(loadErrors(replaced(kMinimal, "spawn_offset = 1", "spawn_offset = 0.5")),
                 "spawn_offset must exceed"));
  CHECK(contains(loadErrors(replaced(kMinimal, "min_ammo = 1", "min_ammo = 0")),
                 "min_ammo must be at least 1"));
  CHECK(contains(
      loadErrors(replaced(kMinimal, "perfect_window_ticks = 4", "perfect_window_ticks = 13")),
      "a dash needs active_ticks >= 1 and perfect_window_ticks <= active_ticks"));
  // v0.34: a window of 0 (never perfect) and one spanning the active phase load.
  CHECK(loads(replaced(kMinimal, "perfect_window_ticks = 4", "perfect_window_ticks = 0")));
  CHECK(loads(replaced(kMinimal, "perfect_window_ticks = 4", "perfect_window_ticks = 12")));
  // v0.57: the banana economy; the score keys only in a point-scoring kind.
  CHECK(loads(replaced(kMinimal, "name = \"rounds\"",
                       "name = \"rounds\"\ndeath_bananas = \"vanish\"\nbanana_regrow_ticks = 9")));
  CHECK(contains(loadErrors(replaced(kMinimal, "name = \"rounds\"",
                                     "name = \"rounds\"\ndeath_bananas = \"keep\"")),
                 "unknown death_bananas \"keep\" (drop or vanish)"));
  CHECK(contains(
      loadErrors(replaced(kMinimal, "name = \"rounds\"", "name = \"rounds\"\ncatch_score = 1")),
      "'catch_score' belongs to a point-scoring kind"));
  CHECK(loads(replaced(kMinimal, "name = \"rounds\"",
                       "name = \"rounds\"\nkind = \"deathmatch\"\nsuicide_score = -1")));
  CHECK(contains(loadErrors(replaced(kMinimal, "name = \"rounds\"",
                                     "name = \"rounds\"\nkind = \"deathmatch\"\n"
                                     "suicide_score = -129")),
                 "'suicide_score' must be an integer in [-128, 127]"));
  // v0.34: the space, and the grid's keys only in a grid mode.
  CHECK(contains(
      loadErrors(replaced(kMinimal, "name = \"rounds\"", "name = \"rounds\"\nspace = \"hex\"")),
      "unknown space \"hex\" (continuous or grid)"));
  CHECK(contains(
      loadErrors(replaced(kMinimal, "name = \"rounds\"", "name = \"rounds\"\nturn_ticks = 15")),
      "'turn_ticks' belongs to a grid mode (give space = \"grid\")"));
  CHECK(contains(loadErrors(replaced(kMinimal, "name = \"rounds\"",
                                     "name = \"rounds\"\nspace = \"grid\"\nturn_ticks = 61")),
                 "'turn_ticks' must be an integer in [1, 60]"));
  CHECK(contains(loadErrors(replaced(kMinimal, "name = \"rounds\"",
                                     "name = \"rounds\"\nspace = \"grid\"\ndash_cells = 0")),
                 "'dash_cells' must be an integer in [1, 8]"));
  {
    ContentDB db{};
    Diagnostics diag;
    REQUIRE(parseContent(replaced(kMinimal, "name = \"rounds\"",
                                  "name = \"rounds\"\nspace = \"grid\"\nturn_ticks = 4\n"
                                  "dash_cells = 3"),
                         "test.toml", db, diag));
    CHECK(db.modes[0].space == sim::Space::Grid);
    CHECK(db.modes[0].turnTicks == 4);
    CHECK(db.modes[0].dashCells == 3);
    CHECK_FALSE(db.compatible(0, 0));  // a hand-placed arena has no cells
  }
  // v0.34: a mode's own arena must exist and suit it.
  CHECK(loads(replaced(kMinimal, "name = \"rounds\"", "name = \"rounds\"\narena = \"box\"")));
  CHECK(contains(
      loadErrors(replaced(kMinimal, "name = \"rounds\"", "name = \"rounds\"\narena = \"nope\"")),
      "'arena' names unknown arena \"nope\""));
  CHECK(contains(loadErrors(replaced(kMinimal, "name = \"rounds\"",
                                     "name = \"rounds\"\nspace = \"grid\"\narena = \"box\"")),
                 "its arena \"box\" does not suit it"));
  CHECK(contains(
      loadErrors(replaced(kMinimal, "active_ticks = 12", "active_ticks = 12\nstartup_ticks = 1")),
      "startup_ticks = 0"));
  CHECK(contains(loadErrors(replaced(kMinimal, "kind = \"dash\"",
                                     "kind = \"dash\"\ntags_active = [\"flying\"]")),
                 "unknown tag in 'tags_active'"));
  // v0.34: root_phases is a list of phase names.
  CHECK(contains(loadErrors(replaced(kMinimal, "kind = \"throw\"",
                                     "kind = \"throw\"\nroot_phases = [\"flying\"]")),
                 "'root_phases' entries must be startup, active or recovery"));
  {
    ContentDB db{};
    Diagnostics diag;
    REQUIRE(parseContent(replaced(kMinimal, "kind = \"throw\"",
                                  "kind = \"throw\"\nroot_phases = [\"recovery\", \"startup\"]"),
                         "test.toml", db, diag));
    uint8_t thr = 0;
    for (uint8_t i = 0; i < db.abilityCount; ++i) {
      if (db.abilities[i].name.view() == "throw") thr = i;
    }
    REQUIRE(thr != 0);
    CHECK(db.abilities[thr].rootPhases ==
          (sim::phaseBit(sim::AbilityPhase::Recovery) | sim::phaseBit(sim::AbilityPhase::Startup)));
  }
  CHECK(contains(
      loadErrors(replaced(kMinimal, "spawn_points = [[-2, 0], [2, 0]]",
                          "spawn_points = [[-2, 0], [2, 0]]\nspawn_ellipse = { radii = [1, 1], "
                          "step_degrees = 90, order = [0] }")),
      "exactly one of 'spawn_points' or 'spawn_ellipse'"));

  std::string manyArenas(kMinimal);
  for (uint32_t i = 0; i < sim::kMaxArenas; ++i) {
    manyArenas += "[[arena]]\nname = \"a" + std::to_string(i) +
                  "\"\nhalf_extents = [10, 5]\nspawn_points = [[0, 0]]\n";
  }
  CHECK(contains(loadErrors(manyArenas), "too many arenas (at most 16, kMaxArenas)"));
}

// --- power-ups (§5.8, v0.20) ------------------------------------------------

namespace {

// kMinimal plus one power-up and an arena with one spawner; tests break one
// piece at a time.
std::string withPowerUp() {
  return std::string(kMinimal) + R"(
[[ability]]
name = "throw_fast"
kind = "throw"
cooldown_ticks = 8

[[arena]]
name = "box_pu"
half_extents = [10, 5]
spawn_points = [[-2, 0], [2, 0]]
spawners = [{ pos = [3, 1], power_ups = ["boost"], first_ticks = 60, interval_ticks = 300 }]

[[powerup]]
name = "boost"
duration_ticks = 300
color = 6
shape = "diamond"
grants = [
  { kind = "stat", stat = "move_speed", mul = 1.5 },
  { kind = "ability", slot = "throw", ability = "throw_fast" },
]
)";
}

}  // namespace

TEST_CASE("power-ups load: grants, visuals, overrides and spawners resolve by name",
          "[content][loader][powerup]") {
  ContentDB db{};
  Diagnostics diag;
  const bool ok = parseContent(withPowerUp(), "test.toml", db, diag);
  INFO(diag.format());
  REQUIRE(ok);
  REQUIRE(db.powerUpCount == 1);
  const sim::PowerUpDef& d = db.powerUps[0];
  CHECK(d.name.view() == "boost");
  CHECK(d.durationTicks == 300);
  CHECK(d.visual.color == 6);
  CHECK(d.visual.shape == sim::PowerUpShape::Diamond);
  REQUIRE(d.grantCount == 2);
  CHECK(d.grants[0].kind == sim::GrantKind::Stat);
  CHECK(d.grants[0].stat.stat == sim::StatKind::MoveSpeed);
  CHECK(d.grants[0].stat.mul == Fixed::fromRatio(3, 2));
  CHECK(d.grants[1].kind == sim::GrantKind::AbilityOverride);
  CHECK(d.grants[1].slot == sim::kAbilitySlotThrow);
  CHECK(d.grants[1].ability == 3);
  REQUIRE(db.findArena("box_pu"));
  const sim::ArenaDef& a = db.arenas[db.findArena("box_pu").value_or(0)];
  REQUIRE(a.pickupSpawnerCount == 1);
  CHECK(a.pickupSpawners[0].pos == Vec2{Fixed::fromInt(3), Fixed::one()});
  CHECK(a.pickupSpawners[0].powerUpSet == 1);
  CHECK(a.pickupSpawners[0].firstTicks == 60);
  CHECK(a.pickupSpawners[0].intervalTicks == 300);
  CHECK(a.pickupSpawners[0].maxAlive == 1);
  CHECK(db.arenas[0].pickupSpawnerCount == 0);
}

TEST_CASE("power-up validation", "[content][loader][powerup]") {
  // References.
  CHECK(contains(
      loadErrors(replaced(withPowerUp(), "ability = \"throw_fast\"", "ability = \"throw_faster\"")),
      "'ability' names unknown ability \"throw_faster\""));
  CHECK(contains(
      loadErrors(replaced(withPowerUp(), "power_ups = [\"boost\"]", "power_ups = [\"bost\"]")),
      "'power_ups' names unknown power-up \"bost\""));
  // Slots and kinds.
  CHECK(contains(loadErrors(replaced(withPowerUp(), "slot = \"throw\"", "slot = \"jump\"")),
                 "'slot' must be one of: dash, throw"));
  // Any kind may override any slot (v0.21: buttons belong to slots).
  CHECK(loads(replaced(withPowerUp(), "slot = \"throw\"", "slot = \"dash\"")));
  CHECK(loads(replaced(withPowerUp(), "slot = \"throw\"", "slot = \"ability\"")));
  CHECK(contains(loadErrors(replaced(withPowerUp(), "kind = \"stat\"", "kind = \"buff\"")),
                 "unknown grant kind \"buff\""));
  // Integer stats are add-only and whole.
  CHECK(contains(loadErrors(replaced(withPowerUp(), "stat = \"move_speed\", mul = 1.5",
                                     "stat = \"max_hp\", mul = 2")),
                 "'max_hp' is an integer stat: it takes 'add' only, never 'mul'"));
  CHECK(contains(loadErrors(replaced(withPowerUp(), "stat = \"move_speed\", mul = 1.5",
                                     "stat = \"ammo_max\", add = 1.5")),
                 "'ammo_max' is an integer stat: 'add' must be whole"));
  CHECK(contains(loadErrors(replaced(withPowerUp(), "stat = \"move_speed\"", "stat = \"luck\"")),
                 "'stat' must be one of: move_speed, max_hp, ammo_max, radius"));
  // v0.34: the radius stat takes a mul in (0, 4] and an add.
  CHECK(loads(replaced(withPowerUp(), "stat = \"move_speed\", mul = 1.5",
                       "stat = \"radius\", mul = 0.7, add = -0.05")));
  CHECK(contains(loadErrors(replaced(withPowerUp(), "stat = \"move_speed\", mul = 1.5",
                                     "stat = \"radius\", mul = 5")),
                 "'radius' takes a 'mul' in (0, 4]"));
  CHECK(contains(loadErrors(replaced(withPowerUp(), "stat = \"move_speed\", mul = 1.5",
                                     "stat = \"radius\", mul = 0")),
                 "'radius' takes a 'mul' in (0, 4]"));
  // Tags a power-up may not grant.
  CHECK(contains(
      loadErrors(replaced(withPowerUp(), "{ kind = \"stat\", stat = \"move_speed\", mul = 1.5 }",
                          "{ kind = \"tag\", tag = \"hit_stun\" }")),
      "can grant only 'shielded', 'invulnerable' or 'friendly_fire'"));
  // Values and shapes.
  CHECK(contains(loadErrors(replaced(withPowerUp(), "duration_ticks = 300", "duration_ticks = 0")),
                 "'duration_ticks' must be an integer in [1, 65534]"));
  CHECK(contains(loadErrors(replaced(withPowerUp(), "shape = \"diamond\"", "shape = \"blob\"")),
                 "'shape' must be one of: orb, cube, diamond, coin"));
  CHECK(contains(loadErrors(replaced(withPowerUp(), "color = 6", "color = 10")),
                 "'color' must be an integer in [0, 9]"));
  CHECK(contains(loadErrors(replaced(withPowerUp(), "mul = 1.5 }", "mul = 1.5, bonus = 1 }")),
                 "unknown key 'bonus'"));
  CHECK(contains(
      loadErrors(replaced(withPowerUp(), "{ kind = \"stat\", stat = \"move_speed\", mul = 1.5 }",
                          "{ kind = \"projectile\", radius_mul = 5 }")),
      "'radius_mul' = 5.000 is outside"));
  // v0.80: the projectile speed multiplier has its own limit, 8; the radius
  // multiplier keeps 4.
  const auto projectile = [](std::string_view grant) {
    return replaced(withPowerUp(), R"({ kind = "stat", stat = "move_speed", mul = 1.5 })", grant);
  };
  CHECK(loads(projectile(R"({ kind = "projectile", speed_mul = 8, radius_mul = 4 })")));
  CHECK(loads(projectile(R"({ kind = "projectile", speed_mul = 7.5 })")));
  CHECK(contains(loadErrors(projectile(R"({ kind = "projectile", speed_mul = 8.5 })")),
                 "'speed_mul' = 8.500 is outside"));
  CHECK(contains(
      loadErrors(projectile(R"({ kind = "projectile", speed_mul = 8, radius_mul = 4.5 })")),
      "'radius_mul' = 4.500 is outside"));
  {
    // validateContent() holds content built in code to the same limits.
    ContentDB db{};
    Diagnostics diag;
    REQUIRE(
        parseContent(projectile(R"({ kind = "projectile", speed_mul = 8 })"), "p.toml", db, diag));
    sim::PowerUpGrant* grant = nullptr;
    for (uint8_t g = 0; g < db.powerUps[0].grantCount; ++g) {
      if (db.powerUps[0].grants[g].kind == sim::GrantKind::Projectile) {
        grant = &db.powerUps[0].grants[g];
      }
    }
    if (grant == nullptr) {
      FAIL("no projectile grant");
      return;
    }
    Diagnostics ok;
    CHECK(validateContent(db, ok));
    grant->projectile.speedMul = Fixed::fromInt(8) + Fixed::epsilon();
    Diagnostics fast;
    CHECK_FALSE(validateContent(db, fast));
    CHECK(fast.format().contains("speed_mul must be in (0, 8] and radius_mul in (0, 4]"));
    grant->projectile.speedMul = Fixed::fromInt(8);
    grant->projectile.radiusMul = Fixed::fromInt(4) + Fixed::epsilon();
    Diagnostics wide;
    CHECK_FALSE(validateContent(db, wide));
    CHECK(wide.format().contains("speed_mul must be in (0, 8] and radius_mul in (0, 4]"));
  }
  // Spawners.
  CHECK(contains(loadErrors(replaced(withPowerUp(), "pos = [3, 1]", "pos = [12, 1]")),
                 "spawner 0: pos (12.000, 1.000) is outside the arena"));
  CHECK(contains(loadErrors(replaced(withPowerUp(), "interval_ticks = 300 }",
                                     "interval_ticks = 300, max_alive = 17 }")),
                 "max_alive add up to 17"));
  CHECK(contains(loadErrors(std::string(kMinimal) +
                            "[[arena]]\nname = \"bare\"\nhalf_extents = [10, 5]\n"
                            "spawn_points = [[0, 0]]\nspawners = [{ pos = [1, 1] }]\n"),
                 "has spawners but no [[powerup]] is defined"));
  // v0.22 statics: a spawner inside a wall or over a pit.
  CHECK(contains(loadErrors(replaced(withPowerUp(), "spawners = [",
                                     "walls = [{ min = [2, 0], max = [4, 2] }]\nspawners = [")),
                 "spawner 0: pos (3.000, 1.000) is inside wall 0"));
  CHECK(contains(loadErrors(replaced(
                     withPowerUp(), "spawners = [",
                     "hazards = [{ kind = \"pit\", min = [2, 0], max = [4, 2] }]\nspawners = [")),
                 "spawner 0: pos (3.000, 1.000) is in pit hazard 0"));
}

// --- trees (v0.81) --------------------------------------------------------

namespace {

// kMinimal plus a tilemap arena with one tree and no [[powerup]] at all.
std::string withTree() {
  return std::string(kMinimal) + R"(
[[arena]]
name = "orchard"
cell = 1.0
map = [
  "........",
  ".0....1.",
  "........",
  "........",
]
spawners = [{ pos = [0.5, 0.5], tree = true, first_ticks = 30, interval_ticks = 90, max_alive = 2 }]
)";
}

}  // namespace

TEST_CASE("trees load: tree = true, no power-ups needed", "[content][loader][tree]") {
  ContentDB db{};
  Diagnostics diag;
  const bool ok = parseContent(withTree(), "test.toml", db, diag);
  INFO(diag.format());
  REQUIRE(ok);
  REQUIRE(db.findArena("orchard"));
  const sim::ArenaDef& a = db.arenas[db.findArena("orchard").value_or(0)];
  REQUIRE(a.pickupSpawnerCount == 1);
  const sim::PickupSpawner& sp = a.pickupSpawners[0];
  CHECK(sp.tree == 1);
  CHECK(sp.isTree());
  CHECK(sp.powerUpSet == 0);
  CHECK(sp.firstTicks == 30);
  CHECK(sp.intervalTicks == 90);
  CHECK(sp.maxAlive == 2);
  CHECK(sim::hasTree(a));
  CHECK_FALSE(sim::hasTree(db.arenas[0]));
  // tree = false is a power-up spawner, and the default.
  ContentDB off{};
  Diagnostics offDiag;
  REQUIRE(
      parseContent(replaced(withPowerUp(), "first_ticks = 60", "tree = false, first_ticks = 60"),
                   "test.toml", off, offDiag));
  CHECK(off.arenas[off.findArena("box_pu").value_or(0)].pickupSpawners[0].tree == 0);
  CHECK(off.arenas[off.findArena("box_pu").value_or(0)].pickupSpawners[0].powerUpSet == 1);
}

TEST_CASE("tree validation", "[content][loader][tree]") {
  // A tree takes no power-ups.
  CHECK(contains(loadErrors(replaced(withPowerUp(), "power_ups = [\"boost\"]",
                                     "tree = true, power_ups = [\"boost\"]")),
                 "'power_ups' belongs to a power-up spawner: a tree lays bananas"));
  // A tree needs cells: the hand-placed box_pu has none.
  CHECK(
      contains(loadErrors(replaced(withPowerUp(), "power_ups = [\"boost\"]", "tree = true")),
               "arena \"box_pu\" spawner 0: a tree needs an arena with cells (a tilemap, `cell`)"));
  // A boolean.
  CHECK(contains(loadErrors(replaced(withTree(), "tree = true", "tree = 1")),
                 "'tree' must be true or false"));
  // The statics' checks hold for a tree as for any spawner.
  CHECK(contains(loadErrors(replaced(withTree(), "pos = [0.5, 0.5]", "pos = [9.5, 0.5]")),
                 "spawner 0: pos (9.500, 0.500) is outside the arena"));
  // A tree's max_alive does not count toward the power-ups' half of the pool.
  CHECK(loads(replaced(withTree(), "max_alive = 2", "max_alive = 32")));
  CHECK(contains(loadErrors(replaced(withTree(), "max_alive = 2", "max_alive = 33")),
                 "'max_alive' must be an integer in [1, 32]"));
  {
    // validateContent() holds content built in code to the same rules.
    ContentDB db{};
    Diagnostics diag;
    REQUIRE(parseContent(withTree(), "t.toml", db, diag));
    sim::ArenaDef& a = db.arenas[db.findArena("orchard").value_or(0)];
    Diagnostics ok;
    CHECK(validateContent(db, ok));
    a.pickupSpawners[0].tree = 2;
    Diagnostics two;
    CHECK_FALSE(validateContent(db, two));
    CHECK(two.format().contains("tree is a 0/1 switch"));
    a.pickupSpawners[0].tree = 1;
    a.pickupSpawners[0].powerUpSet = 1;
    Diagnostics set;
    CHECK_FALSE(validateContent(db, set));
    CHECK(set.format().contains("a tree takes no power_ups"));
    a.pickupSpawners[0].powerUpSet = 0;
    a.cellSize = Fixed::zero();
    Diagnostics cells;
    CHECK_FALSE(validateContent(db, cells));
    CHECK(cells.format().contains("a tree needs an arena with cells"));
  }
}

TEST_CASE("content/ ships the power-ups and an arena that spawns them",
          "[content][loader][powerup]") {
  ContentDB db{};
  Diagnostics diag;
  REQUIRE(loadContentDir(KFM_CONTENT_DIR, db, diag));
  // v0.34 appended quick_hands and the grid modes' two baselines, v0.80 the
  // contest's baseline and the _1t twins its arena lays, v0.81 the harvest
  // baseline.
  const std::array<std::string_view, 14> names = {
      "speed_boost",     "big_banana",     "triple_banana", "shield",       "fast_hands",
      "quick_hands",     "grid_base",      "grid_base_1t",  "contest_base", "big_banana_1t",
      "triplebanana_1t", "quick_hands_1t", "shield_1t",     "harvest_base"};
  REQUIRE(db.powerUpCount == names.size());
  for (size_t i = 0; i < names.size(); ++i)
    CHECK(db.powerUps[i].name.view() == names[i]);
  CHECK(db.arenas[0].pickupSpawnerCount == 0);  // the default match has no power-ups
  const std::optional<uint8_t> arena = db.findArena("dojo_powerups");
  REQUIRE(arena);
  CHECK(db.arenas[arena.value_or(0)].pickupSpawnerCount == 4);
  // dojo_powerups is the dojo plus spawners.
  CHECK(db.arenas[arena.value_or(0)].halfExtents == db.arenas[0].halfExtents);
  CHECK(db.arenas[arena.value_or(0)].spawnPoints == db.arenas[0].spawnPoints);
  const sim::PowerUpGrant& hands = db.powerUps[db.findPowerUp("fast_hands").value_or(0)].grants[0];
  CHECK(hands.kind == sim::GrantKind::AbilityOverride);
  CHECK(db.abilities[hands.ability].name.view() == "throw_fast");
  // v0.34: its spawners name the five they always drew from, so the grid's
  // power-ups never join their draws.
  for (uint8_t k = 0; k < db.arenas[arena.value_or(0)].pickupSpawnerCount; ++k) {
    CHECK(db.arenas[arena.value_or(0)].pickupSpawners[k].powerUpSet == 0x1F);
  }
}

// v0.34 (architecture §5.12): the grid content and the numbers §5.12 starts
// with, as content/ ships them.
TEST_CASE("content/ ships the grid modes, their kits and the courtyard",
          "[content][loader][grid]") {
  ContentDB db{};
  Diagnostics diag;
  REQUIRE(loadContentDir(KFM_CONTENT_DIR, db, diag));
  const std::optional<uint8_t> courtyard = db.findArena("courtyard");
  REQUIRE(courtyard);
  const sim::ArenaDef& a = db.arenas[courtyard.value_or(0)];
  CHECK(a.cellSize == Fixed::one());
  CHECK(a.spawnGroups == 2);
  CHECK(a.spawnPointCount == 10);
  // Every tilemap's spawn points sit on cell centres (grid_cells.hpp's
  // mapping is the loader's), so a grid monkey spawns at rest on a cell.
  for (uint8_t i = 0; i < db.arenaCount; ++i) {
    const sim::ArenaDef& t = db.arenas[i];
    if (!sim::grid::hasCells(t)) continue;
    INFO(t.name.view());
    for (uint8_t k = 0; k < t.spawnPointCount; ++k) {
      const sim::grid::Cell cell = sim::grid::cellOf(t, t.spawnPoints[k]);
      CHECK(sim::grid::inside(t, cell));
      CHECK(sim::grid::cellCentre(t, cell) == t.spawnPoints[k]);
    }
  }
  for (const std::string_view name : {"grid_rounds", "grid_rounds_1t"}) {
    INFO(name);
    const std::optional<uint8_t> mi = db.findMode(name);
    REQUIRE(mi);
    const sim::ModeDef& m = db.modes[mi.value_or(0)];
    CHECK(m.space == sim::Space::Grid);
    CHECK(m.kind == sim::ModeKind::TeamRounds);
    CHECK(m.teams == 2);
    CHECK(m.dashCells == 2);
    CHECK(m.arena == courtyard);
    CHECK(db.firstCompatibleArena(mi.value_or(0)) == courtyard);
    CHECK(db.compatible(courtyard.value_or(0), mi.value_or(0)));
    CHECK_FALSE(db.compatible(0, mi.value_or(0)));  // the dojo has no cells
    REQUIRE(m.roleCount == 5);
    REQUIRE(m.hasBaseline());
    const bool fast = name == "grid_rounds_1t";
    CHECK(m.turnTicks == (fast ? 1 : 15));
    // One catcher per team, then the base grid kit.
    const sim::LoadoutDef& catcher = db.loadouts[m.roles[0]];
    const sim::LoadoutDef& base = db.loadouts[m.roles[1]];
    CHECK(catcher.name.view() == (fast ? "catcher_1t" : "catcher"));
    CHECK(base.name.view() == (fast ? "grid_1t" : "grid"));
    for (uint8_t r = 1; r < m.roleCount; ++r)
      CHECK(m.roles[r] == m.roles[1]);
    const sim::AbilityDef& dash = db.abilities[base.abilities[sim::kAbilitySlotDash]];
    const sim::AbilityDef& catchDash = db.abilities[catcher.abilities[sim::kAbilitySlotDash]];
    const sim::AbilityDef& thr = db.abilities[base.abilities[sim::kAbilitySlotThrow]];
    CHECK(dash.perfectWindowTicks == 0);
    CHECK(dash.activeTicks == m.turnTicks);  // two cells in one turn
    CHECK(catchDash.perfectWindowTicks == catchDash.activeTicks);
    CHECK(catchDash.cooldownTicks == 4 * m.turnTicks);  // four turns
    CHECK(thr.recoveryTicks == m.turnTicks);
    CHECK(thr.rootPhases == sim::phaseBit(sim::AbilityPhase::Recovery));
    // v0.35: every grid kit calls once a turn, as far as the cone reaches,
    // and the bots see under `own` fog.
    for (const sim::LoadoutDef* kit : {&catcher, &base}) {
      const sim::AbilityDef& call = db.abilities[kit->abilities[sim::kAbilitySlotExtra]];
      CHECK(call.kind == sim::AbilityKind::Call);
      CHECK(call.cooldownTicks == m.turnTicks);
      CHECK(call.param0 == Fixed::fromInt(m.vision.cone) * a.cellSize);
    }
    CHECK(m.vision.kind == sim::VisionKind::Own);
    CHECK(m.vision.peripheral == 1);
    CHECK(m.vision.cone == 4);
    CHECK(m.vision.focus == 1);
    CHECK(m.vision.hearing == 3);
    CHECK(m.vision.bananaSight == 1);
    // The baseline: monkeys of 0.35, bananas of 0.15, HP 2.
    const sim::PowerUpDef& baseline = db.powerUps[m.baseline];
    Fixed radiusMul = Fixed::one();
    int32_t hp = 0;
    Fixed bananaMul = Fixed::one();
    Fixed speedMul = Fixed::one();
    for (uint8_t g = 0; g < baseline.grantCount; ++g) {
      const sim::PowerUpGrant& grant = baseline.grants[g];
      if (grant.kind == sim::GrantKind::Stat && grant.stat.stat == sim::StatKind::Radius)
        radiusMul = grant.stat.mul;
      if (grant.kind == sim::GrantKind::Stat && grant.stat.stat == sim::StatKind::MaxHp)
        hp = grant.stat.add.floorToInt();
      if (grant.kind == sim::GrantKind::Projectile) {
        bananaMul = grant.projectile.radiusMul;
        speedMul = grant.projectile.speedMul;
      }
    }
    CHECK(db.player.radius * radiusMul == Fixed::fromRatio(35, 100));
    CHECK(db.boomerang.radius * bananaMul == Fixed::fromRatio(15, 100));
    CHECK(db.player.maxHp + hp == 2);
    if (!fast) {
      // 2 cells per 15-tick turn, within the per-tick truncation.
      const Fixed perTurn = db.boomerang.speed * speedMul * Fixed::fromInt(15);
      CHECK(abs(perTurn - Fixed::fromInt(2)) < Fixed::fromRatio(1, 1000));
    }
  }
  // Quick Hands overrides the throw with an unrooted def.
  const std::optional<uint8_t> quick = db.findPowerUp("quick_hands");
  REQUIRE(quick);
  const sim::PowerUpGrant& g = db.powerUps[quick.value_or(0)].grants[0];
  CHECK(g.kind == sim::GrantKind::AbilityOverride);
  CHECK(g.slot == sim::kAbilitySlotThrow);
  CHECK(db.abilities[g.ability].rootPhases == 0);
}

TEST_CASE("a bad content directory is reported", "[content][loader]") {
  ContentDB db{};
  Diagnostics diag;
  CHECK_FALSE(loadContentDir("/nonexistent/kfm-content", db, diag));
  CHECK(contains(diag.format(), "not a content directory"));
}

TEST_CASE("Diagnostics caps the list but keeps counting", "[content][loader]") {
  Diagnostics diag;
  for (size_t i = 0; i < Diagnostics::kMaxItems + 5; ++i)
    diag.error("f.toml", 1, "x");
  diag.warning("f.toml", 2, "w");
  CHECK(diag.items().size() == Diagnostics::kMaxItems);
  CHECK(diag.errorCount() == Diagnostics::kMaxItems + 5);
  CHECK(diag.dropped() == 6);
  CHECK(contains(diag.format(), "... and 6 more"));
}

TEST_CASE("app content: --content wins, else the embedded copy", "[content][app]") {
  LoadedContent c;
  Diagnostics diag;
  REQUIRE(loadAppContent("", "", c, diag));
  CHECK(c.embedded());
  CHECK(c.hash == sim::contentHash(c.db));
  const uint32_t embeddedHash = c.hash;
  CHECK(contains(c.describe(), "content: embedded (hash 0x"));
  CHECK(contains(c.describe(), " arena"));

  REQUIRE(loadAppContent(KFM_CONTENT_DIR, "", c, diag));
  CHECK_FALSE(c.embedded());
  CHECK(c.hash == embeddedHash);

  // An explicit directory that does not load is an error, not a fallback.
  CHECK_FALSE(loadAppContent("/nonexistent/kfm-content", "", c, diag));
}

TEST_CASE("app content: <exe dir>/content is found, and the watcher sees edits", "[content][app]") {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path exeDir = fs::temp_directory_path(ec) / "kfm_content_test_exe";
  REQUIRE_FALSE(ec);
  fs::remove_all(exeDir, ec);
  const fs::path dir = exeDir / "content";
  fs::create_directories(dir / "sub", ec);
  REQUIRE_FALSE(ec);
  CHECK(findContentDir("", exeDir).empty());  // no *.toml yet: embedded
  CHECK(findContentDir("elsewhere", exeDir) == fs::path("elsewhere"));
  {
    std::ofstream(dir / "sub" / "all.toml") << kMinimal;
  }
#ifndef __EMSCRIPTEN__
  CHECK(findContentDir("", exeDir) == dir);
#endif

  ContentWatcher watcher(dir, 1.0);
  CHECK(watcher.active());
  CHECK_FALSE(watcher.poll(0.0));  // nothing changed since construction
  {
    std::ofstream(dir / "sub" / "all.toml", std::ios::app) << "# tuned\n";
  }
  CHECK_FALSE(watcher.poll(0.5));  // checks at most once per interval
  CHECK(watcher.poll(1.0));
  CHECK_FALSE(watcher.poll(2.0));
  {
    std::ofstream(dir / "extra.toml") << "# a new file\n";
  }
  CHECK(watcher.poll(3.0));
  CHECK_FALSE(ContentWatcher().active());
  fs::remove_all(exeDir, ec);
}

TEST_CASE("modes: kinds, per-kind numbers and their validation", "[content][loader][modes]") {
  const std::string modes = R"(
[[mode]]
name = "dm"
kind = "deathmatch"
score_to_win = 7
time_limit_ticks = 7200
respawn_ticks = 90
respawn_invuln_ticks = 30

[[mode]]
name = "last"
kind = "survival"
rounds_to_win = 2
time_limit_ticks = 1800

[[mode]]
name = "r0"
score_to_win = 0
)";
  ContentDB db{};
  Diagnostics diag;
  const bool ok = parseContent(std::string(kMinimal) + modes, "modes.toml", db, diag);
  INFO(diag.format());
  REQUIRE(ok);
  REQUIRE(db.modeCount == 4);
  CHECK(db.modes[0].kind == sim::ModeKind::Rounds);  // kind defaults to rounds
  const sim::ModeDef& dm = db.modes[1];
  CHECK(dm.kind == sim::ModeKind::Deathmatch);
  CHECK(dm.scoreToWin == 7);
  CHECK(dm.timeLimitTicks == 7200);
  CHECK(dm.respawnTicks == 90);
  CHECK(dm.respawnInvulnTicks == 30);
  const sim::ModeDef& last = db.modes[2];
  CHECK(last.kind == sim::ModeKind::Survival);
  CHECK(last.roundsToWin == 2);
  CHECK(last.timeLimitTicks == 1800);
  // score_to_win = 0 means nothing outside deathmatch.
  CHECK(db.modes[3].kind == sim::ModeKind::Rounds);

  const std::string base(kMinimal);
  CHECK(contains(loadErrors(base + "[[mode]]\nname = \"ctf\"\nkind = \"ctf\"\n"),
                 "mode \"ctf\": unknown kind \"ctf\""));
  CHECK(contains(loadErrors(base + "[[mode]]\nname = \"dm\"\nkind = \"deathmatch\"\n"
                                   "score_to_win = 0\n"),
                 "mode \"dm\": score_to_win must be at least 1 in deathmatch"));
  CHECK(contains(loadErrors(base + "[[mode]]\nname = \"dm\"\nkind = \"deathmatch\"\n"
                                   "respawn_ticks = 0\n"),
                 "mode \"dm\": respawn_ticks must be at least 1 in deathmatch"));
  CHECK(
      contains(loadErrors(base + "[[mode]]\nname = \"x\"\nscore_to_win = 256\n"), "score_to_win"));
  CHECK(contains(loadErrors(base + "[[mode]]\nname = \"x\"\nscore_limit = 3\n"),
                 "unknown key 'score_limit'"));

  // v0.80: a mode may override the banana's flight (0, the default, keeps
  // the [boomerang] def's), within the [boomerang] keys' bounds.
  {
    ContentDB flight{};
    Diagnostics flightDiag;
    REQUIRE(parseContent(base + "[[mode]]\nname = \"far\"\nbanana_outbound_ticks = 45\n"
                                "banana_turn_rate = 8000\n",
                         "modes.toml", flight, flightDiag));
    CHECK(flight.modes[0].bananaOutboundTicks == 0);
    CHECK(flight.modes[0].bananaTurnRateBam == 0);
    CHECK(flight.modes[1].bananaOutboundTicks == 45);
    CHECK(flight.modes[1].bananaTurnRateBam == 8000);
    CHECK(sim::bananaOutboundTicksOf(flight.boomerang, flight.modes[0]) ==
          flight.boomerang.outboundTicks);
    CHECK(sim::bananaOutboundTicksOf(flight.boomerang, flight.modes[1]) == 45);
    CHECK(sim::bananaTurnRateBamOf(flight.boomerang, flight.modes[0]) ==
          flight.boomerang.turnRateBam);
    CHECK(sim::bananaTurnRateBamOf(flight.boomerang, flight.modes[1]) == 8000);
  }
  CHECK(loads(base + "[[mode]]\nname = \"x\"\nbanana_outbound_ticks = 65535\n"
                     "banana_turn_rate = 32768\n"));
  CHECK(contains(loadErrors(base + "[[mode]]\nname = \"x\"\nbanana_outbound_ticks = 65536\n"),
                 "'banana_outbound_ticks'"));
  CHECK(contains(loadErrors(base + "[[mode]]\nname = \"x\"\nbanana_turn_rate = 32769\n"),
                 "'banana_turn_rate'"));
  CHECK(contains(loadErrors(base + "[[mode]]\nname = \"x\"\nbanana_turn_rate = -1\n"),
                 "'banana_turn_rate'"));

  // kMaxModes is 32 since v0.31 (8 before): kMinimal's rounds plus 31 more.
  std::string many = base;
  for (uint32_t i = 1; i < sim::kMaxModes; ++i)
    many += "[[mode]]\nname = \"m" + std::to_string(i) + "\"\n";
  ContentDB full{};
  Diagnostics fullDiag;
  REQUIRE(parseContent(many, "modes.toml", full, fullDiag));
  CHECK(full.modeCount == 32);
  CHECK(full.findMode("m31") == std::optional<uint8_t>{31});
  CHECK(contains(loadErrors(many + "[[mode]]\nname = \"m32\"\n"),
                 "too many modes (at most 32, kMaxModes)"));
}

// --- harvest (v0.81) -------------------------------------------------------

namespace {

// kMinimal plus four tilemap arenas (two that suit harvest, one without a
// tree, one with a single zone) and a harvest mode over a pool of two.
std::string withHarvest() {
  std::string text(kMinimal);
  const auto arena = [&](std::string_view name, std::string_view top, bool tree) {
    const bool b = top.ends_with("bb");
    text += "[[arena]]\nname = \"";
    text += name;
    text += "\"\ncell = 1.0\nspawn_groups = 2\nmap = [\n  \"";
    text += top;
    text += b ? "\",\n  \"aa....bb\"," : "\",\n  \"aa......\",";
    text += "\n  \"........\",\n  \"........\",\n]\n";
    if (tree) {
      text +=
          "spawners = [{ pos = [0.5, 0.5], tree = true, first_ticks = 0, "
          "interval_ticks = 60 }]\n";
    }
  };
  arena("grove", "aa0..1bb", true);
  arena("grove2", "aa0..1bb", true);
  arena("bare", "aa0..1bb", false);
  arena("lone", "aa0..1..", true);
  text += R"(
[[mode]]
name = "harvest"
kind = "harvest"
teams = 2
score_to_win = 40
bank_ticks = 3
steal_ticks = 9
respawn_ammo = 0
arenas = ["grove", "grove2"]
arena = "grove2"
)";
  return text;
}

}  // namespace

TEST_CASE("modes: harvest, its keys and its pool of arenas (v0.81)", "[content][loader][modes]") {
  ContentDB db{};
  Diagnostics diag;
  const bool ok = parseContent(withHarvest(), "modes.toml", db, diag);
  INFO(diag.format());
  REQUIRE(ok);
  const std::optional<uint8_t> index = db.findMode("harvest");
  REQUIRE(index);
  const uint8_t mi = index.value_or(0);
  const sim::ModeDef& m = db.modes[mi];
  const uint8_t grove = db.findArena("grove").value_or(0);
  const uint8_t grove2 = db.findArena("grove2").value_or(0);
  const uint8_t bare = db.findArena("bare").value_or(0);
  const uint8_t lone = db.findArena("lone").value_or(0);
  CHECK(m.kind == sim::ModeKind::Harvest);
  CHECK(m.bankTicks == 3);
  CHECK(m.stealTicks == 9);
  CHECK(m.respawnAmmo == 0);
  CHECK(m.arena == grove2);
  CHECK(m.arenaSet == ((1u << grove) | (1u << grove2)));
  CHECK(m.hasArenaSet());
  CHECK(m.inArenaSet(grove));
  CHECK_FALSE(m.inArenaSet(bare));
  CHECK(sim::respawnAmmoOf(db.player, m) == 0);
  // compatible(): a zone per team and a tree.
  CHECK(db.compatible(grove, mi));
  CHECK_FALSE(db.compatible(bare, mi));
  CHECK_FALSE(db.compatible(lone, mi));
  CHECK_FALSE(db.compatible(0, mi));  // the box: no zones, no cells
  // The defaults: no pool, the respawn's hand ammo_start, banking every 2
  // ticks, no stealing.
  const sim::ModeDef& rounds = db.modes[0];
  CHECK(rounds.respawnAmmo == sim::kRespawnAmmoStart);
  CHECK(sim::respawnAmmoOf(db.player, rounds) == db.player.ammoStart);
  CHECK(rounds.bankTicks == 2);
  CHECK(rounds.stealTicks == 0);
  CHECK(rounds.arenaSet == sim::kNoArenaSet);
  CHECK_FALSE(rounds.hasArenaSet());
  CHECK(db.firstCompatibleArena(mi) == std::optional<uint8_t>{grove2});  // its own arena

  // Without `arena`, a host's pick is the pool's first member that suits it,
  // ahead of an earlier arena outside the pool.
  {
    ContentDB pool{};
    Diagnostics poolDiag;
    REQUIRE(parseContent(replaced(replaced(withHarvest(), "arena = \"grove2\"\n", ""),
                                  "arenas = [\"grove\", \"grove2\"]", "arenas = [\"grove2\"]"),
                         "modes.toml", pool, poolDiag));
    const uint8_t h = pool.findMode("harvest").value_or(0);
    CHECK_FALSE(pool.modes[h].hasArena());
    CHECK(pool.firstCompatibleArena(h) == pool.findArena("grove2"));
  }
  // Key bounds.
  CHECK(loads(replaced(withHarvest(), "respawn_ammo = 0", "respawn_ammo = 254")));
  CHECK(contains(loadErrors(replaced(withHarvest(), "respawn_ammo = 0", "respawn_ammo = 255")),
                 "'respawn_ammo' must be an integer in [0, 254]"));
  CHECK(contains(loadErrors(replaced(withHarvest(), "respawn_ammo = 0", "respawn_ammo = -1")),
                 "'respawn_ammo' must be an integer in [0, 254]"));
  CHECK(loads(replaced(withHarvest(), "bank_ticks = 3", "bank_ticks = 65535")));
  CHECK(contains(loadErrors(replaced(withHarvest(), "bank_ticks = 3", "bank_ticks = 0")),
                 "'bank_ticks' must be an integer in [1, 65535]"));
  CHECK(loads(replaced(withHarvest(), "steal_ticks = 9", "steal_ticks = 0")));
  CHECK(contains(loadErrors(replaced(withHarvest(), "steal_ticks = 9", "steal_ticks = 65536")),
                 "'steal_ticks' must be an integer in [0, 65535]"));
  // The pool.
  const auto pooled = [](std::string_view list) {
    return replaced(withHarvest(), R"(arenas = ["grove", "grove2"])", list);
  };
  CHECK(contains(loadErrors(pooled("arenas = []")), "'arenas' must list 1 to 16 arena names"));
  CHECK(
      contains(loadErrors(pooled("arenas = \"grove\"")), "'arenas' must list 1 to 16 arena names"));
  CHECK(contains(loadErrors(pooled("arenas = [3]")), "'arenas' entries must be arena names"));
  CHECK(contains(loadErrors(pooled("arenas = [\"grove2\", \"nowhere\"]")),
                 "'arenas' names unknown arena \"nowhere\""));
  CHECK(contains(loadErrors(pooled("arenas = [\"grove2\", \"grove2\"]")),
                 "'arenas' names arena \"grove2\" twice"));
  CHECK(contains(loadErrors(pooled("arenas = [\"grove2\", \"bare\"]")),
                 "mode \"harvest\": arenas member \"bare\" does not suit it"));
  CHECK(contains(loadErrors(pooled("arenas = [\"grove\"]")),
                 "mode \"harvest\": its arena must be one of its arenas"));
  // A pool needs no harvest: any mode may name one its members suit.
  CHECK(loads(std::string(kMinimal) + "[[mode]]\nname = \"free\"\narenas = [\"box\"]\n"));
  // The kind: a team kind that respawns, scoring points.
  CHECK(contains(loadErrors(replaced(withHarvest(), "teams = 2\n", "")),
                 "mode \"harvest\": a team kind needs teams of 2 or more"));
  CHECK(contains(loadErrors(replaced(withHarvest(), "score_to_win = 40",
                                     "score_to_win = 40\nrespawn_ticks = 0")),
                 "respawn_ticks must be at least 1 in harvest"));
  CHECK(loads(replaced(withHarvest(), "score_to_win = 40", "score_to_win = 40\ncatch_score = 1")));
  CHECK(contains(loadErrors(std::string(kMinimal) + "[[mode]]\nname = \"x\"\nkind = \"farm\"\n"),
                 "control or harvest)"));
  {
    // validateContent() holds content built in code to the same rules.
    ContentDB v{};
    Diagnostics vDiag;
    REQUIRE(parseContent(withHarvest(), "modes.toml", v, vDiag));
    sim::ModeDef& h = v.modes[v.findMode("harvest").value_or(0)];
    Diagnostics fine;
    CHECK(validateContent(v, fine));
    h.bankTicks = 0;
    Diagnostics zero;
    CHECK_FALSE(validateContent(v, zero));
    CHECK(zero.format().contains("bank_ticks must be at least 1"));
    h.bankTicks = 2;
    h.arenaSet = static_cast<uint16_t>(h.arenaSet | (1u << 15));
    Diagnostics undefined;
    CHECK_FALSE(validateContent(v, undefined));
    CHECK(undefined.format().contains("arenas names an undefined arena"));
  }
}

TEST_CASE("content/ ships rounds, deathmatch and survival", "[content][loader][modes]") {
  ContentDB loaded{};
  Diagnostics diag;
  REQUIRE(loadContentDir(KFM_CONTENT_DIR, loaded, diag));
  const uint8_t deathmatch = loaded.findMode("deathmatch").value_or(0);
  const uint8_t survival = loaded.findMode("survival").value_or(0);
  REQUIRE(loaded.findMode("rounds") == std::optional<uint8_t>{0});  // the default stays first
  REQUIRE(deathmatch != 0);
  REQUIRE(survival != 0);
  CHECK(loaded.modes[0].kind == sim::ModeKind::Rounds);
  CHECK(loaded.modes[deathmatch].kind == sim::ModeKind::Deathmatch);
  CHECK(loaded.modes[deathmatch].timeLimitTicks > 0);
  CHECK(loaded.modes[survival].kind == sim::ModeKind::Survival);
  CHECK(loaded.modes[survival].timeLimitTicks > 0);
}

TEST_CASE("content/ settles the banana economy per mode (v0.57)", "[content][loader][modes]") {
  ContentDB loaded{};
  Diagnostics diag;
  REQUIRE(loadContentDir(KFM_CONTENT_DIR, loaded, diag));
  for (const char* name : {"rounds", "survival", "team_rounds", "grid_rounds", "grid_rounds_1t"}) {
    const sim::ModeDef& m = loaded.modes[loaded.findMode(name).value_or(0)];
    CHECK(m.deathBananas == sim::DeathBananas::Drop);
    CHECK(m.catchScore == 0);
    CHECK(m.suicideScore == 0);
    CHECK(m.bananaRegrowTicks == 0);
  }
  for (const char* name : {"deathmatch", "team_deathmatch"}) {
    const sim::ModeDef& m = loaded.modes[loaded.findMode(name).value_or(0)];
    CHECK(m.deathBananas == sim::DeathBananas::Vanish);
    CHECK(m.catchScore == 1);
    CHECK(m.suicideScore == -1);
    CHECK(m.bananaRegrowTicks == 0);
    CHECK(m.respawnTicks == 120);
  }
  for (const char* name : {"hill", "capture"}) {
    const sim::ModeDef& m = loaded.modes[loaded.findMode(name).value_or(0)];
    CHECK(m.deathBananas == sim::DeathBananas::Vanish);
    CHECK(m.catchScore == 0);
    CHECK(m.suicideScore == 0);
    CHECK(m.bananaRegrowTicks == 480);
    CHECK(m.respawnTicks == 300);
  }
}

// M7 (architecture §8 "The ladder"): the contest row is the ladder's ruleset.
// Since v0.80 it is the control kind (§11 "Contest design") with
// grid_rounds_1t's squad: the space, turns, kits and the vision's radii are
// that row's; the baseline is contest_base (the banana at 2 cells a turn),
// the arena courtyard_zones (three zones), hearing 1 and the banana's flight
// the mode's own. A season's change to the row is a deliberate edit here;
// the arena, the twins and the flight are pinned in contest_test.cpp.
TEST_CASE("content/ ships contest as control with grid_rounds_1t's squad (M7, v0.80)",
          "[content][loader][modes]") {
  ContentDB db{};
  Diagnostics diag;
  REQUIRE(loadContentDir(KFM_CONTENT_DIR, db, diag));
  const std::optional<uint8_t> contest = db.findMode("contest");
  const std::optional<uint8_t> grid = db.findMode("grid_rounds_1t");
  const std::optional<uint8_t> courtyard = db.findArena("courtyard");
  const std::optional<uint8_t> zones = db.findArena("courtyard_zones");
  const std::optional<uint8_t> base = db.findPowerUp("contest_base");
  REQUIRE(contest);
  REQUIRE(grid);
  REQUIRE(courtyard);
  REQUIRE(zones);
  REQUIRE(base);
  const sim::ModeDef& g = db.modes[grid.value_or(0)];
  const sim::ModeDef& m = db.modes[contest.value_or(0)];
  CHECK(m.kind == sim::ModeKind::Control);
  CHECK(m.space == g.space);
  CHECK(m.turnTicks == g.turnTicks);
  CHECK(m.turnTicks == 1);
  CHECK(m.dashCells == g.dashCells);
  CHECK(m.teams == g.teams);
  CHECK(m.roleCount == g.roleCount);
  CHECK(m.roles == g.roles);
  CHECK(m.baseline == base);
  CHECK(m.vision.kind == sim::VisionKind::Own);
  CHECK(m.vision.peripheral == g.vision.peripheral);
  CHECK(m.vision.cone == g.vision.cone);
  CHECK(m.vision.focus == g.vision.focus);
  CHECK(m.vision.bananaSight == g.vision.bananaSight);
  CHECK(m.vision.hearing == 1);
  CHECK(m.captureTicks == 30);
  CHECK(m.hillTicks == 60);
  CHECK(m.scoreToWin == 10);
  CHECK(m.timeLimitTicks == 3600);
  CHECK(m.respawnTicks == 240);
  CHECK(m.respawnInvulnTicks == 8);
  CHECK(m.deathBananas == sim::DeathBananas::Vanish);
  CHECK(m.bananaRegrowTicks == 120);
  CHECK(m.catchScore == 0);
  CHECK(m.suicideScore == 0);
  CHECK(m.bananaOutboundTicks == 3);
  CHECK(m.bananaTurnRateBam == 15474);
  CHECK(m.roundStartTicks == 60);
  CHECK(m.arena == zones);
  CHECK(db.firstCompatibleArena(contest.value_or(0)) == zones);
  CHECK(db.compatible(zones.value_or(0), contest.value_or(0)));
  CHECK_FALSE(db.compatible(courtyard.value_or(0), contest.value_or(0)));  // no zone
  CHECK_FALSE(db.compatible(0, contest.value_or(0)));                      // the dojo has no cells
}

// v0.69's hill, kept for the hill kind: the courtyard with its middle block
// taken out and the floor between the pillars made zone 0.
TEST_CASE("content/ ships courtyard_hill as the courtyard with a hill (v0.69)",
          "[content][loader][arena]") {
  ContentDB db{};
  Diagnostics diag;
  REQUIRE(loadContentDir(KFM_CONTENT_DIR, db, diag));
  const std::optional<uint8_t> courtyard = db.findArena("courtyard");
  const std::optional<uint8_t> hillArena = db.findArena("courtyard_hill");
  const std::optional<uint8_t> hill = db.findMode("hill");
  REQUIRE(courtyard);
  REQUIRE(hillArena);
  REQUIRE(hill);
  CHECK(db.compatible(hillArena.value_or(0), hill.value_or(0)));
  // The arena is the courtyard but for its middle: same box, spawns and
  // spawners, one zone (4 x 2 cells between the pillars), one wall box fewer.
  const sim::ArenaDef& a = db.arenas[hillArena.value_or(0)];
  const sim::ArenaDef& c = db.arenas[courtyard.value_or(0)];
  CHECK(a.cellSize == c.cellSize);
  CHECK(a.halfExtents == c.halfExtents);
  CHECK(a.spawnGroups == c.spawnGroups);
  CHECK(a.spawnPointCount == c.spawnPointCount);
  for (uint8_t i = 0; i < c.spawnPointCount; ++i)
    CHECK(a.spawnPoints[i] == c.spawnPoints[i]);
  CHECK(a.hazardCount == c.hazardCount);
  for (uint8_t i = 0; i < c.hazardCount; ++i)
    CHECK((a.hazards[i].min == c.hazards[i].min && a.hazards[i].max == c.hazards[i].max &&
           a.hazards[i].kind == c.hazards[i].kind));
  CHECK(a.pickupSpawnerCount == c.pickupSpawnerCount);
  for (uint8_t i = 0; i < c.pickupSpawnerCount; ++i)
    CHECK(bytesOf(a.pickupSpawners[i]) == bytesOf(c.pickupSpawners[i]));
  CHECK(a.zoneCount == 1);
  CHECK(a.zones[0].min == Vec2{Fixed::fromInt(-2), Fixed::fromInt(-1)});
  CHECK(a.zones[0].max == Vec2{Fixed::fromInt(2), Fixed::fromInt(1)});
  CHECK(a.wallCount + 1 == c.wallCount);
}

TEST_CASE("content/ ships blink, slam and parry as loadouts (v0.21, v0.31)",
          "[content][loader][abilities][loadout]") {
  ContentDB loaded{};
  Diagnostics diag;
  REQUIRE(loadContentDir(KFM_CONTENT_DIR, loaded, diag));
  // The base kit keeps the ability button empty.
  CHECK(loaded.player.abilities[sim::kAbilitySlotExtra] == 0);
  CHECK(loaded.loadouts[0].name.view() == sim::kBaseLoadoutName);
  const std::array<std::pair<std::string_view, sim::AbilityKind>, 3> kinds = {{
      {"blink", sim::AbilityKind::Blink},
      {"slam", sim::AbilityKind::Slam},
      {"parry", sim::AbilityKind::Parry},
  }};
  for (const auto& [name, kind] : kinds) {
    INFO(name);
    const uint8_t index = loaded.findLoadout(name).value_or(0);
    REQUIRE(index != 0);
    const sim::LoadoutDef& l = loaded.loadouts[index];
    CHECK(l.abilities[sim::kAbilitySlotDash] == loaded.player.abilities[sim::kAbilitySlotDash]);
    CHECK(l.abilities[sim::kAbilitySlotThrow] == loaded.player.abilities[sim::kAbilitySlotThrow]);
    const sim::AbilityDef& def = loaded.abilityAt(l.abilities[sim::kAbilitySlotExtra]);
    CHECK(def.name.view() == name);
    CHECK(def.kind == kind);
    // The v0.21 ability modes are gone: the kit is its own axis.
    CHECK_FALSE(loaded.findMode("rounds_" + std::string(name)).has_value());
  }
}

TEST_CASE("ability kinds: names and per-kind validation (v0.21)", "[content][loader][abilities]") {
  const std::string base(kMinimal);
  const auto withAbility = [&](std::string_view body) {
    return base + "\n[[ability]]\nname = \"extra\"\n" + std::string(body);
  };
  CHECK(loads(withAbility("kind = \"blink\"\nactive_ticks = 10\nspeed = 30\n")));
  CHECK(loads(withAbility("kind = \"slam\"\nactive_ticks = 12\nparam0 = 3\nparam1 = 4\n")));
  CHECK(loads(withAbility("kind = \"parry\"\nactive_ticks = 8\nrecovery_ticks = 4\n")));
  CHECK(
      loads(withAbility("kind = \"parry\"\nactive_ticks = 8\nrecovery_ticks = 4\n"
                        "tags_active = [\"parrying\", \"slamming\"]\n")));
  CHECK(contains(loadErrors(withAbility("kind = \"teleport\"\n")), "unknown kind \"teleport\""));
  CHECK(contains(loadErrors(withAbility("kind = \"blink\"\nactive_ticks = 10\n")),
                 "a blink needs active_ticks >= 1 and a positive speed"));
  CHECK(contains(loadErrors(withAbility("kind = \"slam\"\nactive_ticks = 12\nparam0 = 0.25\n")),
                 "a slam needs active_ticks >= 1, param0 (reach) in (player radius, 8]"));
  CHECK(contains(loadErrors(withAbility("kind = \"slam\"\nactive_ticks = 12\nparam0 = 9\n")),
                 "a slam needs"));
  CHECK(contains(loadErrors(withAbility("kind = \"parry\"\nactive_ticks = 8\n")),
                 "a parry needs active_ticks >= 1 and recovery_ticks >= 1"));
  // v0.35: a call's range is its param0, in units.
  {
    ContentDB db{};
    Diagnostics d;
    const bool ok = parseContent(withAbility("kind = \"call\"\ncooldown_ticks = 15\nrange = 4.5\n"),
                                 "test.toml", db, d);
    INFO(d.format());
    REQUIRE(ok);
    const sim::AbilityDef& call = db.abilities[db.abilityCount - 1];
    CHECK(call.name.view() == "extra");
    CHECK(call.kind == sim::AbilityKind::Call);
    CHECK(call.param0 == Fixed::fromRatio(9, 2));
  }
  CHECK(loads(withAbility("kind = \"call\"\nparam0 = 4\n")));
  CHECK(contains(loadErrors(withAbility("kind = \"call\"\n")), "a call needs a range in (0, 128]"));
  CHECK(
      contains(loadErrors(withAbility("kind = \"call\"\nrange = 129\n")), "a call needs a range"));
  CHECK(contains(loadErrors(withAbility("kind = \"call\"\nrange = 4\nparam0 = 4\n")), "not both"));
  CHECK(contains(loadErrors(withAbility("kind = \"slam\"\nactive_ticks = 12\nrange = 3\n")),
                 "'range' belongs to a call"));
  // v0.48: a throw's charge under its own names: param0, param1 and the rate
  // per tick in speed.
  {
    ContentDB db{};
    Diagnostics d;
    const bool ok = parseContent(withAbility("kind = \"throw\"\ncharge_min = 0.5\n"
                                             "charge_max = 1.5\ncharge_ticks = 32\n"),
                                 "test.toml", db, d);
    INFO(d.format());
    REQUIRE(ok);
    const sim::AbilityDef& thr = db.abilities[db.abilityCount - 1];
    CHECK(thr.kind == sim::AbilityKind::Throw);
    CHECK(thr.param0 == Fixed::half());
    CHECK(thr.param1 == Fixed::fromRatio(3, 2));
    CHECK(thr.speed == Fixed::fromRatio(1, 32));
  }
  CHECK(contains(loadErrors(withAbility("kind = \"throw\"\ncharge_min = 0.5\ncharge_max = 1.5\n")),
                 "missing required key 'charge_ticks'"));
  CHECK(contains(loadErrors(withAbility("kind = \"throw\"\ncharge_min = 1.5\ncharge_max = 1.5\n"
                                        "charge_ticks = 10\n")),
                 "charge_max must be greater than charge_min"));
  CHECK(contains(loadErrors(withAbility("kind = \"dash\"\ncharge_min = 0.5\ncharge_max = 1\n"
                                        "charge_ticks = 10\n")),
                 "belongs to a throw"));
  CHECK(contains(loadErrors(withAbility("kind = \"throw\"\ncharge_min = 0.5\ncharge_max = 1\n"
                                        "charge_ticks = 10\nspeed = 3\n")),
                 "not speed, param0 or param1"));
  CHECK(contains(loadErrors(withAbility("kind = \"throw\"\nspeed = 3\n")),
                 "a charged throw needs 0 < charge_min < charge_max"));
  // A third slot through the loadout.
  CHECK(loads(replaced(withAbility("kind = \"blink\"\nactive_ticks = 10\nspeed = 30\n"),
                       "[\"dash\", \"throw\"]", "[\"dash\", \"throw\", \"extra\"]")));
  // Power-ups never grant ability-owned tags.
  CHECK(contains(
      loadErrors(std::string(kMinimal) + "\n[[powerup]]\nname = \"p\"\nduration_ticks = 60\n"
                                         "grants = [{ kind = \"tag\", tag = \"parrying\" }]\n"),
      "a power-up can grant only 'shielded', 'invulnerable' or 'friendly_fire'"));
}

TEST_CASE("modes: the vision block (v0.35)", "[content][loader][modes][vision]") {
  const std::string base = std::string(kMinimal) + R"(
[[arena]]
name = "cells"
cell = 1.0
spawn_groups = 2
map = ["0....", ".....", "....1"]

[[mode]]
name = "fog"
kind = "team_rounds"
teams = 2
)";
  const auto with = [&](std::string_view vision) {
    return base + "vision = " + std::string(vision) + "\n";
  };
  {
    ContentDB db{};
    Diagnostics d;
    const bool ok = parseContent(with("{ kind = \"own\", peripheral = 1, cone = 4, focus = 1, "
                                      "hearing = 3, banana_sight = 1 }"),
                                 "test.toml", db, d);
    INFO(d.format());
    REQUIRE(ok);
    const std::optional<uint8_t> mi = db.findMode("fog");
    REQUIRE(mi);
    const sim::VisionDef& v = db.modes[mi.value_or(0)].vision;
    CHECK(v.kind == sim::VisionKind::Own);
    CHECK(v.fog());
    CHECK(v.peripheral == 1);
    CHECK(v.cone == 4);
    CHECK(v.focus == 1);
    CHECK(v.hearing == 3);
    CHECK(v.bananaSight == 1);
    // A fog vision needs cells: the box arena has none, "cells" has.
    const std::optional<uint8_t> cells = db.findArena("cells");
    const std::optional<uint8_t> box = db.findArena("box");
    REQUIRE(cells);
    REQUIRE(box);
    CHECK(db.compatible(cells.value_or(0), mi.value_or(0)));
    CHECK_FALSE(db.compatible(box.value_or(0), mi.value_or(0)));
    // The default is everything, and rounds keeps it.
    CHECK(db.modes[0].vision == sim::VisionDef{});
    CHECK_FALSE(db.modes[0].vision.fog());
    // The hash sees the block.
    ContentDB other = db;
    other.modes[mi.value_or(0)].vision.cone = 5;
    CHECK(sim::contentHash(other) != sim::contentHash(db));
  }
  CHECK(loads(with("{ kind = \"team\", cone = 3 }")));
  CHECK(loads(with("{ kind = \"all\" }")));
  CHECK(contains(loadErrors(with("{ kind = \"all\", cone = 3 }")), "the radii belong to"));
  CHECK(contains(loadErrors(with("{ kind = \"fog\" }")), "unknown vision kind \"fog\""));
  CHECK(contains(loadErrors(with("{ cone = 3 }")), "missing required key 'kind'"));
  CHECK(contains(loadErrors(with("{ kind = \"own\", cone = 33 }")), "'cone' must be an integer"));
  CHECK(contains(loadErrors(with("{ kind = \"own\", hearing = 33 }")), "'hearing'"));
  CHECK(contains(loadErrors(with("{ kind = \"own\", peripheral = 17 }")), "'peripheral'"));
  CHECK(contains(loadErrors(with("{ kind = \"own\", sight = 1 }")), "unknown key 'sight'"));
  CHECK(contains(loadErrors(with("\"own\"")), "'vision' must be a table"));
  CHECK(contains(loadErrors(replaced(with("{ kind = \"team\" }"), "teams = 2\n", "")),
                 "vision kind \"team\" needs a team mode"));
}

TEST_CASE("team modes: teams, baseline, roles and spawn groups (v0.32)",
          "[content][loader][modes][teams]") {
  const std::string base = std::string(kMinimal) + R"(
[[arena]]
name = "versus"
half_extents = [10, 5]
spawn_groups = 2
spawn_points = [[-6, 0], [-6, 2], [6, 0], [6, 2]]

[[loadout]]
name = "twice"
abilities = ["dash", "throw", "throw"]

[[powerup]]
name = "ff"
duration_ticks = 1
grants = [{ kind = "tag", tag = "friendly_fire" }]
)";
  const std::string teamMode = R"(
[[mode]]
name = "tr"
kind = "team_rounds"
teams = 2
baseline = "ff"
roles = ["base", "twice"]
)";
  ContentDB db{};
  Diagnostics diag;
  const bool ok = parseContent(base + teamMode, "teams.toml", db, diag);
  INFO(diag.format());
  REQUIRE(ok);
  REQUIRE(db.modeCount == 2);
  const sim::ModeDef& tr = db.modes[1];
  CHECK(tr.kind == sim::ModeKind::TeamRounds);
  CHECK(tr.teams == 2);
  CHECK(tr.baseline == db.findPowerUp("ff"));
  CHECK(tr.roleCount == 2);
  CHECK(tr.roles[0] == 0);
  CHECK(tr.roles[1] == db.findLoadout("twice"));
  CHECK(db.modes[0].teams == 0);
  CHECK_FALSE(db.modes[0].hasBaseline());
  CHECK(db.arenas[0].spawnGroups == 1);
  CHECK(db.arenas[1].spawnGroups == 2);
  CHECK(db.powerUps[0].grants[0].tag == sim::Tag::FriendlyFire);
  CHECK_FALSE(db.compatible(0, 1));
  CHECK(db.compatible(1, 1));
  CHECK(db.compatible(0, 0));

  // --mode tr without --arena takes the first arena that suits it; naming
  // one that does not is an error. The teams follow the mode.
  sim::MatchConfig config{.players = 0b1111};
  std::string error;
  REQUIRE(resolveMatchNames(db, "", "tr", config, error));
  CHECK(config.arena == 1);
  CHECK(config.teams == sim::balancedTeams(2, 0b1111));
  CHECK_FALSE(resolveMatchNames(db, "box", "tr", config, error));
  CHECK(contains(error, "arena 'box' does not suit mode 'tr'"));
  CHECK(contains(error, "arenas that do: versus"));
  REQUIRE(resolveMatchNames(db, "versus", "rounds", config, error));
  CHECK(config.teams == sim::kFreeForAllTeams);

  const auto withMode = [&](std::string_view m) { return base + std::string(m); };
  CHECK(contains(loadErrors(withMode("[[mode]]\nname = \"t\"\nteams = 1\n")),
                 "teams must be 0 (free-for-all) or 2 to 10"));
  CHECK(contains(loadErrors(withMode("[[mode]]\nname = \"t\"\nteams = 11\n")), "'teams'"));
  CHECK(contains(loadErrors(withMode("[[mode]]\nname = \"t\"\nkind = \"team_deathmatch\"\n")),
                 "a team kind needs teams of 2 or more"));
  CHECK(contains(loadErrors(withMode("[[mode]]\nname = \"t\"\nroles = [\"base\"]\n")),
                 "'roles' needs a team mode"));
  CHECK(contains(loadErrors(withMode("[[mode]]\nname = \"t\"\nbaseline = \"zap\"\n")),
                 "'baseline' names unknown power-up \"zap\""));
  CHECK(contains(loadErrors(withMode("[[mode]]\nname = \"t\"\nteams = 2\nroles = [\"zap\"]\n")),
                 "'roles' names unknown loadout \"zap\""));
  CHECK(contains(loadErrors(replaced(base, "spawn_groups = 2", "spawn_groups = 5")),
                 "spawn_groups must be in [1, 4] (every group needs a spawn point)"));
  CHECK(contains(loadErrors(replaced(base, "spawn_groups = 2", "spawn_groups = 0")),
                 "'spawn_groups'"));
}

TEST_CASE("content/ ships team_rounds and team_deathmatch on standoff (v0.32)",
          "[content][loader][modes][teams]") {
  ContentDB loaded{};
  Diagnostics diag;
  REQUIRE(loadContentDir(KFM_CONTENT_DIR, loaded, diag));
  const uint8_t tr = loaded.findMode("team_rounds").value_or(0);
  const uint8_t td = loaded.findMode("team_deathmatch").value_or(0);
  const uint8_t standoff = loaded.findArena("standoff").value_or(0);
  REQUIRE(tr != 0);
  REQUIRE(td != 0);
  REQUIRE(standoff != 0);
  CHECK(loaded.modeAt(tr).kind == sim::ModeKind::TeamRounds);
  CHECK(loaded.modeAt(td).kind == sim::ModeKind::TeamDeathmatch);
  CHECK(loaded.modeAt(tr).teams == 2);
  CHECK(loaded.modeAt(td).teams == 2);
  CHECK_FALSE(loaded.modeAt(tr).hasBaseline());  // no friendly fire by default
  CHECK(loaded.arenaAt(standoff).spawnGroups == 2);
  CHECK(loaded.firstCompatibleArena(tr) == std::optional<uint8_t>{standoff});
  // The free-for-all arenas keep one group: their checksums never moved.
  // Arenas made for team modes (standoff, v0.33's objective arenas) have two.
  for (const std::string_view name : {"dojo", "pillars", "corridors", "island", "dojo_powerups"}) {
    INFO(name);
    CHECK(loaded.arenaAt(loaded.findArena(name).value_or(0)).spawnGroups == 1);
  }
}
