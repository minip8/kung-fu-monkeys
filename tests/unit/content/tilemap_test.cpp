// Zones and tilemap arenas (architecture §5.10, v0.33): glyphs, run merging,
// caps, zone rectangles, spawn order, and ContentDB::compatible().
#include "kfm/content/loader.hpp"
#include "kfm/sim/content.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>

using namespace kfm;
using namespace kfm::content;
using sim::ContentDB;

namespace {

// Everything but the arenas and modes.
constexpr std::string_view kBase = R"(
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

[[mode]]
name = "rounds"
)";

std::string withArena(std::string_view arena) {
  return std::string(kBase) + "\n[[arena]]\n" + std::string(arena) + "\n";
}

// Loads one arena; nullopt (with the diagnostics in `errors`) on failure.
bool loadArena(std::string_view arena, ContentDB& db, std::string& errors) {
  Diagnostics diag;
  const bool ok = parseContent(withArena(arena), "map.toml", db, diag);
  errors = diag.format();
  return ok;
}

std::string arenaErrors(std::string_view arena) {
  ContentDB db{};
  std::string errors;
  CHECK_FALSE(loadArena(arena, db, errors));
  return errors;
}

Vec2 v(double x, double y) {
  return Vec2{Fixed::fromRaw(static_cast<int32_t>(x * 65536.0)),
              Fixed::fromRaw(static_cast<int32_t>(y * 65536.0))};
}

}  // namespace

TEST_CASE("tilemap: glyphs, extents, statics and spawn order", "[content][tilemap]") {
  // 8 x 4 cells of 1: x in [-4, 4], y in [-2, 2], row 0 at the top.
  ContentDB db{};
  std::string errors;
  const bool ok = loadArena(R"(name = "tiles"
cell = 1.0
map = [
  "1..##..0",
  "...##.:.",
  "~~....:.",
  "~~..aa..",
])",
                            db, errors);
  INFO(errors);
  REQUIRE(ok);
  const sim::ArenaDef& a = db.arenas[0];
  CHECK(a.cellSize == Fixed::one());
  CHECK(a.halfExtents == v(4, 2));
  // The two '#' rows have the same columns: one box.
  REQUIRE(a.wallCount == 1);
  CHECK(a.walls[0].min == v(-1, 0));
  CHECK(a.walls[0].max == v(1, 2));
  // Pits first, then mud; each merged vertically.
  REQUIRE(a.hazardCount == 2);
  CHECK(a.hazards[0].kind == sim::HazardKind::Pit);
  CHECK(a.hazards[0].min == v(-4, -2));
  CHECK(a.hazards[0].max == v(-2, 0));
  CHECK(a.hazards[1].kind == sim::HazardKind::Slow);
  CHECK(a.hazards[1].speedMul == Fixed::half());
  CHECK(a.hazards[1].min == v(2, -1));
  CHECK(a.hazards[1].max == v(3, 1));
  // Spawn points in digit order at their cell centres.
  REQUIRE(a.spawnPointCount == 2);
  CHECK(a.spawnPoints[0] == v(3.5, 1.5));
  CHECK(a.spawnPoints[1] == v(-3.5, 1.5));
  REQUIRE(a.zoneCount == 1);
  CHECK(a.zones[0].min == v(0, -2));
  CHECK(a.zones[0].max == v(2, -1));
  CHECK(a.zones[0].centre() == v(1, -1.5));
}

