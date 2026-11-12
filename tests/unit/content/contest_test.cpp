// The contest's content as shipped (architecture §11 "Contest design", v0.80):
// the courtyard_zones arena, its spawners and the _1t power-up twins they
// lay, the contest_base baseline, and the banana's flight under the contest
// row. The row's own numbers are pinned by loader_test.cpp ("content/ ships
// contest as control ...").
#include "kfm/content/loader.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/spaces/grid_cells.hpp"
#include "kfm/sim/step.hpp"
#include "kfm/sim/visit.hpp"
#include "kfm/sim/world_state.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

using namespace kfm;
using sim::ContentDB;

namespace {

template <class T>
std::vector<std::byte> bytesOf(const T& def) {
  std::vector<std::byte> buf(sizeof(T) * 2 + 16);
  BitWriter w(buf);
  REQUIRE(sim::writeTo(w, def));
  buf.resize(w.bytesWritten());
  return buf;
}

const ContentDB& shipped() {
  static const ContentDB db = [] {
    ContentDB c{};
    content::Diagnostics diag;
    REQUIRE(content::loadContentDir(KFM_CONTENT_DIR, c, diag));
    return c;
  }();
  return db;
}

[[nodiscard]] Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

// The point under the arena's half turn about its centre (the origin).
[[nodiscard]] Vec2 turned(Vec2 p) {
  return Vec2{-p.x, -p.y};
}

// Whether the box [min, max] turns into the box [min2, max2].
[[nodiscard]] bool turnsInto(Vec2 min, Vec2 max, Vec2 min2, Vec2 max2) {
  return turned(max) == min2 && turned(min) == max2;
}

template <class Box, size_t N>
[[nodiscard]] bool hasTurnedTwin(const std::array<Box, N>& boxes, uint8_t count, const Box& b) {
  for (uint8_t i = 0; i < count; ++i) {
    if (turnsInto(b.min, b.max, boxes[i].min, boxes[i].max)) return true;
  }
  return false;
}

[[nodiscard]] uint8_t arenaIndex() {
  const std::optional<uint8_t> i = shipped().findArena("courtyard_zones");
  REQUIRE(i.has_value());
  return i.value_or(0);
}

[[nodiscard]] const sim::ArenaDef& zonesArena() {
  return shipped().arenas[arenaIndex()];
}

[[nodiscard]] uint8_t contestIndex() {
  const std::optional<uint8_t> i = shipped().findMode("contest");
  REQUIRE(i.has_value());
  return i.value_or(0);
}

[[nodiscard]] uint16_t bitOf(std::string_view powerUp) {
  const std::optional<uint8_t> i = shipped().findPowerUp(powerUp);
  INFO("power-up " << powerUp);
  REQUIRE(i.has_value());
  return static_cast<uint16_t>(1u << i.value_or(0));
}

}  // namespace

