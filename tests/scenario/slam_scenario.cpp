// Slam end to end (architecture §5.5, §5.6, v0.21): a radial impulse on the
// ability button. The impact tick widens the slammer's contact radius; rows
// 16 and 26 ground non-own bananas and push monkeys within the reach, once
// per slam. No other row takes a contact made only through the reach.
#include "kfm/sim/systems.hpp"
#include "loadout_util.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

const PlayerId kP0{0};
const PlayerId kP1{1};
const PlayerId kP2{2};
constexpr uint8_t kAbility = buttonBit(Button::Ability1);

Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

AbilityDef slamDef() {
  AbilityDef d;
  d.name = ContentName::of("slam");
  d.kind = AbilityKind::Slam;
  d.cooldownTicks = 120;
  d.startupTicks = 12;
  d.activeTicks = 12;
  d.recoveryTicks = 12;
  d.param0 = Fixed::fromInt(3);  // reach
  d.param1 = Fixed::fromInt(4);  // push
  return d;
}

// Slammer P0 at the centre, P1 2 units to its right (inside the reach, not
// touching), P2 5 units above (outside it).
WorldState slamWorld(const ContentDB& c) {
  WorldState s = makePlayingWorld(9, 0b111, c);
  placePlayer(s, kP0, at(0, 0));
  placePlayer(s, kP1, at(2, 0));
  placePlayer(s, kP2, at(0, 5));
  return s;
}

}  // namespace

TEST_CASE("slam: the impact pushes monkeys within the reach, once", "[scenario][slam]") {
  const ContentDB c = withExtraSlot(slamDef());
  const AbilityDef def = slamDef();
  WorldState s = slamWorld(c);
  EventLog log;
  const Tick pressTick = s.tick;
  pressOnce(s, kP0, kAbility, c, log);
  idle(s, def.startupTicks - 1, c, log);
  CHECK(countEvents(log, EventKind::SlamLanded) == 0);
  CHECK_FALSE(playerAt(s, kP0).tags.has(Tag::Slamming));
  idle(s, 1, c, log);  // the impact tick
  const SimEvent& landed = firstEvent(log, EventKind::SlamLanded);
  CHECK(landed.tick.minus(pressTick) == def.startupTicks);
  CHECK(static_cast<int32_t>(landed.a) == def.param0.raw);
  CHECK(playerAt(s, kP0).tags.has(Tag::Slamming));
  const SimEvent& slammed = firstEvent(log, EventKind::PlayerSlammed);
  CHECK(slammed.player == kP1);
  CHECK(slammed.a == kP0.value);
  CHECK(slammed.tick == landed.tick);
  CHECK(playerAt(s, kP1).knockbackTicks == def.activeTicks);
  // Over the active ticks P1 slides param1 units away (to the right); P2 and
  // the slammer stay put; nobody is hurt.
  idle(s, def.activeTicks + 2, c, log);
  CHECK_FALSE(playerAt(s, kP0).tags.has(Tag::Slamming));
  const Fixed x1 = playerAt(s, kP1).transform.pos.x;
  CHECK(x1 > Fixed::fromRatio(599, 100));
  CHECK(x1 <= Fixed::fromInt(6));
  CHECK(playerAt(s, kP1).transform.pos.y == Fixed::zero());
  CHECK(playerAt(s, kP1).knockbackTicks == 0);
  CHECK(playerAt(s, kP1).knockback == Vec2::zero());
  CHECK(playerAt(s, kP2).transform.pos == at(0, 5));
  CHECK(playerAt(s, kP0).transform.pos == at(0, 0));
  CHECK(countEvents(log, EventKind::PlayerSlammed) == 1);
  CHECK(countEvents(log, EventKind::PlayerHit) == 0);
  CHECK(countEvents(log, EventKind::PlayerDashHit) == 0);
}

