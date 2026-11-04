// Zones and tilemap arenas (architecture §5.10, v0.33): glyphs, run merging,
// caps, zone rectangles, spawn order, and ContentDB::compatible().
#include "kfm/content/loader.hpp"
#include "kfm/sim/content.hpp"

#include <catch2/catch_test_macros.hpp>

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
  nine += "]";
  CHECK(arenaErrors(nine).contains("'zones' must list at most 8"));
}