TEST_CASE("courtyard_zones: the courtyard with three zones in the designed order (v0.80)",
          "[content][contest]") {
  const ContentDB& db = shipped();
  const sim::ArenaDef& a = zonesArena();
  const std::optional<uint8_t> courtyard = db.findArena("courtyard");
  const std::optional<uint8_t> hill = db.findArena("courtyard_hill");
  REQUIRE(courtyard);
  REQUIRE(hill);
  // Appended after courtyard_hill: the arenas before it keep their indices.
  CHECK(arenaIndex() == hill.value_or(0) + 1);
  const sim::ArenaDef& c = db.arenas[courtyard.value_or(0)];
  const sim::ArenaDef& h = db.arenas[hill.value_or(0)];
  CHECK(a.cellSize == Fixed::one());
  CHECK(a.halfExtents == c.halfExtents);  // 24 x 14 cells
  CHECK(a.spawnGroups == 2);
  CHECK(a.spawnPointCount == 10);
  for (uint8_t i = 0; i < c.spawnPointCount; ++i)
    CHECK(a.spawnPoints[i] == c.spawnPoints[i]);
  CHECK(a.hazardCount == c.hazardCount);
  for (uint8_t i = 0; i < c.hazardCount; ++i)
    CHECK((a.hazards[i].min == c.hazards[i].min && a.hazards[i].max == c.hazards[i].max &&
           a.hazards[i].kind == c.hazards[i].kind));
  // The hill's walls: the courtyard's less its middle block.
  REQUIRE(a.wallCount == h.wallCount);
  for (uint8_t i = 0; i < h.wallCount; ++i)
    CHECK((a.walls[i].min == h.walls[i].min && a.walls[i].max == h.walls[i].max));
  // Zone 0 the centre (courtyard_hill's hill, between the pillars), zone 1
  // the west home (columns 4-6), zone 2 the east home (columns 17-19), all
  // on rows 6-7.
  REQUIRE(a.zoneCount == 3);
  CHECK(a.zones[0].min == h.zones[0].min);
  CHECK(a.zones[0].max == h.zones[0].max);
  CHECK(a.zones[0].min == at(-2, -1));
  CHECK(a.zones[0].max == at(2, 1));
  CHECK(a.zones[1].min == at(-8, -1));
  CHECK(a.zones[1].max == at(-5, 1));
  CHECK(a.zones[2].min == at(5, -1));
  CHECK(a.zones[2].max == at(8, 1));
  // Each home zone is on its own team's side: team 0 (group 0) spawns in
  // the west, team 1 in the east.
  const sim::SpawnRange west = sim::spawnGroupRange(a, 0);
  const sim::SpawnRange east = sim::spawnGroupRange(a, 1);
  for (uint8_t i = west.begin; i < west.end; ++i)
    CHECK(a.spawnPoints[i].x < a.zones[1].min.x);
  for (uint8_t i = east.begin; i < east.end; ++i)
    CHECK(a.spawnPoints[i].x > a.zones[2].max.x);
}

TEST_CASE("courtyard_zones is symmetric under a half turn, zone 1 mapping to zone 2",
          "[content][contest]") {
  const sim::ArenaDef& a = zonesArena();
  for (uint8_t i = 0; i < a.wallCount; ++i) {
    INFO("wall " << int{i});
    CHECK(hasTurnedTwin(a.walls, a.wallCount, a.walls[i]));
  }
  for (uint8_t i = 0; i < a.hazardCount; ++i) {
    INFO("hazard " << int{i});
    CHECK(hasTurnedTwin(a.hazards, a.hazardCount, a.hazards[i]));
  }
  REQUIRE(a.zoneCount == 3);
  CHECK(turnsInto(a.zones[0].min, a.zones[0].max, a.zones[0].min, a.zones[0].max));
  CHECK(turnsInto(a.zones[1].min, a.zones[1].max, a.zones[2].min, a.zones[2].max));
  CHECK(turnsInto(a.zones[2].min, a.zones[2].max, a.zones[1].min, a.zones[1].max));
  // Spawn point k of group 0 turns into point k of group 1.
  const sim::SpawnRange west = sim::spawnGroupRange(a, 0);
  const sim::SpawnRange east = sim::spawnGroupRange(a, 1);
  REQUIRE(west.end - west.begin == east.end - east.begin);
  for (uint8_t k = 0; k < west.end - west.begin; ++k) {
    INFO("spawn " << int{k} << " of each group");
    CHECK(turned(a.spawnPoints[west.begin + k]) == a.spawnPoints[east.begin + k]);
  }
  // The two spawners turn into each other and lay alike.
  REQUIRE(a.pickupSpawnerCount == 2);
  const sim::PickupSpawner& s0 = a.pickupSpawners[0];
  const sim::PickupSpawner& s1 = a.pickupSpawners[1];
  CHECK(turned(s0.pos) == s1.pos);
  CHECK(s0.powerUpSet == s1.powerUpSet);
  CHECK(s0.firstTicks == s1.firstTicks);
  CHECK(s0.intervalTicks == s1.intervalTicks);
  CHECK(s0.maxAlive == s1.maxAlive);
}

