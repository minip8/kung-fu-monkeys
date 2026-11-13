// Trees (architecture §11 "Contest design: bananas as the economy", v0.82): a
// pickup spawner with PickupSpawner::tree lays one banana pickup on its cell
// on the spawner's beat (firstTicks, then every intervalTicks) while fewer
// than maxAlive bananas lie on that cell, whoever dropped them. No RNG draw;
// trees bear in every mode and whatever MatchConfig::powerUps says; a full
// pickup pool stacks the fruit as it stacks a drop (rules/bananas.hpp); a
// tree counts in bananaLeft, so a round kind on its arena never runs dry.
// Hand-built content: the default dojo given 1-unit cells.
#include "kfm/sim/spaces/grid_cells.hpp"
#include "loadout_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint16_t kFirst = 30;
constexpr uint16_t kInterval = 60;

[[nodiscard]] constexpr Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}
[[nodiscard]] constexpr Vec2 half(int32_t x2, int32_t y2) {  // (x2 / 2, y2 / 2)
  return Vec2{Fixed::fromRatio(x2, 2), Fixed::fromRatio(y2, 2)};
}

// The tree at (0.5, 0.5), the centre of cell (16, 8) of the dojo with 1-unit
// cells.
constexpr Vec2 kTree = half(1, 1);

[[nodiscard]] PickupSpawner tree(Vec2 pos, uint8_t maxAlive, uint16_t first = kFirst,
                                 uint16_t interval = kInterval) {
  PickupSpawner sp;
  sp.pos = pos;
  sp.firstTicks = first;
  sp.intervalTicks = interval;
  sp.maxAlive = maxAlive;
  sp.tree = 1;
  return sp;
}

// The default content with arena 0 given 1-unit cells and `spawners`, and a
// power-up ("more", an ammo grant) for the power-up spawners.
[[nodiscard]] ContentDB treeContent(std::initializer_list<PickupSpawner> spawners) {
  ContentDB c = defaultContent();
  addPowerUp(c, "more", 600, {statGrant(StatKind::AmmoMax, Fixed::one(), Fixed::fromInt(2))});
  ArenaDef& a = c.arenas[0];
  a.cellSize = Fixed::one();
  a.pickupSpawnerCount = 0;
  for (const PickupSpawner& sp : spawners)
    a.pickupSpawners[a.pickupSpawnerCount++] = sp;
  return c;
}

[[nodiscard]] PickupSpawner powerUpSpawner(Vec2 pos, uint8_t maxAlive) {
  PickupSpawner sp;
  sp.pos = pos;
  sp.firstTicks = kFirst;
  sp.intervalTicks = kInterval;
  sp.maxAlive = maxAlive;
  return sp;
}

// The PickupSpawned events of banana pickups laid at `pos`.
[[nodiscard]] std::vector<SimEvent> laidAt(const WorldState& s, const EventLog& log, Vec2 pos) {
  std::vector<SimEvent> out;
  for (const SimEvent& e : log) {
    if (e.kind != EventKind::PickupSpawned || e.a != static_cast<uint32_t>(PickupKind::Banana)) {
      continue;
    }
    const Pickup* k = s.pickups.get(e.entity);
    if (k != nullptr && k->pos == pos) out.push_back(e);
  }
  return out;
}

[[nodiscard]] uint32_t bananasOnFloor(const WorldState& s) {
  uint32_t n = 0;
  s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& k) {
    if (k.kind == PickupKind::Banana) n += k.count;
  });
  return n;
}

[[nodiscard]] std::vector<Tick> powerUpTicks(const EventLog& log) {
  std::vector<Tick> out;
  for (const SimEvent& e : log) {
    if (e.kind == EventKind::PickupSpawned && e.a == static_cast<uint32_t>(PickupKind::PowerUp)) {
      out.push_back(e.tick);
    }
  }
  return out;
}

void emptyHands(WorldState& s) {
  s.players.forEachAlive([](EntityId /*id*/, Player& p) { p.ammo = 0; });
}

}  // namespace

TEST_CASE("trees: a tree lays a banana on the spawner's beat while fewer than max_alive lie",
          "[scenario][tree]") {
  // A power-up spawner with the same beat and room for three shows when the
  // beats fall; the tree holds two.
  const ContentDB c = treeContent({powerUpSpawner(at(-8, 4), 3), tree(kTree, 2)});
  WorldState s = makePlayingWorld(7, 0b11, c);
  EventLog log;
  idle(s, kFirst + (2 * kInterval) + 1, c, log);
  const std::vector<Tick> beats = powerUpTicks(log);
  REQUIRE(beats.size() == 3);
  CHECK(beats[1].value - beats[0].value == kInterval);
  // The first two beats lay a banana each on the tree's cell, at its position.
  const std::vector<SimEvent> laid = laidAt(s, log, kTree);
  REQUIRE(laid.size() == 2);
  CHECK(laid[0].tick == beats[0]);
  CHECK(laid[1].tick == beats[1]);
  CHECK_FALSE(laid[0].player.isValid());
  CHECK(bananasOnFloor(s) == 2);  // the third beat skipped: two lie there

  SECTION("taking one lets the next beat lay again") {
    s.pickups.despawn(laid[0].entity);
    log.clear();
    idle(s, kInterval, c, log);
    CHECK(laidAt(s, log, kTree).size() == 1);
    CHECK(bananasOnFloor(s) == 2);
  }
  SECTION("a banana anyone dropped on the cell counts, one on the next cell does not") {
    s.pickups.despawn(laid[0].entity);
    s.pickups.despawn(laid[1].entity);
    spawnPickup(s, half(19, 1));                                             // (9.5, 0.5): far away
    spawnPickup(s, Vec2{Fixed::fromRatio(9, 10), Fixed::fromRatio(1, 10)});  // cell (16, 8)
    spawnPickup(s, half(3, 1));  // (1.5, 0.5): cell (17, 8)
    REQUIRE(grid::cellOf(c.arenas[0], half(3, 1)) != grid::cellOf(c.arenas[0], kTree));
    log.clear();
    idle(s, kInterval, c, log);
    CHECK(laidAt(s, log, kTree).size() == 1);  // one dropped there + one laid = max_alive
    log.clear();
    idle(s, kInterval, c, log);
    CHECK(laidAt(s, log, kTree).empty());
  }
  SECTION("a stack counts as its bananas") {
    s.pickups.despawn(laid[1].entity);
    Pickup* k = s.pickups.get(laid[0].entity);
    REQUIRE(k != nullptr);
    k->count = 2;
    log.clear();
    idle(s, kInterval, c, log);
    CHECK(laidAt(s, log, kTree).empty());
  }
}