TEST_CASE("slam: bananas in flight within the reach drop, own ones and pickups do not react",
          "[scenario][slam]") {
  const ContentDB c = withExtraSlot(slamDef());
  WorldState s = slamWorld(c);
  // P1's banana hovering 2.5 below the slammer; the slammer's own above it.
  const Vec2 slow{Fixed::fromRatio(1, 64), Fixed::zero()};
  const EntityId enemy = spawnBanana(s, kP1, Vec2{Fixed::zero(), Fixed::fromRatio(-5, 2)}, slow);
  const EntityId own = spawnBanana(s, kP0, Vec2{Fixed::zero(), Fixed::fromRatio(5, 2)}, slow);
  // A pickup within the reach, with room in hand: the reach does not take it.
  playerAt(s, kP0).ammo = 0;
  spawnPickup(s, Vec2{Fixed::fromInt(-2), Fixed::fromInt(-1)});
  EventLog log;
  pressOnce(s, kP0, kAbility, c, log);
  idle(s, slamDef().startupTicks, c, log);
  REQUIRE(countEvents(log, EventKind::SlamLanded) == 1);
  CHECK_FALSE(s.boomerangs.isAlive(enemy));  // grounded, then a pickup
  CHECK(countEvents(log, EventKind::BoomerangGrounded) == 1);
  CHECK(s.boomerangs.isAlive(own));
  CHECK(countEvents(log, EventKind::PickupTaken) == 0);
  CHECK(playerAt(s, kP0).ammo == 0);
}

TEST_CASE("slam: before the impact a banana hits; the cooldown holds", "[scenario][slam]") {
  const ContentDB c = withExtraSlot(slamDef());
  SECTION("a banana arriving during the startup falls through to damage") {
    WorldState s = slamWorld(c);
    spawnBanana(s, kP2, Vec2{Fixed::fromRatio(-3, 2), Fixed::zero()},
                Vec2{c.boomerang.speed, Fixed::zero()});
    EventLog log;
    pressOnce(s, kP0, kAbility, c, log);
    idle(s, slamDef().startupTicks, c, log);
    CHECK(countEvents(log, EventKind::PlayerHit) == 1);
    CHECK(countEvents(log, EventKind::SlamLanded) == 0);
  }
  SECTION("a second press inside the cooldown does nothing") {
    WorldState s = slamWorld(c);
    EventLog log;
    pressOnce(s, kP0, kAbility, c, log);
    idle(s, 59, c, log);
    pressOnce(s, kP0, kAbility, c, log);
    idle(s, 59, c, log);
    CHECK(countEvents(log, EventKind::SlamLanded) == 1);
    pressOnce(s, kP0, kAbility, c, log);  // 120 ticks after the first press
    idle(s, slamDef().startupTicks, c, log);
    CHECK(countEvents(log, EventKind::SlamLanded) == 2);
  }
}

TEST_CASE("slam: a contact made only through the reach matches no other row", "[scenario][slam]") {
  const ContentDB c = withExtraSlot(slamDef());
  WorldState s = slamWorld(c);
  // P1 dashing with a banana in hand, P0 slamming: a dash attack needs bodies
  // that touch, so a reach-only contact from P1's side resolves nothing.
  Player& dasher = playerAt(s, kP1);
  dasher.tags.set(Tag::Dashing);
  dasher.ammo = 1;
  playerAt(s, kP0).tags.set(Tag::Slamming);
  Contact contact;
  contact.kindA = EntityKind::Player;
  contact.a = s.players.idAt(kP1.value);
  contact.kindB = EntityKind::Player;
  contact.b = s.players.idAt(kP0.value);
  contact.reach = kReachB;
  EventSink sink;
  interactionResolveContact(s, c, contact, sink);
  CHECK(countEvents(sink, EventKind::PlayerDashHit) == 0);
  CHECK(playerAt(s, kP0).hp == c.player.maxHp);
  // The same touch with the bodies in contact is a dash attack.
  contact.reach = 0;
  interactionResolveContact(s, c, contact, sink);
  CHECK(countEvents(sink, EventKind::PlayerDashHit) == 1);
}