TEST_CASE("tilemap: run merging keeps unequal rows apart", "[content][tilemap]") {
  ContentDB db{};
  std::string errors;
  // Row 0 run [0, 4), row 1 run [0, 2) and [3, 4), row 2 run [0, 2): the
  // [0, 2) runs of rows 1 and 2 merge, nothing merges into row 0.
  const bool ok = loadArena(R"(name = "runs"
cell = 1
mud_speed_mul = 0.25
map = [
  "####0",
  "##.#.",
  "##...",
  "::::.",
])",
                            db, errors);
  INFO(errors);
  REQUIRE(ok);
  const sim::ArenaDef& a = db.arenas[0];
  CHECK(a.halfExtents == v(2.5, 2));
  REQUIRE(a.wallCount == 3);
  CHECK(a.walls[0].min == v(-2.5, 1));  // row 0, columns 0-3
  CHECK(a.walls[0].max == v(1.5, 2));
  CHECK(a.walls[1].min == v(-2.5, -1));  // rows 1-2, columns 0-1
  CHECK(a.walls[1].max == v(-0.5, 1));
  CHECK(a.walls[2].min == v(0.5, 0));  // row 1, column 3
  CHECK(a.walls[2].max == v(1.5, 1));
  REQUIRE(a.hazardCount == 1);
  CHECK(a.hazards[0].speedMul == Fixed::fromRaw(Fixed::kOneRaw / 4));
  CHECK(a.hazards[0].min == v(-2.5, -2));
  CHECK(a.hazards[0].max == v(1.5, -1));
  CHECK(a.spawnPoints[0] == v(2, 1.5));
}

TEST_CASE("tilemap: caps, zones and spawn digits are validated", "[content][tilemap]") {
  // 33 separate wall cells: more boxes than kMaxArenaWalls after merging.
  std::string map = "name = \"many\"\ncell = 1\nmap = [\n  \"0";
  for (int i = 0; i < 33; ++i)
    map += ".#";
  map += "\",\n]";
  CHECK(arenaErrors(map).contains("the map merges into 33 wall boxes, more than kMaxArenaWalls"));
  std::string pits = "name = \"pits\"\ncell = 1\nmap = [\n  \"0";
  for (int i = 0; i < 17; ++i)
    pits += ".~";
  pits += "\",\n]";
  CHECK(arenaErrors(pits).contains("17 pit and mud boxes, more than kMaxArenaHazards"));

  CHECK(arenaErrors("name = \"z\"\ncell = 1\nmap = [\"0aa\", \".a.\"]")
            .contains("zone glyph 'a' must fill one rectangle of cells"));
  CHECK(arenaErrors("name = \"z\"\ncell = 1\nmap = [\"0b.\"]")
            .contains("zone glyph 'b' needs every zone letter before it"));
  CHECK(arenaErrors("name = \"z\"\ncell = 1\nmap = [\"02.\"]")
            .contains("spawn glyph '2' needs every digit below it"));
  CHECK(arenaErrors("name = \"z\"\ncell = 1\nmap = [\"00.\"]")
            .contains("spawn glyph '0' appears more than once"));
  CHECK(arenaErrors("name = \"z\"\ncell = 1\nmap = [\"0x.\"]").contains("unknown glyph 'x'"));
  CHECK(arenaErrors("name = \"z\"\ncell = 1\nmap = [\"0..\", \"..\"]")
            .contains("rows must be of equal length"));
  CHECK(arenaErrors("name = \"z\"\nmap = [\"0..\"]").contains("missing required key 'cell'"));
  CHECK(arenaErrors("name = \"z\"\ncell = 1\nhalf_extents = [4, 4]\nmap = [\"0..\"]")
            .contains("'half_extents' cannot be given with 'map'"));
  CHECK(arenaErrors("name = \"z\"\ncell = 1\nhalf_extents = [4, 4]\nspawn_points = [[0, 0]]")
            .contains("'cell' belongs to a tilemap arena"));
  // No spawn glyph: the arena validation's spawn point count applies.
  CHECK(arenaErrors("name = \"z\"\ncell = 1\nmap = [\"...\"]").contains("spawn points"));
  // A spawn cell next to a wall is fine (one radius from the face), but the
  // same validation as a hand-placed arena runs: a cell of 0.5 puts it inside.
  {
    ContentDB db{};
    std::string errors;
    CHECK(loadArena("name = \"z\"\ncell = 1\nmap = [\"#0#\"]", db, errors));
  }
  CHECK(arenaErrors("name = \"z\"\ncell = 0.5\nmap = [\"#0#\"]")
            .contains("puts a monkey inside wall"));
}