TEST_CASE("trees: no RNG draw, so mirrored trees stay mirrored", "[scenario][tree]") {
  const Vec2 mirror = half(-1, -1);
  const ContentDB c = treeContent({tree(kTree, 1), tree(mirror, 1)});
  const ContentDB bare = treeContent({});
  WorldState s = makePlayingWorld(9, 0b11, c);
  WorldState t = makePlayingWorld(9, 0b11, bare);
  REQUIRE(s.rng.s == t.rng.s);
  EventLog log;
  EventLog bareLog;
  idle(s, kFirst + kInterval + 1, c, log);
  idle(t, kFirst + kInterval + 1, bare, bareLog);
  CHECK(s.rng.s == t.rng.s);  // the trees drew nothing
  const std::vector<SimEvent> east = laidAt(s, log, kTree);
  const std::vector<SimEvent> west = laidAt(s, log, mirror);
  REQUIRE(east.size() == 1);
  REQUIRE(west.size() == 1);
  CHECK(east[0].tick == west[0].tick);
}

TEST_CASE("trees: they bear in every mode, whatever the match's power-up set", "[scenario][tree]") {
  ContentDB c = treeContent({powerUpSpawner(at(-8, 4), 1), tree(kTree, 1)});
  ModeDef& dm = c.modes[1];
  dm = c.modes[0];
  dm.name = ContentName::of("dm");
  dm.kind = ModeKind::Deathmatch;
  dm.timeLimitTicks = 6000;
  c.modeCount = 2;
  for (const uint8_t mode : {uint8_t{0}, uint8_t{1}}) {
    INFO("mode " << int{mode});
    const MatchContent match =
        matchContent(c, MatchConfig{.seed = 11, .players = 0b11, .mode = mode, .powerUps = 0});
    WorldState s = playingWorld(11, 0b11, match);
    EventLog log;
    idle(s, kFirst + 1, match, log);
    CHECK(powerUpTicks(log).empty());  // the power-up spawner is off
    CHECK(laidAt(s, log, kTree).size() == 1);
  }
}

TEST_CASE("trees: a full pickup pool stacks the fruit, never losing it", "[scenario][tree]") {
  // A power-up spawner first in the list meets the full pool and skips its
  // beat; the tree after it still bears.
  const ContentDB c = treeContent({powerUpSpawner(at(-8, 4), 1), tree(kTree, 3)});
  WorldState s = makePlayingWorld(13, 0b11, c);
  // Fill the pool with bananas away from the tree's cell; (3, 3) is nearest.
  EntityId nearest{};
  for (uint32_t i = 0; s.pickups.size() < kMaxPickups; ++i) {
    const Vec2 pos = i == 0 ? at(3, 3) : at(-14 + static_cast<int32_t>(i % 28), -7);
    const EntityId id = spawnPickup(s, pos);
    if (i == 0) nearest = id;
  }
  const uint32_t before = bananasOnFloor(s);
  EventLog log;
  idle(s, kFirst + 1, c, log);
  CHECK(countEvents(log, EventKind::PickupSpawned) == 0);
  CHECK(bananasOnFloor(s) == before + 1);
  const Pickup* k = s.pickups.get(nearest);
  REQUIRE(k != nullptr);
  CHECK(k->count == 2);
}

TEST_CASE("trees: a round kind on an arena with a tree never runs dry", "[scenario][tree][dry]") {
  SECTION("a tree with a beat to come keeps an empty round going") {
    const ContentDB c = treeContent({tree(kTree, 1, 6000, 600)});
    WorldState s = makePlayingWorld(15, 0b11, c);
    emptyHands(s);
    EventLog log;
    idle(s, 120, c, log);
    CHECK(countEvents(log, EventKind::RoundEnded) == 0);
    CHECK(s.rules.dryTicks == 0);
  }
  SECTION("a tree that bears once and is spent no longer counts") {
    const ContentDB c = treeContent({tree(kTree, 1, 0, 0)});
    WorldState s = makePlayingWorld(15, 0b11, c);
    EventLog log;
    idle(s, 2, c, log);
    REQUIRE(s.rules.phase == MatchPhase::Playing);
    emptyHands(s);
    s.pickups.forEachAlive([&](EntityId id, const Pickup& /*k*/) { s.pickups.despawn(id); });
    idle(s, 1, c, log);
    CHECK(countEvents(log, EventKind::RoundEnded) == 1);
  }
}
