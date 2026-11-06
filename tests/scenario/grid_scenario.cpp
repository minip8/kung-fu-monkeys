// The grid space end to end (architecture §5.4, §5.12, v0.34): commitments
// at the latch, the slide and its exact landing, walls and corners,
// contested cells, the dash as a commitment, the rooted throw, and the
// catch's perfect window of 0 or of the whole active phase.
#include "kfm/sim/spaces/grid_cells.hpp"
#include "kfm/sim/systems.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;
using grid::Cell;

namespace {

constexpr uint8_t kCells = 1;  // 16 x 10 cells of one unit, a wall cell and a pit cell
constexpr uint8_t kGrid = 1;   // rounds in the grid space, 4-tick turns, 2-cell dashes
constexpr uint8_t kTurn = 4;
constexpr Cell kWall{8, 4};
constexpr uint8_t kThrow = buttonBit(Button::Throw);
constexpr uint8_t kDash = buttonBit(Button::Dash);
constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};
constexpr PlayerId kP2{2};

// moveDir of octant k (0 east, counter-clockwise).
constexpr uint8_t dirOf(uint8_t octant) {
  return static_cast<uint8_t>(octant * 32);
}
constexpr PlayerInput walk(uint8_t octant, uint8_t buttons = 0, uint8_t subTick = 0) {
  return PlayerInput{dirOf(octant), 255, dirOf(octant), buttons, subTick};
}

[[nodiscard]] Vec2 centreOf(const ArenaDef& a, Cell k) {
  return grid::cellCentre(a, k);
}

// defaultContent() with monkeys of radius 0.35, the cells arena and the
// grid mode; the throw roots its recovery, which lasts a turn. The dash's
// perfect window is the argument (0: never perfect; 12: the whole active
// phase, the catcher's).
ContentDB gridContent(uint16_t perfectWindow = 0) {
  ContentDB c = defaultContent();
  removeThrowCharge(c);  // v0.48: like content/'s grid_throw, a press throws
  c.player.radius = Fixed::fromRatio(35, 100);
  ArenaDef& a = c.arenas[kCells];
  a = ArenaDef{};
  a.name = ContentName::of("cells");
  a.cellSize = Fixed::one();
  a.halfExtents = Vec2{Fixed::fromInt(8), Fixed::fromInt(5)};
  const Vec2 wallMin = centreOf(a, kWall) - Vec2{Fixed::half(), Fixed::half()};
  a.walls[0] = Wall{wallMin, wallMin + Vec2{Fixed::one(), Fixed::one()}};
  a.wallCount = 1;
  const Vec2 pitMin = centreOf(a, Cell{8, 8}) - Vec2{Fixed::half(), Fixed::half()};
  a.hazards[0] =
      Hazard{pitMin, pitMin + Vec2{Fixed::one(), Fixed::one()}, HazardKind::Pit, Fixed::one()};
  a.hazardCount = 1;
  a.spawnPoints[0] = centreOf(a, Cell{2, 2});
  a.spawnPoints[1] = centreOf(a, Cell{13, 2});
  a.spawnPoints[2] = centreOf(a, Cell{2, 7});
  a.spawnPoints[3] = centreOf(a, Cell{13, 7});
  a.spawnPointCount = 4;
  c.arenaCount = 2;
  ModeDef& m = c.modes[kGrid];
  m = c.modes[0];
  m.name = ContentName::of("grid");
  m.space = Space::Grid;
  m.turnTicks = kTurn;
  m.dashCells = 2;
  m.roundStartTicks = 1;
  m.roundsToWin = 9;
  c.modeCount = 2;
  AbilityDef& dash = c.abilities[c.player.abilities[kAbilitySlotDash]];
  dash.perfectWindowTicks = perfectWindow;
  AbilityDef& thr = c.abilities[c.player.abilities[kAbilitySlotThrow]];
  thr.recoveryTicks = kTurn;
  thr.rootPhases = phaseBit(AbilityPhase::Recovery);
  REQUIRE(c.compatible(kCells, kGrid));
  REQUIRE_FALSE(c.compatible(0, kGrid));  // the dojo has no cells
  return c;
}

struct Lab {
  ContentDB db;
  MatchConfig config;
  MatchContent m;
  WorldState s;
  EventLog log;

