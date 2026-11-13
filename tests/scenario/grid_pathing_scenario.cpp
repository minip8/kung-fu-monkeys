// The scripted generator's harvest steering in the grid space
// (kfm/sim/scripted_inputs.hpp): the route (gridStepToward, a breadth-first
// search by the grid's movement rules, walls and pits closed, no corner
// cutting) and the roles that play the kind (gatherers, a raider, a
// defender), Rng-free, then a whole scripted match on a small walled arena
// that gathers, banks and steals. Hand-built content only: no shipped row or
// arena (the corpus plays those, tests/determinism/corpus_events_test.cpp).
#include "kfm/sim/scripted_inputs.hpp"
#include "kfm/sim/spaces/grid_cells.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;
using grid::Cell;

namespace {

constexpr uint8_t kMap = 1;      // the arena under test (each test builds its own)
constexpr uint8_t kHarvest = 1;  // harvest in the grid space, 1-tick turns
constexpr uint8_t kTdm = 2;      // the same as team deathmatch (no objective)
constexpr Fixed kRadius = Fixed::fromRatio(35, 100);
constexpr uint8_t kHand = 3;  // the baseline's ammo_max: 1 + 2

// A tilemap arena of one-unit cells: '#' a wall cell, '~' a pit cell, 'a'
// and 'b' the cells of zone 0 and zone 1 (one rectangle each), 'T' a tree,
// a digit k spawn point k (two spawn groups), '.' floor. A row's run of
// walls or pits is one box.
ArenaDef mapArena(std::initializer_list<std::string_view> map) {
  ArenaDef a{};
  a.name = ContentName::of("map");
  a.cellSize = Fixed::one();
  const auto rows = static_cast<int32_t>(map.size());
  const auto cols = static_cast<int32_t>(map.begin()->size());
  a.halfExtents = Vec2{Fixed::fromRatio(cols, 2), Fixed::fromRatio(rows, 2)};
  const Vec2 half{Fixed::half(), Fixed::half()};
  const auto lo = [&](Cell k) { return grid::cellCentre(a, k) - half; };
  const auto hi = [&](Cell k) { return grid::cellCentre(a, k) + half; };
  std::array<bool, 2> zoneSeen{};
  int32_t row = 0;
  for (const std::string_view line : map) {
    REQUIRE(static_cast<int32_t>(line.size()) == cols);
    for (int32_t col = 0; col < cols; ++col) {
      const char g = line[static_cast<size_t>(col)];
      const Cell k{col, row};
      if (g == '#' || g == '~') {
        const auto at = static_cast<size_t>(col);
        if (at > 0 && line[at - 1] == g) continue;  // in the run
        size_t end = at;
        while (end + 1 < line.size() && line[end + 1] == g)
          ++end;
        if (g == '#') {
          REQUIRE(a.wallCount < kMaxArenaWalls);
          a.walls[a.wallCount++] = Wall{lo(k), hi(Cell{static_cast<int32_t>(end), row})};
        } else {
          REQUIRE(a.hazardCount < kMaxArenaHazards);
          a.hazards[a.hazardCount++] = Hazard{lo(k), hi(Cell{static_cast<int32_t>(end), row}),
                                              HazardKind::Pit, Fixed::one()};
        }
      } else if (g == 'a' || g == 'b') {
        const auto z = static_cast<size_t>(g - 'a');
        Zone& zone = a.zones[z];
        if (!zoneSeen[z]) {
          zone = Zone{lo(k), hi(k)};
          zoneSeen[z] = true;
        }
        zone.min = Vec2{min(zone.min.x, lo(k).x), min(zone.min.y, lo(k).y)};
        zone.max = Vec2{max(zone.max.x, hi(k).x), max(zone.max.y, hi(k).y)};
        a.zoneCount = std::max<uint8_t>(a.zoneCount, static_cast<uint8_t>(z + 1));
      } else if (g == 'T') {
        REQUIRE(a.pickupSpawnerCount < kMaxPickupSpawners);
        PickupSpawner tree;
        tree.pos = grid::cellCentre(a, k);
        tree.firstTicks = 30;
        tree.intervalTicks = 30;
        tree.maxAlive = 3;
        tree.tree = 1;
        a.pickupSpawners[a.pickupSpawnerCount++] = tree;
      } else if (g >= '0' && g <= '9') {
        const auto i = static_cast<uint8_t>(g - '0');
        a.spawnPoints[i] = grid::cellCentre(a, k);
        a.spawnPointCount = std::max<uint8_t>(a.spawnPointCount, static_cast<uint8_t>(i + 1));
      }
    }
    ++row;
  }
  a.spawnGroups = 2;
  return a;
}

// defaultContent() with monkeys of radius 0.35, the map as arena 1 and
// harvest in the grid space (1-tick turns, bank every 2, steal every 6, the
// hand three, a respawn empty-handed) as mode 1, its team-deathmatch twin
// as mode 2.
ContentDB pathContent(const ArenaDef& arena) {
  ContentDB c = defaultContent();
  removeThrowCharge(c);
  c.player.radius = kRadius;
  const uint8_t base = addPowerUp(c, "harvest_base", 1,
                                  {statGrant(StatKind::AmmoMax, Fixed::one(), Fixed::fromInt(2))});
  c.arenas[kMap] = arena;
  c.arenaCount = 2;
  ModeDef& m = c.modes[kHarvest];
  m = c.modes[0];
  m.name = ContentName::of("harvest_grid");
  m.kind = ModeKind::Harvest;
  m.space = Space::Grid;
  m.turnTicks = 1;
  m.dashCells = 2;
  m.teams = 2;
  m.bankTicks = 2;
  m.stealTicks = 6;
  m.scoreToWin = 99;
  m.timeLimitTicks = 60000;
  m.roundStartTicks = 10;
  m.respawnTicks = 60;
  m.respawnInvulnTicks = 0;
  m.respawnAmmo = 0;
  m.deathBananas = DeathBananas::Drop;
  m.baseline = base;
  m.arena = kMap;
  c.modes[kTdm] = m;
  c.modes[kTdm].name = ContentName::of("tdm_grid");
  c.modes[kTdm].kind = ModeKind::TeamDeathmatch;
  c.modeCount = 3;
  AbilityDef& thr = c.abilities[c.player.abilities[kAbilitySlotThrow]];
  thr.recoveryTicks = 1;  // a turn, rooted (as content/'s grid throws)
  thr.rootPhases = phaseBit(AbilityPhase::Recovery);
  return c;
}

// A harvest match on the map in Playing.
struct Match {
  ContentDB db;
  MatchConfig config;
  MatchContent content;
  WorldState s;
  EventLog log;

