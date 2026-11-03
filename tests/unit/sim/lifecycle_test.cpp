// LifecycleSystem (architecture §5.3 step 9, §5.7): eliminated players leave
// the pool, grounded bananas become pickups.
#include "kfm/sim/systems.hpp"
#include "sim_test_util.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

EventSink runLifecycle(WorldState& s) {
  EventSink sink;
  lifecycleSystem(s, defaultContent(), sink);
  REQUIRE(sink.dropped == 0);
  return sink;
}

}  // namespace

TEST_CASE("eliminated players are despawned, others stay", "[sim][lifecycle]") {
  WorldState s = makePlayingWorld(1, 0b111);
  s.playerBySlot(PlayerId{1})->tags.set(Tag::Eliminated);
  s.playerBySlot(PlayerId{2})->tags.set(Tag::HitStun);
  const EventSink sink = runLifecycle(s);
  CHECK(sink.count == 0);  // PlayerEliminated was already emitted by the damage rule
  CHECK(s.players.size() == 2);
  CHECK(s.playerBySlot(PlayerId{0}) != nullptr);
  CHECK(s.playerBySlot(PlayerId{1}) == nullptr);
  CHECK(s.playerBySlot(PlayerId{2}) != nullptr);
}

TEST_CASE("grounded bananas become pickups at their position", "[sim][lifecycle]") {
  WorldState s = initialWorld(MatchConfig{1, 0b11});
  const Vec2 groundPos{Fixed::fromInt(3), Fixed::fromRatio(-5, 2)};
  const EntityId flying = spawnBanana(s, PlayerId{0}, Vec2::zero(), Vec2::zero());
  const EntityId returning =
      spawnBanana(s, PlayerId{0}, Vec2::zero(), Vec2::zero(), BoomerangPhase::Returning);
  const EntityId grounded =
      spawnBanana(s, PlayerId{1}, groundPos, Vec2::zero(), BoomerangPhase::Grounded);

  const EventSink sink = runLifecycle(s);
  CHECK(s.boomerangs.isAlive(flying));
  CHECK(s.boomerangs.isAlive(returning));
  CHECK_FALSE(s.boomerangs.isAlive(grounded));
  REQUIRE(s.pickups.size() == 1);

  REQUIRE(sink.count == 2);
  CHECK(sink.events[0].kind == EventKind::BoomerangGrounded);
  CHECK(sink.events[0].entity == grounded);
  CHECK(sink.events[1].kind == EventKind::PickupSpawned);
  const EntityId pickup = sink.events[1].entity;
  REQUIRE(s.pickups.isAlive(pickup));
  CHECK(s.pickups.items[pickup.index].kind == PickupKind::Banana);
  CHECK(s.pickups.items[pickup.index].pos == groundPos);
}

TEST_CASE("several grounded bananas convert in pool order", "[sim][lifecycle]") {
  WorldState s = initialWorld(MatchConfig{1, 0b11});
  const EntityId a = spawnBanana(s, PlayerId{0}, Vec2{Fixed::fromInt(1), Fixed::zero()},
                                 Vec2::zero(), BoomerangPhase::Grounded);
  const EntityId b = spawnBanana(s, PlayerId{1}, Vec2{Fixed::fromInt(2), Fixed::zero()},
                                 Vec2::zero(), BoomerangPhase::Grounded);
  const EventSink sink = runLifecycle(s);
  REQUIRE(sink.count == 4);
  CHECK(sink.events[0].entity == a);
  CHECK(sink.events[2].entity == b);
  CHECK(s.boomerangs.empty());
  REQUIRE(s.pickups.size() == 2);
  CHECK(s.pickups.items[sink.events[1].entity.index].pos.x == Fixed::fromInt(1));
  CHECK(s.pickups.items[sink.events[3].entity.index].pos.x == Fixed::fromInt(2));
}

TEST_CASE("with the pickup pool full a grounded banana just vanishes", "[sim][lifecycle]") {
  WorldState s = initialWorld(MatchConfig{1, 0b11});
  while (!s.pickups.full())
    static_cast<void>(spawnPickup(s, Vec2::zero()));
  const EntityId grounded =
      spawnBanana(s, PlayerId{0}, Vec2::zero(), Vec2::zero(), BoomerangPhase::Grounded);
  const EventSink sink = runLifecycle(s);
  CHECK_FALSE(s.boomerangs.isAlive(grounded));
  CHECK(s.pickups.size() == kMaxPickups);
  REQUIRE(sink.count == 1);
  CHECK(sink.events[0].kind == EventKind::BoomerangGrounded);
  CHECK(sink.events[0].entity == grounded);
}

TEST_CASE("lifecycle on an empty world is a no-op", "[sim][lifecycle]") {
  WorldState s = initialWorld(MatchConfig{1, 0b11});
  const uint32_t before = checksum(s);
  const EventSink sink = runLifecycle(s);
  CHECK(sink.count == 0);
  CHECK(checksum(s) == before);
}