  explicit Lab(PlayerMask players, uint16_t perfectWindow = 0)
      : db(gridContent(perfectWindow)),
        config{.seed = 0x6121D, .players = players, .arena = kCells, .mode = kGrid},
        m(matchContent(db, config)),
        s(initialWorld(config)) {
    for (int i = 0; i < 100 && s.rules.phase != MatchPhase::Playing; ++i) {
      runTicks(s, frameFor(s), 1, m, log);
    }
    REQUIRE(s.rules.phase == MatchPhase::Playing);
    toLatch();
  }
  [[nodiscard]] const ArenaDef& arena() const { return m.arena; }
  // Idle ticks until the next tick is a latch.
  void toLatch() {
    while (s.rules.ticksInPhase % kTurn != 0)
      runTicks(s, frameFor(s), 1, m, log);
  }
  // Puts a monkey at rest on a cell centre.
  void put(PlayerId id, Cell k) {
    Player& p = placePlayer(s, id, centreOf(arena(), k));
    p.moveTicks = 0;
    p.moveTarget = Vec2::zero();
  }
  [[nodiscard]] Cell cellOf(PlayerId id) const {
    return grid::cellOf(arena(), playerAt(s, id).transform.pos);
  }
  [[nodiscard]] Vec2 pos(PlayerId id) const { return playerAt(s, id).transform.pos; }
  // One tick; slot inputs in, everyone else idle.
  void tick(PlayerInput p0 = {}, PlayerInput p1 = {}, PlayerInput p2 = {}) {
    InputFrame f = frameFor(s);
    f.inputs[0] = p0;
    f.inputs[1] = p1;
    f.inputs[2] = p2;
    runTicks(s, f, 1, m, log);
  }
  // A whole turn holding the same inputs (a held button presses once).
  void turn(PlayerInput p0 = {}, PlayerInput p1 = {}, PlayerInput p2 = {}) {
    REQUIRE(s.rules.ticksInPhase % kTurn == 0);
    for (uint8_t k = 0; k < kTurn; ++k)
      tick(p0, p1, p2);
  }
};

}  // namespace

TEST_CASE("grid: a commitment lands exactly on its cell, turn after turn", "[scenario][grid]") {
  Lab lab(0b11);
  lab.put(kP0, Cell{1, 1});
  lab.put(kP1, Cell{14, 8});
  // Eleven turns east along row 1, then six turns south-east and back: no
  // drift, every turn ends on a centre.
  for (int k = 1; k <= 11; ++k) {
    // Mid-turn the monkey is between the cells.
    lab.tick(walk(0));
    CHECK(playerAt(lab.s, kP0).moveTicks == kTurn - 1);
    CHECK(playerAt(lab.s, kP0).moveTarget == centreOf(lab.arena(), Cell{1 + k, 1}));
    for (uint8_t t = 1; t < kTurn; ++t)
      lab.tick(walk(0));
    CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{1 + k, 1}));
    CHECK(playerAt(lab.s, kP0).moveTicks == 0);
  }
  for (int k = 1; k <= 3; ++k) {
    lab.turn(walk(7));  // south-east
    CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{12 + k, 1 + k}));
  }
  for (int k = 1; k <= 3; ++k) {
    lab.turn(walk(3));  // north-west
    CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{15 - k, 4 - k}));
  }
  // The other monkey stood still all along.
  CHECK(lab.pos(kP1) == centreOf(lab.arena(), Cell{14, 8}));
}

TEST_CASE("grid: an idle monkey stays, a stick between latches waits", "[scenario][grid]") {
  Lab lab(0b11);
  lab.put(kP0, Cell{3, 3});
  for (int k = 0; k < 5; ++k)
    lab.turn();
  CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{3, 3}));
  CHECK(playerAt(lab.s, kP0).moveTicks == 0);
  // The stick pushed after the latch does nothing until the next one.
  lab.tick();
  for (uint8_t t = 1; t < kTurn; ++t)
    lab.tick(walk(0));
  CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{3, 3}));
  lab.turn(walk(0));
  CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{4, 3}));
}