  Match(const ArenaDef& arena, PlayerMask players, uint8_t mode = kHarvest)
      : db(pathContent(arena)),
        config{.seed = 0x9A7B,
               .players = players,
               .arena = kMap,
               .mode = mode,
               .teams = teamsFor(db.modeAt(mode), players)},
        content(matchContent(db, config)),
        s(initialWorld(config)) {
    REQUIRE(db.compatible(kMap, mode));
    for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i)
      runTicks(s, frameFor(s), 1, content, log);
    REQUIRE(s.rules.phase == MatchPhase::Playing);
    log.clear();
  }
  [[nodiscard]] const ArenaDef& arena() const { return content.arena; }
  [[nodiscard]] Vec2 at(Cell k) const { return grid::cellCentre(arena(), k); }
  Player& put(PlayerId id, Cell k, uint8_t ammo) {
    Player& p = placePlayer(s, id, at(k));
    p.ammo = ammo;
    return p;
  }
  // Every slot of the match off the board's goals: parked on `k`.
  void parkAll(Cell k) {
    for (uint8_t i = 0; i < kMaxPlayers; ++i) {
      if (s.playerBySlot(PlayerId{i}) != nullptr) put(PlayerId{i}, k, 0);
    }
  }
  void clearPickups() {
    s.pickups.forEachAlive([&](EntityId id, const Pickup& /*k*/) { s.pickups.despawn(id); });
  }
  [[nodiscard]] PlayerInput input(PlayerId id) const {
    Rng rng = Rng::seeded(7);
    return scriptedInput(rng, s, content, id);
  }
};

// The moveDir of an octant.
constexpr uint8_t dirOf(uint8_t octant) {
  return static_cast<uint8_t>(octant * 32);
}

// The cells whose centre lies in zone k.
std::vector<Cell> zoneCells(const ArenaDef& a, uint8_t k) {
  std::vector<Cell> out;
  for (int32_t row = 0; row < grid::rows(a); ++row) {
    for (int32_t col = 0; col < grid::columns(a); ++col) {
      if (boxContains(a.zones[k].min, a.zones[k].max, grid::cellCentre(a, Cell{col, row}))) {
        out.push_back(Cell{col, row});
      }
    }
  }
  return out;
}