TEST_CASE("courtyard_zones lays the _1t twins early and often, away from every zone",
          "[content][contest][powerup]") {
  const sim::ArenaDef& a = zonesArena();
  const auto twins = static_cast<uint16_t>(bitOf("big_banana_1t") | bitOf("triplebanana_1t") |
                                           bitOf("quick_hands_1t") | bitOf("shield_1t"));
  REQUIRE(a.pickupSpawnerCount == 2);
  for (uint8_t i = 0; i < a.pickupSpawnerCount; ++i) {
    INFO("spawner " << int{i});
    const sim::PickupSpawner& s = a.pickupSpawners[i];
    CHECK(s.powerUpSet == twins);
    CHECK((s.powerUpSet & bitOf("speed_boost")) == 0);  // a walk is a cell a turn regardless
    CHECK(s.firstTicks == 90);
    CHECK(s.intervalTicks == 240);
    CHECK(s.maxAlive == 1);
    // On a cell centre in the centre columns (11 or 12), three cells or more
    // (Chebyshev) from every zone cell.
    const sim::grid::Cell cell = sim::grid::cellOf(a, s.pos);
    CHECK(sim::grid::cellCentre(a, cell) == s.pos);
    CHECK((cell.col == 11 || cell.col == 12));
    // A cell centre three cells from a zone cell is 2.5 units from the zone's
    // edge, one two cells away 1.5: the box grown by 2.25 holds the second.
    const Fixed reach = Fixed::fromRatio(9, 4);
    for (uint8_t z = 0; z < a.zoneCount; ++z) {
      INFO("zone " << int{z});
      CHECK_FALSE(sim::boxContains(a.zones[z].min - Vec2{reach, reach},
                                   a.zones[z].max + Vec2{reach, reach}, s.pos));
    }
  }
}

TEST_CASE("the contest plays on courtyard_zones", "[content][contest]") {
  const ContentDB& db = shipped();
  const uint8_t contest = contestIndex();
  CHECK(db.modes[contest].arena == arenaIndex());
  CHECK(db.compatible(arenaIndex(), contest));
  CHECK(db.firstCompatibleArena(contest) == std::optional<uint8_t>{arenaIndex()});
  // The other team modes play there too (two spawn groups, cells, zones).
  for (const std::string_view name : {"hill", "grid_rounds_1t", "team_rounds"}) {
    INFO(name);
    const std::optional<uint8_t> m = db.findMode(name);
    REQUIRE(m);
    CHECK(db.compatible(arenaIndex(), m.value_or(0)));
  }
}

TEST_CASE("the _1t power-up twins are their originals counted in 1-tick turns (v0.80)",
          "[content][contest][powerup]") {
  const ContentDB& db = shipped();
  const std::array<std::tuple<std::string_view, std::string_view, uint16_t>, 4> twins{{
      {"big_banana_1t", "big_banana", 60},
      {"triplebanana_1t", "triple_banana", 90},
      {"quick_hands_1t", "quick_hands", 60},
      {"shield_1t", "shield", 90},
  }};
  const uint8_t gridBase = db.findPowerUp("grid_base_1t").value_or(0xFF);
  for (const auto& [name, original, turns] : twins) {
    INFO(name);
    const std::optional<uint8_t> t = db.findPowerUp(name);
    const std::optional<uint8_t> o = db.findPowerUp(original);
    REQUIRE(t);
    REQUIRE(o);
    CHECK(t.value_or(0) > gridBase);  // appended: the earlier indices stay
    const sim::PowerUpDef& orig = db.powerUps[o.value_or(0)];
    sim::PowerUpDef twin = db.powerUps[t.value_or(0)];
    CHECK(twin.durationTicks == turns);
    // Everything else, grants and looks, is the original's.
    twin.name = orig.name;
    twin.durationTicks = orig.durationTicks;
    CHECK(bytesOf(twin) == bytesOf(orig));
  }
}

TEST_CASE("contest_base is grid_base_1t with the banana at 2 cells a turn (v0.80)",
          "[content][contest][powerup]") {
  const ContentDB& db = shipped();
  const std::optional<uint8_t> base = db.findPowerUp("contest_base");
  const std::optional<uint8_t> grid = db.findPowerUp("grid_base_1t");
  REQUIRE(base);
  REQUIRE(grid);
  CHECK(db.modes[contestIndex()].baseline == base.value_or(0));
  const sim::PowerUpDef& g = db.powerUps[grid.value_or(0)];
  sim::PowerUpDef twin = db.powerUps[base.value_or(0)];
  REQUIRE(twin.grantCount == 3);
  REQUIRE(twin.grants[2].kind == sim::GrantKind::Projectile);
  CHECK(twin.grants[2].projectile.speedMul == Fixed::fromRatio(15, 2));
  // 2 cells per 1-tick turn, within the per-tick truncation of 16 u/s.
  const Fixed perTurn = db.boomerang.speed * twin.grants[2].projectile.speedMul;
  CHECK(abs(perTurn - Fixed::fromInt(2)) < Fixed::fromRatio(1, 1000));
  // grid_rounds_1t's stays at 4.
  CHECK(g.grants[2].projectile.speedMul == Fixed::fromInt(4));
  twin.name = g.name;
  twin.grants[2].projectile.speedMul = g.grants[2].projectile.speedMul;
  CHECK(bytesOf(twin) == bytesOf(g));
}