TEST_CASE("grid: walls, corners and the outer box block a commitment", "[scenario][grid]") {
  Lab lab(0b11);
  lab.put(kP1, Cell{14, 8});
  // Into the wall cell from the west.
  lab.put(kP0, Cell{7, 4});
  lab.turn(walk(0));
  CHECK(lab.cellOf(kP0) == Cell{7, 4});
  CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{7, 4}));
  // Diagonally past the wall's corner: north-east of (7, 4) is (8, 3),
  // free, but (8, 4) beside the path is the wall.
  lab.turn(walk(1));
  CHECK(lab.cellOf(kP0) == Cell{7, 4});
  // South-east of (7, 3) is the wall itself.
  lab.put(kP0, Cell{7, 3});
  lab.turn(walk(7));
  CHECK(lab.cellOf(kP0) == Cell{7, 3});
  // The same corner from a cell with nothing beside the path goes.
  lab.put(kP0, Cell{6, 3});
  lab.turn(walk(7));
  CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{7, 4}));
  // The outer box: west of column 0 and north of row 0.
  lab.put(kP0, Cell{0, 0});
  lab.turn(walk(4));
  lab.turn(walk(2));
  lab.turn(walk(3));
  CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{0, 0}));
}

TEST_CASE("grid: a pit cell may be entered, and the monkey falls", "[scenario][grid]") {
  Lab lab(0b111);
  lab.put(kP0, Cell{7, 8});
  lab.put(kP1, Cell{14, 1});
  lab.put(kP2, Cell{1, 1});
  const size_t before = lab.log.size();
  lab.turn(walk(0));
  bool fell = false;
  for (size_t i = before; i < lab.log.size(); ++i) {
    fell = fell || (lab.log[i].kind == EventKind::PlayerEliminated && lab.log[i].player == kP0);
  }
  CHECK(fell);
}

TEST_CASE("grid: a contested cell goes to the earlier press, then the lower slot",
          "[scenario][grid]") {
  SECTION("the earlier press wins") {
    Lab lab(0b11);
    lab.put(kP0, Cell{2, 2});
    lab.put(kP1, Cell{4, 2});
    lab.turn(walk(0, 0, 200), walk(4, 0, 10));  // both into (3, 2); slot 1 pressed first
    CHECK(lab.pos(kP1) == centreOf(lab.arena(), Cell{3, 2}));
    CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{2, 2}));
  }
  SECTION("an equal press goes to the lower slot") {
    Lab lab(0b111);
    lab.put(kP0, Cell{2, 2});
    lab.put(kP1, Cell{4, 2});
    lab.put(kP2, Cell{3, 1});
    lab.turn(walk(0, 0, 7), walk(4, 0, 7), walk(6, 0, 7));  // three into (3, 2)
    CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{3, 2}));
    CHECK(lab.pos(kP1) == centreOf(lab.arena(), Cell{4, 2}));
    CHECK(lab.pos(kP2) == centreOf(lab.arena(), Cell{3, 1}));
  }
  SECTION("a swap is blocked: the loser stays, the winner moves in") {
    Lab lab(0b11);
    lab.put(kP0, Cell{5, 5});
    lab.put(kP1, Cell{6, 5});
    lab.turn(walk(0), walk(4));
    CHECK(lab.pos(kP1) == centreOf(lab.arena(), Cell{6, 5}));
    CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{6, 5}));
  }
  SECTION("following into a cell being vacated is no contest") {
    Lab lab(0b11);
    lab.put(kP0, Cell{5, 5});
    lab.put(kP1, Cell{6, 5});
    lab.turn(walk(0), walk(0));
    CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{6, 5}));
    CHECK(lab.pos(kP1) == centreOf(lab.arena(), Cell{7, 5}));
  }
}

TEST_CASE("grid: a dash commits to dashCells cells and a wall stops it on the last free one",
          "[scenario][grid]") {
  Lab lab(0b11);
  lab.put(kP1, Cell{14, 8});
  const AbilityDef& dash = lab.db.abilityAt(lab.db.player.abilities[kAbilitySlotDash]);
  SECTION("two free cells") {
    lab.put(kP0, Cell{2, 6});
    lab.tick(walk(0, kDash));
    CHECK(playerAt(lab.s, kP0).tags.has(Tag::Dashing));
    CHECK(playerAt(lab.s, kP0).moveTarget == centreOf(lab.arena(), Cell{4, 6}));
    for (uint16_t t = 1; t < dash.activeTicks; ++t)
      lab.tick(walk(0));
    CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{4, 6}));
    CHECK(playerAt(lab.s, kP0).moveTicks == 0);
  }
  SECTION("the wall two cells ahead") {
    lab.put(kP0, Cell{6, 4});
    lab.tick(walk(0, kDash));
    for (uint16_t t = 1; t < dash.activeTicks; ++t)
      lab.tick(walk(0));
    CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{7, 4}));
  }
  SECTION("mid-turn, from the cell holding the monkey") {
    lab.put(kP0, Cell{2, 6});
    lab.tick(walk(2));  // the latch: north to (2, 5)
    lab.tick(walk(2));
    lab.tick(walk(2));         // three quarters of the way: in (2, 5)
    lab.tick(walk(2, kDash));  // dash north from (2, 5)
    for (uint16_t t = 1; t < dash.activeTicks; ++t)
      lab.tick(walk(2));
    CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{2, 3}));
  }
}

