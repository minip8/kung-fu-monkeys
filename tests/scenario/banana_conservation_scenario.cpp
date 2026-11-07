// Bananas never disappear (architecture §5.6, v0.57, rules/bananas.hpp):
// a monkey killed by anything but a pit drops every banana it holds as
// floor pickups scattered around it (inside the arena, off the walls); a
// catch over ammoMax drops the extra banana at the catch point; ammo above a
// lowered ammoMax (a power-up running out) vanishes with it (round 6); a full pickup pool stacks
// a new banana on the nearest banana pickup; walking over a pickup at full
// ammo leaves it; an ammo power-up's top-up never makes phantom bananas.
// (A pit fall keeps its held bananas, and a banana over a pit sinks:
// pit_scenario.cpp. A dash attack's spent banana: dash_attack_scenario.cpp.)
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

const PlayerId kP0{0};
const PlayerId kP1{1};
const PlayerId kP2{2};

Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

// The default content with an ammo power-up (ammo_max + 2); its index.
uint8_t addAmmoPowerUp(ContentDB& c, uint16_t duration = 600) {
  return addPowerUp(c, "more", duration,
                    {statGrant(StatKind::AmmoMax, Fixed::one(), Fixed::fromInt(2))});
}

// Kills `victim` with a banana of `by` hitting it on the next tick.
EventLog hitAndStep(WorldState& s, const MatchContent& m, PlayerId victim, PlayerId by) {
  const Vec2 v = playerAt(s, victim).transform.pos;
  spawnBanana(s, by, v - Vec2{Fixed::one(), Fixed{}}, Vec2{Fixed::fromRatio(1, 4), Fixed{}},
              BoomerangPhase::Outbound, m.db);
  EventLog log;
  runTicks(s, frameFor(s), 1, m, log);
  return log;
}

std::vector<Vec2> bananaPickups(const WorldState& s) {
  std::vector<Vec2> out;
  s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& k) {
    if (k.kind != PickupKind::Banana) return;
    for (uint8_t i = 0; i < k.count; ++i)
      out.push_back(k.pos);
  });
  return out;
}

}  // namespace

TEST_CASE("a hit monkey's held bananas scatter around it as pickups", "[scenario][banana]") {
  ContentDB c = defaultContent();
  const uint8_t more = addAmmoPowerUp(c);
  const MatchContent m = matchContent(c, MatchConfig{.seed = 61, .players = 0b111});
  WorldState s = makePlayingWorld(61, 0b111, c);
  const Vec2 died = at(2, 1);
  Player& p0 = placePlayer(s, kP0, died);
  givePowerUp(p0, more, 600);
  EventLog warm;
  runTicks(s, frameFor(s), 1, m, warm);  // the stats see the power-up: ammoMax 3
  playerAt(s, kP0).ammo = 3;
  placePlayer(s, kP1, at(-10, 6));
  placePlayer(s, kP2, at(10, -6));
  const EventLog log = hitAndStep(s, m, kP0, kP1);
  REQUIRE(firstEvent(log, EventKind::PlayerEliminated).player == kP0);
  CHECK(countEvents(log, EventKind::BananaDropped) == 3);
  const std::vector<Vec2> lying = bananaPickups(s);
  REQUIRE(lying.size() == 3);
  for (size_t i = 0; i < lying.size(); ++i) {
    // On a ring around where it died, distinct, inside the arena.
    CHECK(abs(distance(lying[i], died) - Fixed::fromRatio(3, 4)) <= Fixed::fromRatio(1, 50));
    CHECK(abs(lying[i].x) <= c.arenas[0].halfExtents.x);
    CHECK(abs(lying[i].y) <= c.arenas[0].halfExtents.y);
    for (size_t j = i + 1; j < lying.size(); ++j)
      CHECK(distance(lying[i], lying[j]) > Fixed::one());
  }
  // A single held banana lies right where the monkey died.
  const Player& p2 = playerAt(s, kP2);
  REQUIRE(p2.ammo == 1);
  const Vec2 p2At = p2.transform.pos;
  const EventLog second = hitAndStep(s, m, kP2, kP1);
  REQUIRE(firstEvent(second, EventKind::PlayerEliminated).player == kP2);
  const Pickup* k = s.pickups.get(firstEvent(second, EventKind::BananaDropped).entity);
  REQUIRE(k != nullptr);
  CHECK(k->pos == p2At);
}