// The flight the row's banana_outbound_ticks and banana_turn_rate give, through
// step() on the shipped content: a monkey at rest in the open row 4 (column
// 2) throws east. As measured when the numbers were chosen (v0.80): 2 cells a
// turn, 5 cells straight out (the spawn offset and two turns), bending home
// on the third, at most 2.34 cells off the line, back in its hand 5 turns
// after the press.
TEST_CASE("the contest banana flies 3 turns out and comes home within a few cells",
          "[content][contest][flight]") {
  const ContentDB& db = shipped();
  const uint8_t mode = contestIndex();
  const uint8_t arena = arenaIndex();
  const sim::PlayerMask players = 0b11;
  const sim::MatchConfig cfg{.seed = 7,
                             .players = players,
                             .arena = arena,
                             .mode = mode,
                             .teams = sim::teamsFor(db.modes[mode], players)};
  const sim::MatchContent m(db, arena, mode, sim::kAllPowerUps, 0, cfg.teams);
  CHECK(m.bananaOutboundTicks() == 3);
  CHECK(m.bananaTurnRateBam() == 15474);  // 85 degrees
  sim::WorldState s = sim::initialWorld(cfg);
  sim::EventSink sink;
  const auto tick = [&](sim::PlayerInput p0) {
    sim::InputFrame f;
    f.tick = s.tick;
    f.present = s.rules.present;
    f.inputs[0] = p0;
    sink.clear();
    sim::step(s, f, m, sink);
    REQUIRE(sink.dropped == 0);
  };
  for (int i = 0; i < 200 && s.rules.phase != sim::MatchPhase::Playing; ++i)
    tick({});
  REQUIRE(s.rules.phase == sim::MatchPhase::Playing);
  const auto put = [&](PlayerId id, sim::grid::Cell cell) {
    sim::Player* p = s.playerBySlot(id);
    REQUIRE(p != nullptr);
    if (p == nullptr) return;  // for the analysers: REQUIRE has thrown
    p->transform.pos = sim::grid::cellCentre(m.arena, cell);
    p->transform.prevPos = p->transform.pos;
    p->moveTicks = 0;
  };
  const sim::grid::Cell home{2, 4};
  put(PlayerId{0}, home);
  put(PlayerId{1}, sim::grid::Cell{22, 12});  // out of the way
  tick({});
  const Vec2 from = sim::grid::cellCentre(m.arena, home);
  sim::PlayerInput press{};
  press.buttons = sim::buttonBit(sim::Button::Throw);
  press.aimDir = 0;  // east, along the open row
  tick(press);

  int caughtAfter = 0;
  int returningFrom = 0;
  Fixed maxAhead{};
  Fixed maxSide{};
  for (int t = 1; t <= 20 && caughtAfter == 0; ++t) {
    tick({});
    for (const sim::SimEvent& e : sink) {
      if (e.kind == sim::EventKind::BoomerangCaught && e.player == PlayerId{0}) caughtAfter = t;
    }
    s.boomerangs.forEachAlive([&](EntityId /*id*/, const sim::Boomerang& b) {
      if (b.phase == sim::BoomerangPhase::Returning && returningFrom == 0) returningFrom = t;
      const Vec2 d = b.transform.pos - from;
      maxAhead = max(maxAhead, d.x);
      maxSide = max(maxSide, abs(d.y));
    });
  }
  CHECK(returningFrom == 2);  // the press tick, the next, then the turn home
  CHECK(maxAhead >= Fixed::fromInt(5));
  CHECK(maxAhead < Fixed::fromInt(6));
  CHECK(maxSide < Fixed::fromRatio(5, 2));
  CHECK(caughtAfter == 5);
}

// --- the harvest pool (v0.81, §11 "Contest design: bananas as the economy") ---