TEST_CASE("grid: a rooted throw costs the next latch while the slide in progress finishes",
          "[scenario][grid]") {
  Lab lab(0b11);
  lab.put(kP0, Cell{2, 2});
  lab.put(kP1, Cell{14, 8});
  REQUIRE(playerAt(lab.s, kP0).ammo > 0);
  const size_t before = lab.log.size();
  lab.turn(walk(0, kThrow));  // walk east and throw on the same latch
  CHECK(countEvents(std::vector<SimEvent>(lab.log.begin() + static_cast<std::ptrdiff_t>(before),
                                          lab.log.end()),
                    EventKind::BoomerangThrown) == 1);
  CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{3, 2}));
  REQUIRE(rooted(playerAt(lab.s, kP0), lab.m));
  lab.turn(walk(0));  // the next latch is rooted: no commitment
  CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{3, 2}));
  lab.turn(walk(0));  // free again
  CHECK(lab.pos(kP0) == centreOf(lab.arena(), Cell{4, 2}));
}

TEST_CASE("grid: a dash with a 0-tick window is never PerfectDash and never catches",
          "[scenario][grid][catch]") {
  for (const uint16_t window : {uint16_t{0}, uint16_t{12}}) {
    INFO("perfect window " << window);
    Lab lab(0b11, window);
    const AbilityDef& dash = lab.db.abilityAt(lab.db.player.abilities[kAbilitySlotDash]);
    REQUIRE(dash.perfectWindowTicks == window);
    lab.put(kP0, Cell{4, 6});
    lab.put(kP1, Cell{13, 6});
    Player& p0 = playerAt(lab.s, kP0);
    p0.ammo = 0;  // room to catch
    p0.hp = 2;
    // A banana of slot 1 coming west along row 6, met by an eastward dash.
    (void)spawnBanana(lab.s, kP1, centreOf(lab.arena(), Cell{9, 6}),
                      Vec2{-lab.db.boomerang.speed, Fixed::zero()}, BoomerangPhase::Outbound,
                      lab.db);
    const size_t before = lab.log.size();
    bool perfect = false;
    lab.tick(walk(0, kDash, 128));  // a press with a sub-tick: the tail tick case
    perfect = perfect || playerAt(lab.s, kP0).tags.has(Tag::PerfectDash);
    for (uint16_t t = 1; t < dash.activeTicks + 2; ++t) {
      lab.tick(walk(0));
      if (lab.s.playerBySlot(kP0) == nullptr) break;
      perfect = perfect || playerAt(lab.s, kP0).tags.has(Tag::PerfectDash);
    }
    int caught = 0;
    int hit = 0;
    for (size_t i = before; i < lab.log.size(); ++i) {
      if (lab.log[i].kind == EventKind::BoomerangCaught && lab.log[i].player == kP0) ++caught;
      if (lab.log[i].kind == EventKind::PlayerHit && lab.log[i].player == kP0) ++hit;
    }
    if (window == 0) {
      CHECK_FALSE(perfect);
      CHECK(caught == 0);
      CHECK(hit == 1);
    } else {
      CHECK(perfect);
      CHECK(caught == 1);  // the catcher's dash: the window spans the active phase
      CHECK(hit == 0);
    }
  }
}

TEST_CASE("grid: facing is latched once per turn", "[scenario][grid]") {
  Lab lab(0b11);
  lab.put(kP0, Cell{5, 5});
  lab.tick(PlayerInput{0, 0, 64, 0, 0});  // the latch: aim north
  CHECK(playerAt(lab.s, kP0).transform.facing == Angle::fromInput8(64));
  lab.tick(PlayerInput{0, 0, 128, 0, 0});  // mid-turn aim west: facing waits
  CHECK(playerAt(lab.s, kP0).transform.facing == Angle::fromInput8(64));
  for (uint8_t t = 2; t < kTurn; ++t)
    lab.tick(PlayerInput{0, 0, 128, 0, 0});
  lab.tick(PlayerInput{0, 0, 128, 0, 0});  // the next latch
  CHECK(playerAt(lab.s, kP0).transform.facing == Angle::fromInput8(128));
}