TEST_CASE("tilemap: spawn groups split the digit order", "[content][tilemap]") {
  ContentDB db{};
  std::string errors;
  const bool ok = loadArena(R"(name = "sides"
cell = 1
spawn_groups = 2
map = [
  "0....3",
  "1.aa.4",
  "2....5",
])",
                            db, errors);
  INFO(errors);
  REQUIRE(ok);
  const sim::ArenaDef& a = db.arenas[0];
  CHECK(a.spawnGroups == 2);
  REQUIRE(a.spawnPointCount == 6);
  const sim::SpawnRange west = sim::spawnGroupRange(a, 0);
  const sim::SpawnRange east = sim::spawnGroupRange(a, 1);
  CHECK(west.begin == 0);
  CHECK(west.end == 3);
  CHECK(east.begin == 3);
  CHECK(east.end == 6);
  for (uint8_t k = west.begin; k < west.end; ++k)
    CHECK(a.spawnPoints[k].x < Fixed::zero());
  for (uint8_t k = east.begin; k < east.end; ++k)
    CHECK(a.spawnPoints[k].x > Fixed::zero());
}

TEST_CASE("zones: hand-placed boxes load and are validated", "[content][zones]") {
  const std::string_view arena = R"(name = "ring"
half_extents = [10, 5]
spawn_points = [[-2, 0], [2, 0]]
zones = [{ min = [-1, -1], max = [1, 1] }, { min = [5, 2], max = [6, 5] }])";
  ContentDB db{};
  std::string errors;
  const bool ok = loadArena(arena, db, errors);
  INFO(errors);
  REQUIRE(ok);
  const sim::ArenaDef& a = db.arenas[0];
  REQUIRE(a.zoneCount == 2);
  CHECK(a.zones[1].min == v(5, 2));
  CHECK(a.cellSize == Fixed::zero());
  const auto broken = [&](std::string_view from, std::string_view to) {
    std::string s(arena);
    s.replace(s.find(from), from.size(), to);
    return arenaErrors(s);
  };
  CHECK(broken("max = [6, 5]", "max = [11, 5]").contains("zone 1 reaches outside the arena box"));
  CHECK(broken("min = [5, 2]", "min = [7, 2]").contains("zone 1 needs min < max"));
  CHECK(broken("max = [1, 1] }", "max = [1, 1], kind = \"hill\" }")
            .contains("zone 0: unknown key 'kind'"));
  std::string nine = "name = \"nine\"\nhalf_extents = [10, 5]\nspawn_points = [[0, 0]]\nzones = [";
  for (int i = 0; i < 9; ++i)
    nine += "{ min = [0, 0], max = [1, 1] },";
  nine += ']';
  CHECK(arenaErrors(nine).contains("'zones' must list at most 8"));
}

TEST_CASE("content/ ships hill on the hilltop tilemap (v0.33)", "[content][tilemap][modes]") {
  ContentDB loaded{};
  Diagnostics diag;
  const bool ok = loadContentDir(KFM_CONTENT_DIR, loaded, diag);
  INFO(diag.format());
  REQUIRE(ok);
  const uint8_t hill = loaded.findMode("hill").value_or(0);
  const uint8_t hilltop = loaded.findArena("hilltop").value_or(0);
  REQUIRE(hill != 0);
  REQUIRE(hilltop != 0);
  const sim::ModeDef& m = loaded.modeAt(hill);
  CHECK(m.kind == sim::ModeKind::Hill);
  CHECK(m.teams == 2);
  CHECK(m.hillTicks == 300);
  CHECK(m.scoreToWin == 5);
  const sim::ArenaDef& a = loaded.arenaAt(hilltop);
  CHECK(a.cellSize == Fixed::one());
  CHECK(a.halfExtents == v(16, 9));
  CHECK(a.spawnGroups == 2);
  CHECK(a.spawnPointCount == 10);
  REQUIRE(a.zoneCount == 1);
  CHECK(a.zones[0].min == v(-2, -2));
  CHECK(a.zones[0].max == v(2, 2));
  CHECK(loaded.firstCompatibleArena(hill) == std::optional<uint8_t>{hilltop});
  CHECK(hilltop == 6);  // appended after the v0.32 arenas
}