namespace {

// What each pool arena is drawn to be: its size, its boxes, its trees by
// beat and where its homes lie. Positions are cell centres (half units).
struct PoolArena {
  std::string_view name;
  int32_t columns;
  int32_t rows;
  uint8_t walls;
  uint8_t pits;
  uint8_t richTrees;   // every 30 turns, up to 3
  uint8_t flankTrees;  // every 60 turns, up to 2
};

constexpr std::array<PoolArena, 4> kPool{{
    {"grove", 24, 14, 6, 2, 2, 2},
    {"orchards", 28, 16, 13, 0, 0, 4},
    {"gorge", 26, 16, 6, 3, 2, 2},
    {"burrows", 20, 12, 16, 0, 2, 2},
}};

// A tree's beat: first and interval in turns (1-tick turns), max_alive.
struct Beat {
  uint16_t first;
  uint16_t interval;
  uint8_t maxAlive;
};
constexpr Beat kHomeBeat{90, 90, 1};
constexpr Beat kFlankBeat{60, 60, 2};
constexpr Beat kRichBeat{30, 30, 3};

[[nodiscard]] bool hasBeat(const sim::PickupSpawner& s, Beat b) {
  return s.firstTicks == b.first && s.intervalTicks == b.interval && s.maxAlive == b.maxAlive;
}

[[nodiscard]] const sim::ArenaDef& arenaNamed(std::string_view name) {
  const std::optional<uint8_t> i = shipped().findArena(name);
  INFO("arena " << name);
  REQUIRE(i.has_value());
  return shipped().arenas[i.value_or(0)];
}

// A cell a monkey can stand on: its centre in no wall box and no pit.
[[nodiscard]] bool standable(const sim::ArenaDef& a, sim::grid::Cell k) {
  if (!sim::grid::inside(a, k)) return false;
  const Vec2 c = sim::grid::cellCentre(a, k);
  for (uint8_t w = 0; w < a.wallCount; ++w) {
    if (sim::boxContains(a.walls[w].min, a.walls[w].max, c)) return false;
  }
  for (uint8_t h = 0; h < a.hazardCount; ++h) {
    if (a.hazards[h].kind == sim::HazardKind::Pit &&
        sim::boxContains(a.hazards[h].min, a.hazards[h].max, c))
      return false;
  }
  return true;
}

// The standable cells reached from a zone's cells by steps to the four
// orthogonal neighbours over standable cells (no pit is crossed).
[[nodiscard]] std::vector<bool> reachedFrom(const sim::ArenaDef& a, const sim::Zone& z) {
  const int32_t cols = sim::grid::columns(a);
  const int32_t rows = sim::grid::rows(a);
  std::vector<bool> seen(static_cast<size_t>(cols) * static_cast<size_t>(rows), false);
  std::vector<sim::grid::Cell> queue;
  const auto at = [&](sim::grid::Cell k) {
    return (static_cast<size_t>(k.row) * static_cast<size_t>(cols)) + static_cast<size_t>(k.col);
  };
  for (int32_t r = 0; r < rows; ++r) {
    for (int32_t c = 0; c < cols; ++c) {
      const sim::grid::Cell k{c, r};
      if (standable(a, k) && sim::boxContains(z.min, z.max, sim::grid::cellCentre(a, k))) {
        seen[at(k)] = true;
        queue.push_back(k);
      }
    }
  }
  for (size_t head = 0; head < queue.size(); ++head) {
    const sim::grid::Cell k = queue[head];
    for (const uint8_t o : {uint8_t{0}, uint8_t{2}, uint8_t{4}, uint8_t{6}}) {
      const sim::grid::Cell n = sim::grid::neighbour(k, o);
      if (!standable(a, n) || seen[at(n)]) continue;
      seen[at(n)] = true;
      queue.push_back(n);
    }
  }
  return seen;
}

// Chebyshev distance in cells from a cell centre to the nearest cell of a
// zone (0 inside it).
[[nodiscard]] int32_t cellsFrom(const sim::ArenaDef& a, const sim::Zone& z, Vec2 p) {
  const sim::grid::Cell k = sim::grid::cellOf(a, p);
  const sim::grid::Cell lo = sim::grid::cellOf(a, Vec2{z.min.x, z.max.y});  // north-west cell
  const sim::grid::Cell hi =
      sim::grid::cellOf(a, Vec2{z.max.x - Fixed::fromRaw(1), z.min.y + Fixed::fromRaw(1)});
  const int32_t dc = std::max({lo.col - k.col, k.col - hi.col, 0});
  const int32_t dr = std::max({lo.row - k.row, k.row - hi.row, 0});
  return std::max(dc, dr);
}

}  // namespace