// The fewest turns from a cell to any of the goals (0xFFFF: none reachable).
uint16_t turnsToAny(const ArenaDef& a, Cell from, const std::vector<Cell>& goals) {
  uint16_t best = 0xFFFF;
  for (const Cell g : goals) {
    const GridStep step =
        gridStepToward(a, grid::cellCentre(a, from), kRadius, grid::cellCentre(a, g));
    if (step.reachable) best = std::min(best, step.turns);
  }
  return best;
}

// Whether the input is a step the grid takes (one turn) that brings the
// monkey one turn nearer the nearest of the goals by the route: the first
// step of a shortest route to them, whichever of several.
bool onShortestRoute(const Match& m, PlayerId id, const PlayerInput& in,
                     const std::vector<Cell>& goals) {
  if (in.moveMag == 0 || in.moveDir % 32 != 0) return false;
  const ArenaDef& a = m.arena();
  const Cell here = grid::cellOf(a, playerAt(m.s, id).transform.pos);
  const Cell next = grid::neighbour(here, grid::octantOf(in.moveDir));
  const GridStep one =
      gridStepToward(a, grid::cellCentre(a, here), kRadius, grid::cellCentre(a, next));
  const uint16_t now = turnsToAny(a, here, goals);
  return one.reachable && one.turns == 1 && now != 0xFFFF && turnsToAny(a, next, goals) + 1 == now;
}

// A wall down the middle column with one gap, at the bottom.
const std::initializer_list<std::string_view> kWallMap = {
    ".....#.....", ".....#.....", ".....#.....", ".....#.....",
    ".....#.....", ".....#.....", "...........",
};

}  // namespace

TEST_CASE("grid pathing: routes round a wall, never cutting its corner", "[scenario][scripted]") {
  const ArenaDef a = mapArena(kWallMap);
  const GridStep step =
      gridStepToward(a, grid::cellCentre(a, {2, 3}), kRadius, grid::cellCentre(a, {8, 3}));
  REQUIRE(step.reachable);
  // To the gap's west neighbour in 3, through the gap (it can be entered and
  // left only orthogonally: the wall's end closes the diagonals) and up.
  CHECK(step.turns == 8);
  // South: south-east is as short, and of several shortest routes the
  // first step is the lowest octant's.
  CHECK(step.octant == 6);

  // The corner rule on its own: a diagonal step between two walls touching
  // at a corner is closed, so the cell one diagonal away takes six turns.
  const ArenaDef corner = mapArena({"....", ".#..", "..#.", "...."});
  const GridStep round = gridStepToward(corner, grid::cellCentre(corner, {1, 2}), kRadius,
                                        grid::cellCentre(corner, {2, 1}));
  REQUIRE(round.reachable);
  CHECK(round.turns == 6);
}

TEST_CASE("grid pathing: a pit is closed like a wall", "[scenario][scripted]") {
  const ArenaDef a = mapArena({
      ".....~.....",
      ".....~.....",
      ".....~.....",
      ".....~.....",
      ".....~.....",
      ".....~.....",
      "...........",
  });
  const GridStep step =
      gridStepToward(a, grid::cellCentre(a, {2, 3}), kRadius, grid::cellCentre(a, {8, 3}));
  REQUIRE(step.reachable);
  CHECK(step.turns == 8);
  CHECK(step.octant == 6);
  // A goal in the pit cannot be reached at all.
  CHECK_FALSE(gridStepToward(a, grid::cellCentre(a, {2, 3}), kRadius, grid::cellCentre(a, {5, 3}))
                  .reachable);
}

TEST_CASE("grid pathing: the shorter of two routes", "[scenario][scripted]") {
  // A wall with a gap at either end; the route goes through the nearer one.
  const ArenaDef a = mapArena({
      "...........",
      ".....#.....",
      ".....#.....",
      ".....#.....",
      ".....#.....",
      ".....#.....",
      "...........",
  });
  const GridStep north =
      gridStepToward(a, grid::cellCentre(a, {2, 2}), kRadius, grid::cellCentre(a, {8, 2}));
  REQUIRE(north.reachable);
  CHECK(north.turns == 6);  // the south gap would be 10
  CHECK(north.octant == 1);
  const GridStep south =
      gridStepToward(a, grid::cellCentre(a, {2, 4}), kRadius, grid::cellCentre(a, {8, 4}));
  REQUIRE(south.reachable);
  CHECK(south.turns == 6);
  CHECK(south.octant == 7);
}