TEST_CASE("held bananas never scatter into a wall or out of the arena", "[scenario][banana]") {
  ContentDB c = defaultContent();
  const uint8_t more = addAmmoPowerUp(c);
  ArenaDef& a = c.arenas[0];
  a.walls[0] = Wall{at(3, -2), at(5, 2)};
  a.wallCount = 1;
  const MatchContent m = matchContent(c, MatchConfig{.seed = 62, .players = 0b11});
  WorldState s = makePlayingWorld(62, 0b11, c);
  // Against the wall's west face and near the arena's south edge.
  const Vec2 died{Fixed::fromRatio(5, 2), -a.halfExtents.y + Fixed::fromRatio(3, 5)};
  placePlayer(s, kP0, Vec2{Fixed::fromRatio(5, 2), Fixed::zero()});
  givePowerUp(playerAt(s, kP0), more, 600);
  EventLog warm;
  runTicks(s, frameFor(s), 1, m, warm);
  playerAt(s, kP0).ammo = 3;
  placePlayer(s, kP1, at(-10, 6));
  SECTION("beside a wall") {
    hitAndStep(s, m, kP0, kP1);
  }
  SECTION("at the arena's edge") {
    placePlayer(s, kP0, died);
    hitAndStep(s, m, kP0, kP1);
  }
  const std::vector<Vec2> lying = bananaPickups(s);
  REQUIRE(lying.size() == 3);
  for (const Vec2& p : lying) {
    CHECK_FALSE(circleOverlapsBox(a.walls[0].min, a.walls[0].max, p, c.pickup.radius));
    CHECK(abs(p.x) <= a.halfExtents.x - c.pickup.radius);
    CHECK(abs(p.y) <= a.halfExtents.y - c.pickup.radius);
  }
}

TEST_CASE("a catch over ammoMax drops the extra banana at the catch point", "[scenario][banana]") {
  const ContentDB& c = defaultContent();
  WorldState s = makePlayingWorld(63, 0b11, c);
  placePlayer(s, kP0, at(0, 0));
  placePlayer(s, kP1, at(-10, 6));
  REQUIRE(playerAt(s, kP0).ammo == playerAt(s, kP0).stats.ammoMax);  // full
  const EntityId back = spawnBanana(s, kP0, at(1, 0), Vec2{-Fixed::fromRatio(1, 4), Fixed{}},
                                    BoomerangPhase::Returning, c);
  s.boomerangs.items[back.index].transform.facing = Angle::fromBam(Angle::kHalf);  // homing west
  EventLog log;
  runTicks(s, frameFor(s), 1, c, log);
  REQUIRE(findEvent(log, EventKind::BoomerangCaught) != nullptr);
  CHECK(playerAt(s, kP0).ammo == playerAt(s, kP0).stats.ammoMax);
  REQUIRE(countEvents(log, EventKind::BananaDropped) == 1);
  const Pickup* k = s.pickups.get(firstEvent(log, EventKind::BananaDropped).entity);
  REQUIRE(k != nullptr);
  CHECK(distance(k->pos, at(0, 0)) < Fixed::one());  // at the catch point
  // Standing on it at full ammo, the monkey leaves it lying.
  runTicks(s, frameFor(s), 30, c, log);
  CHECK(countEvents(log, EventKind::PickupTaken) == 0);
  CHECK(s.pickups.size() == 1);
}

