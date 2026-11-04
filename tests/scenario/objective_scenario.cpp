// Objectives (architecture §5.3 "Mode seams", §5.10, v0.33): arena zones and
// zoneOccupants(), the hill and capture kinds, and ContentDB::compatible().
#include "kfm/sim/zones.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kZoned = 1;  // the dojo with two spawn groups and three zones
constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};
constexpr PlayerId kP2{2};
constexpr PlayerId kP3{3};

[[nodiscard]] constexpr Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

ContentDB objectiveContent() {
  ContentDB c = defaultContent();
  ArenaDef& zoned = c.arenas[kZoned];
  zoned = c.arenas[0];
  zoned.name = ContentName::of("zoned");
  zoned.spawnGroups = 2;
  zoned.zones[0] = Zone{at(-2, -2), at(2, 2)};     // the centre: hill
  zoned.zones[1] = Zone{at(-15, -2), at(-11, 2)};  // west (capture: team 0's is zone 0)
  zoned.zones[2] = Zone{at(11, -2), at(15, 2)};    // east
  zoned.zoneCount = 3;
  c.arenaCount = 2;
  return c;
}

const ContentDB& content() {
  static const ContentDB c = objectiveContent();
  return c;
}

}  // namespace

TEST_CASE("zones: occupants are the live monkeys whose centre is inside", "[scenario][zones]") {
  const MatchContent m(content(), kZoned, 0);
  WorldState s = makePlayingWorld(0x20E5, 0b1111, content());
  placePlayer(s, kP0, at(0, 0));
  placePlayer(s, kP1, at(2, 2));  // on the corner: inside (closed)
  placePlayer(s, kP2, Vec2{Fixed::fromInt(2) + Fixed::epsilon(), Fixed::zero()});  // just out
  placePlayer(s, kP3, at(-13, 0));
  const ArenaDef& arena = m.arena;
  CHECK(zoneOccupants(s, arena, 0) == 0b0011);
  CHECK(zoneOccupants(s, arena, 1) == 0b1000);
  CHECK(zoneOccupants(s, arena, 2) == 0);
  CHECK(zoneOccupants(s, arena, 3) == 0);  // no such zone
  CHECK(zoneOccupants(s, arena, kMaxArenaZones) == 0);
  // An eliminated monkey still in the pool counts for nobody.
  playerAt(s, kP1).tags.set(Tag::Eliminated);
  CHECK(zoneOccupants(s, arena, 0) == 0b0001);
  // Zones block nothing: a monkey walks straight through one.
  EventLog log;
  playerAt(s, kP1).tags.clear(Tag::Eliminated);
  placePlayer(s, kP0, at(-4, 0));
  InputFrame f = frameFor(s);
  f.inputs[0].moveDir = 0;  // +x
  f.inputs[0].moveMag = 255;
  runTicks(s, f, 90, m, log);
  CHECK(playerAt(s, kP0).transform.pos.x > Fixed::fromInt(2));
}