TEST_CASE("the harvest pool: grove, orchards, gorge and burrows appended in that order (v0.81)",
          "[content][contest][harvest]") {
  const ContentDB& db = shipped();
  CHECK(db.arenaCount == 15);  // of kMaxArenas' 16
  for (size_t k = 0; k < kPool.size(); ++k) {
    INFO(kPool[k].name);
    CHECK(db.findArena(kPool[k].name) == std::optional<uint8_t>{static_cast<uint8_t>(11 + k)});
  }
}

TEST_CASE("each harvest pool arena is its own half turn", "[content][contest][harvest]") {
  const PoolArena& want = GENERATE(from_range(kPool));
  INFO(want.name);
  const sim::ArenaDef& a = arenaNamed(want.name);
  for (uint8_t i = 0; i < a.wallCount; ++i) {
    INFO("wall " << int{i});
    CHECK(hasTurnedTwin(a.walls, a.wallCount, a.walls[i]));
  }
  for (uint8_t i = 0; i < a.hazardCount; ++i) {
    INFO("hazard " << int{i});
    CHECK(hasTurnedTwin(a.hazards, a.hazardCount, a.hazards[i]));
  }
  // The two homes turn into each other.
  REQUIRE(a.zoneCount == 2);
  CHECK(turnsInto(a.zones[0].min, a.zones[0].max, a.zones[1].min, a.zones[1].max));
  // Spawn point k of group 0 turns into point k of group 1.
  REQUIRE(a.spawnGroups == 2);
  REQUIRE(a.spawnPointCount == 10);
  const sim::SpawnRange west = sim::spawnGroupRange(a, 0);
  const sim::SpawnRange east = sim::spawnGroupRange(a, 1);
  REQUIRE(west.end - west.begin == 5);
  REQUIRE(east.end - east.begin == 5);
  for (uint8_t k = 0; k < 5; ++k) {
    INFO("spawn " << int{k} << " of each group");
    CHECK(turned(a.spawnPoints[west.begin + k]) == a.spawnPoints[east.begin + k]);
  }
  // Every spawner turns into one that lays alike: trees into trees of the
  // same beat (no Rng draw, so their fruit stays mirrored), power-up
  // spawners into ones with the same set and beat.
  for (uint8_t i = 0; i < a.pickupSpawnerCount; ++i) {
    INFO("spawner " << int{i});
    const sim::PickupSpawner& s = a.pickupSpawners[i];
    int twins = 0;
    for (uint8_t j = 0; j < a.pickupSpawnerCount; ++j) {
      const sim::PickupSpawner& t = a.pickupSpawners[j];
      if (j != i && turned(s.pos) == t.pos && s.tree == t.tree && s.powerUpSet == t.powerUpSet &&
          s.firstTicks == t.firstTicks && s.intervalTicks == t.intervalTicks &&
          s.maxAlive == t.maxAlive)
        ++twins;
    }
    CHECK(twins == 1);
  }
}