TEST_CASE("a power-up running out takes the bananas above the new ammoMax with it",
          "[scenario][banana]") {
  ContentDB c = defaultContent();
  const uint8_t more = addAmmoPowerUp(c);
  const MatchContent m = matchContent(c, MatchConfig{.seed = 64, .players = 0b11});
  WorldState s = makePlayingWorld(64, 0b11, c);
  placePlayer(s, kP0, at(0, 0));
  placePlayer(s, kP1, at(-10, 6));
  givePowerUp(playerAt(s, kP0), more, 3);
  EventLog log;
  runTicks(s, frameFor(s), 1, m, log);
  playerAt(s, kP0).ammo = 3;
  runTicks(s, frameFor(s), 5, m, log);  // it expires
  CHECK(playerAt(s, kP0).stats.ammoMax == c.player.ammoMax);
  CHECK(playerAt(s, kP0).ammo == c.player.ammoMax);
  // v0.57 (PR #49 round 6): the extra ones vanish with the power-up.
  CHECK(countEvents(log, EventKind::BananaDropped) == 0);
  CHECK(bananaPickups(s).empty());
}

TEST_CASE("an ammo power-up's top-up never makes phantom bananas", "[scenario][banana]") {
  ContentDB c = defaultContent();
  const uint8_t more = addAmmoPowerUp(c);
  const MatchContent m = matchContent(c, MatchConfig{.seed = 65, .players = 0b11});
  WorldState s = makePlayingWorld(65, 0b11, c);
  placePlayer(s, kP0, at(0, 0));
  placePlayer(s, kP1, at(-10, 6));
  givePowerUp(playerAt(s, kP0), more, 600);
  EventLog log;
  runTicks(s, frameFor(s), 1, m, log);
  playerAt(s, kP0).ammo = 3;  // full with the power-up
  spawnPowerUpPickup(s, at(0, 0), more);
  runTicks(s, frameFor(s), 2, m, log);
  REQUIRE(countEvents(log, EventKind::PickupTaken) == 1);  // the same power-up, refreshed
  CHECK(playerAt(s, kP0).ammo == 3);
  CHECK(countEvents(log, EventKind::BananaDropped) == 0);
}

TEST_CASE("a full pickup pool stacks a new banana on the nearest banana pickup",
          "[scenario][banana]") {
  ContentDB c = defaultContent();
  const uint8_t more = addAmmoPowerUp(c);
  const MatchContent m = matchContent(c, MatchConfig{.seed = 66, .players = 0b111});
  WorldState s = makePlayingWorld(66, 0b111, c);
  placePlayer(s, kP0, at(0, 0));
  placePlayer(s, kP1, at(-14, 7));
  placePlayer(s, kP2, at(14, 7));
  // Fill the pool: one banana pickup near P0, the rest far off.
  const EntityId near = spawnPickup(s, at(2, 0));
  while (s.pickups.size() < kMaxPickups) {
    const auto i = static_cast<int32_t>(s.pickups.size());
    spawnPickup(s, Vec2{Fixed::fromInt(-14 + (i % 28)), Fixed::fromInt(-8)});
  }
  const EventLog log = hitAndStep(s, m, kP0, kP1);
  REQUIRE(firstEvent(log, EventKind::PlayerEliminated).player == kP0);
  CHECK(s.pickups.size() == kMaxPickups);
  CHECK(countEvents(log, EventKind::PickupSpawned) == 0);
  const Pickup* stacked = s.pickups.get(near);
  REQUIRE(stacked != nullptr);
  CHECK(stacked->count == 2);
  CHECK(firstEvent(log, EventKind::BananaDropped).entity == near);
  // Taking it takes as many as fit and leaves the rest: P2 with room for 3.
  givePowerUp(playerAt(s, kP2), more, 600);
  playerAt(s, kP2).ammo = 2;
  EventLog more2;
  runTicks(s, frameFor(s), 1, m, more2);
  placePlayer(s, kP2, at(2, 0));
  runTicks(s, frameFor(s), 1, m, more2);
  CHECK(playerAt(s, kP2).ammo == 3);
  REQUIRE(s.pickups.get(near) != nullptr);
  CHECK(s.pickups.get(near)->count == 1);
}
