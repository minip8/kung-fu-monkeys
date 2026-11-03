#include "kfm/content/loader.hpp"

#include "kfm/content/app_content.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/visit.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
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
// the singletons, every ability the fixture defines, arena 0 and mode 0
// must agree; further abilities, arenas and modes are TOML-only.
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
  CHECK(contains(loadErrors(replaced(kMinimal, "[\"dash\", \"throw\"]", "[\"throw\", \"dash\"]")),
                 "slot 0 must be a dash and slot 1 a throw"));
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
      loadErrors(replaced(kMinimal, "perfect_window_ticks = 4", "perfect_window_ticks = 12")),
      "active_ticks must exceed perfect_window_ticks"));
  CHECK(contains(
      loadErrors(replaced(kMinimal, "active_ticks = 12", "active_ticks = 12\nstartup_ticks = 1")),
      "startup_ticks = 0"));
  CHECK(contains(loadErrors(replaced(kMinimal, "kind = \"dash\"",
                                     "kind = \"dash\"\ntags_active = [\"flying\"]")),
                 "unknown tag in 'tags_active'"));
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
  CHECK(contains(loadErrors(manyArenas), "too many arenas (at most 8, kMaxArenas)"));
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