TEST_CASE("hill: mode keys and validation (v0.33)", "[content][modes][hill]") {
  const std::string arena = "name = \"z\"\ncell = 1\nspawn_groups = 2\nmap = [\"0aa1\"]";
  const auto mode = [&](std::string_view m) {
    return withArena(arena) + "\n[[mode]]\nname = \"h\"\nkind = \"hill\"\n" + std::string(m);
  };
  {
    ContentDB db{};
    Diagnostics diag;
    const bool ok = parseContent(mode("teams = 2\nhill_ticks = 90\n"), "m.toml", db, diag);
    INFO(diag.format());
    REQUIRE(ok);
    CHECK(db.modes[1].kind == sim::ModeKind::Hill);
    CHECK(db.modes[1].hillTicks == 90);
    CHECK(db.compatible(0, 1));
  }
  const auto errors = [](const std::string& text) {
    ContentDB db{};
    Diagnostics diag;
    CHECK_FALSE(parseContent(text, "m.toml", db, diag));
    return diag.format();
  };
  CHECK(errors(mode("")).contains("a team kind needs teams of 2 or more"));
  CHECK(errors(mode("teams = 2\nhill_ticks = 0\n")).contains("'hill_ticks'"));
  CHECK(errors(mode("teams = 2\nscore_to_win = 0\n"))
            .contains("score_to_win must be at least 1 in hill"));
}

TEST_CASE("content/ ships capture on the outposts tilemap (v0.33)", "[content][tilemap][modes]") {
  ContentDB loaded{};
  Diagnostics diag;
  const bool ok = loadContentDir(KFM_CONTENT_DIR, loaded, diag);
  INFO(diag.format());
  REQUIRE(ok);
  const uint8_t capture = loaded.findMode("capture").value_or(0);
  const uint8_t outposts = loaded.findArena("outposts").value_or(0);
  REQUIRE(capture != 0);
  REQUIRE(outposts == 7);  // appended after hilltop
  const sim::ModeDef& m = loaded.modeAt(capture);
  CHECK(m.kind == sim::ModeKind::Capture);
  CHECK(m.teams == 2);
  CHECK(m.scoreToWin == 3);
  CHECK(m.flagReturnTicks == 600);
  const sim::ArenaDef& a = loaded.arenaAt(outposts);
  REQUIRE(a.zoneCount == 2);
  CHECK(a.zones[0].max.x < Fixed::zero());  // team 0's base in the west
  CHECK(a.zones[1].min.x > Fixed::zero());
  CHECK(a.spawnGroups == 2);
  CHECK(loaded.firstCompatibleArena(capture) == std::optional<uint8_t>{outposts});
  CHECK_FALSE(loaded.compatible(loaded.findArena("hilltop").value_or(0), capture));
  // Every spawn group sits next to its team's base.
  for (uint8_t k = sim::spawnGroupRange(a, 0).begin; k < sim::spawnGroupRange(a, 0).end; ++k)
    CHECK(a.spawnPoints[k].x < Fixed::zero());
}