TEST_CASE("each harvest pool arena: a home per team, its trees, its spawners, its capacities",
          "[content][contest][harvest]") {
  const PoolArena& want = GENERATE(from_range(kPool));
  INFO(want.name);
  const sim::ArenaDef& a = arenaNamed(want.name);
  CHECK(a.cellSize == Fixed::one());
  CHECK(sim::grid::columns(a) == want.columns);
  CHECK(sim::grid::rows(a) == want.rows);
  // Within what bots already see: 32 walls, 16 hazards, 8 spawners, 10
  // spawn points (the loader refuses more; these are the counts drawn).
  CHECK(a.wallCount == want.walls);
  CHECK(a.hazardCount == want.pits);
  for (uint8_t h = 0; h < a.hazardCount; ++h)
    CHECK(a.hazards[h].kind == sim::HazardKind::Pit);
  CHECK(a.pickupSpawnerCount == sim::kMaxPickupSpawners);

  // Team t's home is zone t: zone 0 in the west, zone 1 in the east, each
  // team's spawn group near its own home (team 0 spawns from group 0).
  REQUIRE(a.zoneCount == 2);
  CHECK(a.zones[0].max.x < Fixed::zero());
  CHECK(a.zones[1].min.x > Fixed::zero());
  for (uint8_t team = 0; team < 2; ++team) {
    CHECK(sim::homeZoneOf(a, team) == team);
    const sim::SpawnRange g = sim::spawnGroupRange(a, team);
    for (uint8_t i = g.begin; i < g.end; ++i) {
      INFO("team " << int{team} << " spawn " << int{i});
      CHECK(cellsFrom(a, a.zones[team], a.spawnPoints[i]) <= 5);
      CHECK(cellsFrom(a, a.zones[team], a.spawnPoints[i]) <
            cellsFrom(a, a.zones[1 - team], a.spawnPoints[i]));
    }
  }

  // The trees: a poor home tree per team, nearer its own home than the
  // other's; rich and flank trees in the field, at most four; and two
  // power-up spawners laying the three _1t twins (never triplebanana_1t,
  // whose ammo_max grant would top a hand up from nowhere).
  const auto twins =
      static_cast<uint16_t>(bitOf("big_banana_1t") | bitOf("quick_hands_1t") | bitOf("shield_1t"));
  std::array<int, 2> homeTrees{};
  int rich = 0;
  int flank = 0;
  int powerUps = 0;
  for (uint8_t i = 0; i < a.pickupSpawnerCount; ++i) {
    INFO("spawner " << int{i});
    const sim::PickupSpawner& s = a.pickupSpawners[i];
    // On a cell centre a monkey can stand on, outside both homes.
    const sim::grid::Cell cell = sim::grid::cellOf(a, s.pos);
    CHECK(sim::grid::cellCentre(a, cell) == s.pos);
    CHECK(standable(a, cell));
    CHECK(cellsFrom(a, a.zones[0], s.pos) > 0);
    CHECK(cellsFrom(a, a.zones[1], s.pos) > 0);
    if (!s.isTree()) {
      ++powerUps;
      CHECK(s.powerUpSet == twins);
      CHECK(s.firstTicks == 90);
      CHECK(s.intervalTicks == 240);
      CHECK(s.maxAlive == 1);
      continue;
    }
    CHECK(s.powerUpSet == 0);
    if (hasBeat(s, kHomeBeat)) {
      const uint8_t team =
          cellsFrom(a, a.zones[0], s.pos) < cellsFrom(a, a.zones[1], s.pos) ? 0 : 1;
      CHECK(cellsFrom(a, a.zones[team], s.pos) <= 3);
      ++homeTrees[team];
    } else if (hasBeat(s, kFlankBeat)) {
      ++flank;
    } else {
      CHECK(hasBeat(s, kRichBeat));
      ++rich;
    }
  }
  CHECK(homeTrees == std::array<int, 2>{1, 1});
  CHECK(rich == want.richTrees);
  CHECK(flank == want.flankTrees);
  CHECK(rich + flank <= 4);
  CHECK(powerUps == 2);
}

TEST_CASE("each harvest pool arena: every floor cell is reached from both homes without a pit",
          "[content][contest][harvest]") {
  const PoolArena& want = GENERATE(from_range(kPool));
  INFO(want.name);
  const sim::ArenaDef& a = arenaNamed(want.name);
  REQUIRE(a.zoneCount == 2);
  const std::vector<bool> fromWest = reachedFrom(a, a.zones[0]);
  const std::vector<bool> fromEast = reachedFrom(a, a.zones[1]);
  int floor = 0;
  for (int32_t r = 0; r < sim::grid::rows(a); ++r) {
    for (int32_t c = 0; c < sim::grid::columns(a); ++c) {
      const sim::grid::Cell k{c, r};
      if (!standable(a, k)) continue;
      ++floor;
      INFO("cell " << c << ", " << r);
      const size_t i = (static_cast<size_t>(r) * static_cast<size_t>(sim::grid::columns(a))) +
                       static_cast<size_t>(c);
      CHECK(fromWest[i]);
      CHECK(fromEast[i]);
    }
  }
  CHECK(floor > 0);
}