TEST_CASE("grid pathing: an unreachable goal, the goal's own cell, no cells",
          "[scenario][scripted]") {
  const ArenaDef a = mapArena({".....#.....", ".....#.....", ".....#....."});
  const Vec2 from = grid::cellCentre(a, {2, 1});
  CHECK_FALSE(gridStepToward(a, from, kRadius, grid::cellCentre(a, {8, 1})).reachable);
  const GridStep here = gridStepToward(a, from, kRadius, from);
  CHECK(here.reachable);
  CHECK(here.turns == 0);
  ArenaDef plain = a;
  plain.cellSize = Fixed::zero();  // a hand-placed arena: nothing to route
  CHECK_FALSE(gridStepToward(plain, from, kRadius, grid::cellCentre(a, {3, 1})).reachable);
}

TEST_CASE("grid pathing: the sim walks the route the search finds", "[scenario][scripted]") {
  // A monkey stepped by the route's first step each turn arrives in exactly
  // the route's turns: the search's rules are the grid's.
  Match m(mapArena({
              ".....#.....",
              ".....#.....",
              ".a...#...b.",
              ".....#.....",
              ".0...~..T1.",
              ".....#.....",
              "...........",
          }),
          0b01);
  const PlayerId p0{0};
  const Vec2 goal = m.at({8, 1});
  m.put(p0, {2, 3}, 0);
  const GridStep planned = gridStepToward(m.arena(), m.at({2, 3}), kRadius, goal);
  REQUIRE(planned.reachable);
  REQUIRE(planned.turns == 10);
  uint16_t turns = 0;
  for (; turns < 40; ++turns) {
    const Player& p = playerAt(m.s, p0);
    const GridStep next = gridStepToward(m.arena(), p.transform.pos, p.stats.radius, goal);
    REQUIRE(next.reachable);
    if (next.turns == 0) break;
    InputFrame f = frameFor(m.s);
    f.inputs[0] = PlayerInput{dirOf(next.octant), 255, 0, 0, 0};
    runTicks(m.s, f, 1, m.content, m.log);
  }
  CHECK(turns == planned.turns);
  CHECK(countEvents(m.log, EventKind::PlayerFell) == 0);
}