TEST_CASE("capture: mode keys and validation (v0.33)", "[content][modes][capture]") {
  const std::string arena = "name = \"z\"\ncell = 1\nspawn_groups = 2\nmap = [\"a0.1b\"]";
  const auto mode = [&](std::string_view m) {
    return withArena(arena) + "\n[[mode]]\nname = \"c\"\nkind = \"capture\"\n" + std::string(m);
  };
  {
    ContentDB db{};
    Diagnostics diag;
    const bool ok = parseContent(mode("teams = 2\nflag_return_ticks = 90\n"), "m.toml", db, diag);
    INFO(diag.format());
    REQUIRE(ok);
    CHECK(db.modes[1].kind == sim::ModeKind::Capture);
    CHECK(db.modes[1].flagReturnTicks == 90);
    CHECK(db.compatible(0, 1));
  }
  ContentDB db{};
  Diagnostics diag;
  CHECK_FALSE(parseContent(mode(""), "m.toml", db, diag));
  CHECK(diag.format().contains("a team kind needs teams of 2 or more"));
  Diagnostics zero;
  CHECK_FALSE(parseContent(mode("teams = 2\nflag_return_ticks = 0\n"), "m.toml", db, zero));
  CHECK(zero.format().contains("'flag_return_ticks'"));
}

TEST_CASE("control: mode keys and validation (v0.79)", "[content][modes][control]") {
  const std::string arena = "name = \"z\"\ncell = 1\nspawn_groups = 2\nmap = [\"a0b1c\"]";
  const auto mode = [&](std::string_view m) {
    return withArena(arena) + "\n[[mode]]\nname = \"k\"\nkind = \"control\"\n" + std::string(m);
  };
  {
    ContentDB db{};
    Diagnostics diag;
    const bool ok =
        parseContent(mode("teams = 2\ncapture_ticks = 30\nhill_ticks = 60\nscore_to_win = 10\n"),
                     "m.toml", db, diag);
    INFO(diag.format());
    REQUIRE(ok);
    const sim::ModeDef& m = db.modes[1];
    CHECK(m.kind == sim::ModeKind::Control);
    CHECK(sim::scoresPoints(m.kind));
    CHECK(m.captureTicks == 30);
    CHECK(m.hillTicks == 60);
    CHECK(m.scoreToWin == 10);
    REQUIRE(db.arenas[0].zoneCount == 3);
    CHECK(db.compatible(0, 1));
    CHECK(db.firstCompatibleArena(1) == std::optional<uint8_t>{0});
    db.arenas[0].zoneCount = 0;  // control plays every zone, and needs one
    CHECK_FALSE(db.compatible(0, 1));
    CHECK_FALSE(db.firstCompatibleArena(1).has_value());
  }
  {
    // Four teams is the most: the clocks fill two words of modeData.
    ContentDB db{};
    Diagnostics diag;
    CHECK(parseContent(mode("teams = 4\n"), "m.toml", db, diag));
    CHECK(db.modes[1].captureTicks == 60);  // the default
  }
  const auto errors = [](const std::string& text) {
    ContentDB db{};
    Diagnostics diag;
    CHECK_FALSE(parseContent(text, "m.toml", db, diag));
    return diag.format();
  };
  CHECK(errors(mode("")).contains("a team kind needs teams of 2 or more"));
  CHECK(errors(mode("teams = 5\n")).contains("control plays at most 4 teams"));
  CHECK(errors(mode("teams = 2\ncapture_ticks = 0\n")).contains("'capture_ticks'"));
  CHECK(errors(mode("teams = 2\ncapture_ticks = 65536\n")).contains("'capture_ticks'"));
  CHECK(errors(mode("teams = 2\nhill_ticks = 0\n")).contains("'hill_ticks'"));
  CHECK(errors(mode("teams = 2\nscore_to_win = 0\n"))
            .contains("score_to_win must be at least 1 in control"));
  CHECK(errors(mode("teams = 2\nrespawn_ticks = 0\n"))
            .contains("respawn_ticks must be at least 1 in control"));
  {
    // validateContent() holds a mode built in code to the same.
    ContentDB db{};
    Diagnostics diag;
    REQUIRE(parseContent(mode("teams = 2\n"), "m.toml", db, diag));
    db.modes[1].captureTicks = 0;
    Diagnostics zero;
    CHECK_FALSE(validateContent(db, zero));
    CHECK(zero.format().contains("capture_ticks must be at least 1"));
  }
}