TEST_CASE("grid harvest steering: the roles and their goals, Rng-free", "[scenario][scripted]") {
  // Slots 0, 2, 4 are team 0 (home a, west), 1, 3, 5 team 1 (home b, east);
  // slot 2 and 3 raid, 4 and 5 defend, 0 and 1 gather.
  Match m(mapArena({
              "...............",
              ".0..#.....#..3.",
              "....#..T..#....",
              "aa..#.....#..bb",
              "aa1.#..~..#.4bb",
              "aa..#.....#..bb",
              "....#..T..#....",
              ".2..#.....#..5.",
              "...............",
          }),
          0b111111);
  const PlayerId gatherer{0};
  const PlayerId raider{2};
  const PlayerId defender{4};
  const PlayerId enemy{1};
  const Cell park{7, 0};
  m.parkAll(park);
  m.clearPickups();
  const std::vector<Cell> homeA = zoneCells(m.arena(), 0);
  const std::vector<Cell> homeB = zoneCells(m.arena(), 1);
  const auto toward = [&](PlayerId id, const std::vector<Cell>& goals) {
    return onShortestRoute(m, id, m.input(id), goals);
  };

  SECTION("room in the hand: the nearest fruit by the route, not by the crow") {
    m.put(gatherer, {3, 4}, 0);
    spawnPickup(m.s, m.at({5, 4}));  // two cells east, behind the wall
    spawnPickup(m.s, m.at({0, 8}));  // farther as the crow flies
    CHECK(turnsToAny(m.arena(), {3, 4}, {{0, 8}}) == 4);
    CHECK(turnsToAny(m.arena(), {3, 4}, {{5, 4}}) == 10);
    CHECK(toward(gatherer, {{0, 8}}));
  }
  SECTION("a full hand, or nothing left to fetch: home, round the wall, and it stays") {
    m.put(gatherer, {7, 3}, kHand);
    spawnPickup(m.s, m.at({8, 3}));
    CHECK(toward(gatherer, homeA));
    m.clearPickups();
    m.put(gatherer, {7, 3}, 1);
    CHECK(toward(gatherer, homeA));
    m.put(gatherer, {1, 4}, 1);
    CHECK(m.input(gatherer).moveMag == 0);
  }
  SECTION("empty-handed with nothing to fetch: waits on the nearest tree") {
    m.put(gatherer, {12, 0}, 0);  // the tree at (7, 2) is nearer than (7, 6)
    CHECK(toward(gatherer, {{7, 2}}));
    m.put(gatherer, {7, 2}, 0);
    CHECK(m.input(gatherer).moveMag == 0);
  }
  SECTION("the raider: the enemy home once it holds points, standing in it") {
    m.put(raider, {6, 0}, 0);
    spawnPickup(m.s, m.at({6, 1}));
    CHECK(toward(raider, {{6, 1}}));  // nothing to steal: it gathers
    m.s.rules.scores[enemy.value] = 3;
    CHECK(toward(raider, homeB));
    m.put(raider, {13, 4}, 1);
    CHECK(m.input(raider).moveMag == 0);
    m.put(raider, {13, 4}, kHand);  // a full hand goes home to bank
    CHECK(toward(raider, homeA));
  }
  SECTION("the defender: home while an enemy stands in it and the team has points") {
    m.put(defender, {6, 0}, 0);
    spawnPickup(m.s, m.at({6, 1}));
    m.put(enemy, {1, 4}, 0);
    CHECK(toward(defender, {{6, 1}}));  // nothing to lose yet
    m.s.rules.scores[gatherer.value] = 2;
    CHECK(toward(defender, homeA));
    m.put(enemy, park, 0);
    CHECK(toward(defender, {{6, 1}}));
  }
  SECTION("nothing reachable: the stick idle") {
    // Walled into the north-east corner cell.
    ContentDB db = m.db;
    ArenaDef& a = db.arenas[kMap];
    const Vec2 half{Fixed::half(), Fixed::half()};
    a.walls[a.wallCount++] = Wall{m.at({13, 1}) - half, m.at({13, 0}) + half};
    a.walls[a.wallCount++] = Wall{m.at({14, 1}) - half, m.at({14, 1}) + half};
    const MatchContent boxed = matchContent(db, m.config);
    spawnPickup(m.s, m.at({6, 1}));
    m.put(gatherer, {14, 0}, 0);
    Rng rng = Rng::seeded(7);
    CHECK(scriptedInput(rng, m.s, boxed, gatherer).moveMag == 0);
    CHECK(m.input(gatherer).moveMag == 255);  // the same world unboxed: it goes
  }
  SECTION("no Rng draw: the same draws as team deathmatch on the same world") {
    const MatchContent tdm = matchContent(m.db, MatchConfig{.seed = m.config.seed,
                                                            .players = m.config.players,
                                                            .arena = kMap,
                                                            .mode = kTdm,
                                                            .teams = m.config.teams});
    m.s.rules.scores[enemy.value] = 3;
    m.put(gatherer, {3, 4}, 0);
    m.put(raider, {6, 0}, 1);
    m.put(defender, {13, 4}, 2);
    spawnPickup(m.s, m.at({5, 4}));
    for (uint8_t i = 0; i < 6; ++i) {
      Rng a = Rng::seeded(99U + i);
      Rng b = Rng::seeded(99U + i);
      (void)scriptedInput(a, m.s, m.content, PlayerId{i});
      (void)scriptedInput(b, m.s, tdm, PlayerId{i});
      CHECK(a.s == b.s);
    }
  }
}

TEST_CASE("grid harvest steering: a scripted match gathers, banks and steals",
          "[scenario][scripted][harvest]") {
  // Walls between the homes and the trees, gaps only on the outer rows: a
  // straight walk from a home stands at the wall for ever.
  Match m(mapArena({
              "...............",
              ".0..#.....#..3.",
              "....#..T..#....",
              "aa..#.....#..bb",
              "aa1.#..~..#.4bb",
              "aa..#.....#..bb",
              "....#..T..#....",
              ".2..#.....#..5.",
              "...............",
          }),
          0b111111);
  uint32_t opening = 0;
  m.s.players.forEachAlive([&](EntityId /*id*/, const Player& p) { opening += p.ammo; });
  Rng rng = Rng::seeded(0x5C12);
  for (int i = 0; i < 1800; ++i)
    runTicks(m.s, scriptedFrame(rng, m.s, m.content), 1, m.content, m.log);

  int banked = 0;
  for (const SimEvent& e : m.log) {
    if (e.kind == EventKind::ScoreChanged && e.b == static_cast<uint32_t>(ScoreReason::Objective))
      ++banked;
  }
  INFO("banked " << banked << ", opening hands " << opening << ", fruit taken "
                 << countEvents(m.log, EventKind::PickupTaken) << ", stolen "
                 << countEvents(m.log, EventKind::BananaStolen));
  CHECK(countEvents(m.log, EventKind::PickupTaken) > 0);
  CHECK(std::cmp_greater(banked, opening));
  CHECK(countEvents(m.log, EventKind::BananaStolen) > 0);
}
