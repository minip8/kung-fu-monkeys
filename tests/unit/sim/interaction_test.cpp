// InteractionSystem and the M1 rule handlers (architecture §5.6, §5.11).
// Contacts are built by hand and interactionSystem is called directly, so
// these tests do not depend on collision or ability behaviour.
#include "kfm/sim/rules/interaction.hpp"

#include "kfm/sim/rules/handlers.hpp"
#include "kfm/sim/systems.hpp"
#include "sim_test_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <initializer_list>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

const PlayerId kP0{0};
const PlayerId kP1{1};
const PlayerId kP2{2};

Contact touch(const WorldState& s, PlayerId player, EntityKind kindB, EntityId b, Fixed tHit,
              Vec2 point = Vec2::zero()) {
  Contact c;
  c.kindA = EntityKind::Player;
  c.a = s.players.idAt(player.value);
  c.kindB = kindB;
  c.b = b;
  c.tHit = tHit;
  c.point = point;
  c.normal = Vec2{Fixed::one(), Fixed::zero()};
  return c;
}

Contacts contactsOf(std::initializer_list<Contact> list) {
  Contacts cs;
  for (const Contact& c : list)
    cs.push(c);
  return cs;
}

// Gives a player PerfectDash with the dash pressed at (tick, subTick).
void givePerfectDash(WorldState& s, PlayerId id, Tick pressTick, uint8_t pressSubTick) {
  Player& p = playerAt(s, id);
  p.tags.set(Tag::Dashing);
  p.tags.set(Tag::PerfectDash);
  AbilityState& dash = p.abilities[kAbilitySlotDash];
  dash.phase = AbilityPhase::Active;
  dash.activatedTick = pressTick;
  dash.activatedSubTick = pressSubTick;
}

// Gives a player Dashing (past the perfect window) with the dash pressed at
// (tick, subTick).
void giveDash(WorldState& s, PlayerId id, Tick pressTick, uint8_t pressSubTick) {
  Player& p = playerAt(s, id);
  p.tags.set(Tag::Dashing);
  AbilityState& dash = p.abilities[kAbilitySlotDash];
  dash.phase = AbilityPhase::Active;
  dash.activatedTick = pressTick;
  dash.activatedSubTick = pressSubTick;
}

// A Player x Player contact as CollisionSystem lists it (lower slot as A).
Contact playersTouch(const WorldState& s, PlayerId a, PlayerId b, Fixed tHit) {
  Contact c;
  c.kindA = EntityKind::Player;
  c.a = s.players.idAt(a.value);
  c.kindB = EntityKind::Player;
  c.b = s.players.idAt(b.value);
  c.tHit = tHit;
  c.normal = Vec2{Fixed::one(), Fixed::zero()};
  return c;
}

Fixed ticks(double t) {
  return Fixed::fromDouble(t);
}

EventSink resolve(WorldState& s, const Contacts& cs, const ContentDB& content = defaultContent()) {
  EventSink sink;
  interactionSystem(s, content, cs, sink);
  REQUIRE(sink.dropped == 0);
  return sink;
}

}  // namespace

TEST_CASE("rules table is sorted and every row ignores eliminated players", "[sim][interaction]") {
  const auto rules = interactionRules();
  REQUIRE(rules.size() == 7);  // M1's six + the pit (v0.22)
  for (size_t i = 0; i < rules.size(); ++i) {
    if (i > 0) CHECK(rules[i - 1].priority < rules[i].priority);
    CHECK(rules[i].kindA == EntityKind::Player);
    CHECK(rules[i].forbidA.has(Tag::Eliminated));
    if (rules[i].kindB == EntityKind::Player) CHECK(rules[i].forbidB.has(Tag::Eliminated));
    CHECK(rules[i].resolve != nullptr);
  }
}

TEST_CASE("the dash attack row: Player[Dashing] x Player[!HitStun, !Invulnerable]",
          "[sim][interaction]") {
  const auto rules = interactionRules();
  const auto row = std::ranges::find(rules, uint8_t{25}, &InteractionRule::priority);
  REQUIRE(row != rules.end());
  CHECK(row->kindA == EntityKind::Player);
  CHECK(row->kindB == EntityKind::Player);
  CHECK(row->requireA == Tags::of(Tag::Dashing));
  CHECK(row->forbidA.has(Tag::HitStun));  // a stunned dasher cannot strike back
  CHECK(row->requireB == Tags{});
  CHECK(row->forbidB.has(Tag::HitStun));
  CHECK(row->forbidB.has(Tag::Invulnerable));
  CHECK(row->ownership == Ownership::Any);
  CHECK(row->resolve == &dashAttack);
  // The only row on Player x Player.
  CHECK(std::ranges::count(rules, EntityKind::Player, &InteractionRule::kindB) == 1);
}

TEST_CASE("perfect catch inside the window takes the banana and an ammo", "[sim][interaction]") {
  WorldState s = makePlayingWorld(1, 0b11);
  playerAt(s, kP0).ammo = 0;
  const EntityId banana = spawnBanana(s, kP1, Vec2::zero(), Vec2{Fixed::one(), Fixed::zero()});
  givePerfectDash(s, kP0, s.tick, 128);  // pressed half way through this tick

  const EventSink sink =
      resolve(s, contactsOf({touch(s, kP0, EntityKind::Boomerang, banana, ticks(0.75))}));
  CHECK_FALSE(s.boomerangs.isAlive(banana));
  CHECK(playerAt(s, kP0).ammo == 1);
  REQUIRE(sink.count == 1);
  CHECK(sink.events[0].kind == EventKind::BoomerangCaught);
  CHECK(sink.events[0].player == kP0);
  CHECK(sink.events[0].entity == banana);
  CHECK(sink.events[0].a == static_cast<uint32_t>(CatchHow::Perfect));
  CHECK(sink.events[0].tick == s.tick);
}

TEST_CASE("a perfect catch at ammoMax declines and falls through", "[sim][interaction]") {
  // §5.6 (v0.16): the catch needs room below ammoMax. Declining keeps the
  // scan going: a non-own banana in flight reaches the damage row, an own
  // returning banana the auto-catch row (absorbed even at full ammo).
  WorldState s = makePlayingWorld(1, 0b11);
  REQUIRE(playerAt(s, kP0).ammo == playerAt(s, kP0).stats.ammoMax);
  givePerfectDash(s, kP0, s.tick, 0);

  SECTION("non-own banana in flight: the full-ammo dasher is hit") {
    const EntityId banana = spawnBanana(s, kP1, Vec2::zero(), Vec2::zero());
    const EventSink sink =
        resolve(s, contactsOf({touch(s, kP0, EntityKind::Boomerang, banana, ticks(0.5))}));
    CHECK(countEvents(sink, EventKind::BoomerangCaught) == 0);
    CHECK(countEvents(sink, EventKind::PlayerHit) == 1);
    CHECK(playerAt(s, kP0).tags.has(Tag::Eliminated));
    REQUIRE(s.boomerangs.isAlive(banana));
    CHECK(s.boomerangs.items[banana.index].phase == BoomerangPhase::Grounded);
  }
  SECTION("own returning banana: auto-caught, not perfect") {
    const EntityId banana =
        spawnBanana(s, kP0, Vec2::zero(), Vec2::zero(), BoomerangPhase::Returning);
    const EventSink sink =
        resolve(s, contactsOf({touch(s, kP0, EntityKind::Boomerang, banana, ticks(0.5))}));
    REQUIRE(countEvents(sink, EventKind::BoomerangCaught) == 1);
    CHECK(firstEvent(sink, EventKind::BoomerangCaught).a == static_cast<uint32_t>(CatchHow::Auto));
    CHECK_FALSE(s.boomerangs.isAlive(banana));
    CHECK(playerAt(s, kP0).ammo == playerAt(s, kP0).stats.ammoMax);
  }
}

TEST_CASE("perfect window edges are judged against press sub-tick and tHit", "[sim][interaction]") {
  const ContentDB& content = defaultContent();
  const uint16_t window = content.abilityForSlot(kAbilitySlotDash).perfectWindowTicks;
  REQUIRE(window == 4);

  struct Case {
    const char* name;
    int32_t pressTicksAgo;
    uint8_t pressSubTick;
    double tHit;
    bool perfect;
  };
  const Case cases[] = {
      {"hit at the press instant", 0, 0, 0.0, true},
      {"last moment of the window", 3, 0, 0.99, true},
      {"exactly one window after the press", 4, 0, 0.0, false},
      {"window measured from the sub-tick press", 4, 128, 0.25, true},
      {"window from the sub-tick press, just past", 4, 128, 0.5, false},
      {"press late in T-1, hit early in T", 1, 250, 0.05, true},
      {"hit before the press in the same tick", 0, 200, 0.5, false},
  };
  for (const Case& c : cases) {
    DYNAMIC_SECTION(c.name) {
      WorldState s = makePlayingWorld(3, 0b11);
      playerAt(s, kP0).ammo = 0;
      const EntityId banana = spawnBanana(s, kP1, Vec2::zero(), Vec2::zero());
      givePerfectDash(s, kP0, s.tick.plus(-c.pressTicksAgo), c.pressSubTick);
      const EventSink sink =
          resolve(s, contactsOf({touch(s, kP0, EntityKind::Boomerang, banana, ticks(c.tHit))}));
      if (c.perfect) {
        CHECK(countEvents(sink, EventKind::BoomerangCaught) == 1);
        CHECK(countEvents(sink, EventKind::PlayerHit) == 0);
        CHECK_FALSE(s.boomerangs.isAlive(banana));
      } else {
        // Outside the window the catch declines and the scan falls through
        // to the damage rule (the banana is not the catcher's).
        CHECK(countEvents(sink, EventKind::BoomerangCaught) == 0);
        CHECK(countEvents(sink, EventKind::PlayerHit) == 1);
        CHECK(playerAt(s, kP0).tags.has(Tag::Eliminated));
      }
    }
  }
}

TEST_CASE("mistimed perfect dash on an own outbound banana does nothing", "[sim][interaction]") {
  WorldState s = makePlayingWorld(1, 0b11);
  const EntityId banana = spawnBanana(s, kP0, Vec2::zero(), Vec2{Fixed::one(), Fixed::zero()});
  givePerfectDash(s, kP0, s.tick.plus(-10), 0);
  const EventSink sink =
      resolve(s, contactsOf({touch(s, kP0, EntityKind::Boomerang, banana, Fixed::zero())}));
  CHECK(sink.count == 0);
  CHECK(s.boomerangs.isAlive(banana));
}

TEST_CASE("a perfect dash passes through an own outbound banana", "[sim][interaction]") {
  WorldState s = makePlayingWorld(1, 0b11);
  const EntityId banana = spawnBanana(s, kP0, Vec2::zero(), Vec2{Fixed::one(), Fixed::zero()});
  givePerfectDash(s, kP0, s.tick, 0);  // inside the window
  const EventSink sink =
      resolve(s, contactsOf({touch(s, kP0, EntityKind::Boomerang, banana, ticks(0.5))}));
  CHECK(sink.count == 0);
  CHECK(s.boomerangs.isAlive(banana));
}

TEST_CASE("a perfect dash catches an own returning banana as Perfect", "[sim][interaction]") {
  WorldState s = makePlayingWorld(1, 0b11);
  playerAt(s, kP0).ammo = 0;
  const EntityId banana =
      spawnBanana(s, kP0, Vec2::zero(), Vec2::zero(), BoomerangPhase::Returning);
  givePerfectDash(s, kP0, s.tick, 0);
  const EventSink sink =
      resolve(s, contactsOf({touch(s, kP0, EntityKind::Boomerang, banana, ticks(0.5))}));
  REQUIRE(sink.count == 1);
  CHECK(sink.events[0].kind == EventKind::BoomerangCaught);
  CHECK(sink.events[0].a == static_cast<uint32_t>(CatchHow::Perfect));
  CHECK_FALSE(s.boomerangs.isAlive(banana));
}

TEST_CASE("own outbound banana matches no rule", "[sim][interaction]") {
  WorldState s = makePlayingWorld(1, 0b11);
  const Vec2 vel{Fixed::one(), Fixed::zero()};
  const EntityId banana = spawnBanana(s, kP0, Vec2::zero(), vel);
  const uint32_t before = checksum(s);
  const EventSink sink =
      resolve(s, contactsOf({touch(s, kP0, EntityKind::Boomerang, banana, Fixed::zero())}));
  CHECK(sink.count == 0);
  CHECK(checksum(s) == before);
}

TEST_CASE("own returning banana is auto-caught, ammo capped", "[sim][interaction]") {
  WorldState s = makePlayingWorld(1, 0b11);
  Player& p0 = playerAt(s, kP0);

  SECTION("from empty") {
    p0.ammo = 0;
  }
  SECTION("already full: still absorbed") {
    p0.ammo = p0.stats.ammoMax;
  }
  const EntityId banana =
      spawnBanana(s, kP0, Vec2::zero(), Vec2::zero(), BoomerangPhase::Returning);
  const EventSink sink =
      resolve(s, contactsOf({touch(s, kP0, EntityKind::Boomerang, banana, ticks(0.3))}));
  CHECK_FALSE(s.boomerangs.isAlive(banana));
  CHECK(playerAt(s, kP0).ammo == 1);
  REQUIRE(sink.count == 1);
  CHECK(sink.events[0].kind == EventKind::BoomerangCaught);
  CHECK(sink.events[0].player == kP0);
  CHECK(sink.events[0].a == static_cast<uint32_t>(CatchHow::Auto));
  CHECK(countEvents(sink, EventKind::PlayerHit) == 0);
}

TEST_CASE("another player's banana damages, stuns and grounds at the contact point",
          "[sim][interaction]") {
  WorldState s = makePlayingWorld(1, 0b11);
  const Vec2 point{Fixed::fromInt(2), Fixed::fromRatio(-1, 2)};
  const EntityId banana = spawnBanana(s, kP1, Vec2{Fixed::fromInt(3), Fixed::zero()},
                                      Vec2{Fixed::one(), Fixed::zero()}, BoomerangPhase::Returning);
  s.boomerangs.items[banana.index].ticksInPhase = 9;
  const EventSink sink =
      resolve(s, contactsOf({touch(s, kP0, EntityKind::Boomerang, banana, ticks(0.5), point)}));

  const Player& p0 = playerAt(s, kP0);
  CHECK(p0.hp == 0);
  CHECK(p0.tags.has(Tag::HitStun));
  CHECK(p0.tags.has(Tag::Eliminated));
  CHECK(p0.hitStunTicks == defaultContent().player.hitStunTicks);

  REQUIRE(sink.count == 2);
  CHECK(sink.events[0].kind == EventKind::PlayerHit);
  CHECK(sink.events[0].player == kP0);
  CHECK(sink.events[0].entity == banana);
  CHECK(sink.events[0].a == kP1.value);
  CHECK(sink.events[0].b == 0);
  CHECK(sink.events[1].kind == EventKind::PlayerEliminated);
  CHECK(sink.events[1].player == kP0);
  CHECK(sink.events[1].a == kP1.value);

  REQUIRE(s.boomerangs.isAlive(banana));
  const Boomerang& b = s.boomerangs.items[banana.index];
  CHECK(b.phase == BoomerangPhase::Grounded);
  CHECK(b.ticksInPhase == 0);
  CHECK(b.transform.pos == point);
  CHECK(b.transform.prevPos == point);
  CHECK(b.transform.vel == Vec2::zero());
}

TEST_CASE("non-lethal hit keeps the player in play", "[sim][interaction]") {
  ContentDB content = defaultContent();
  content.player.maxHp = 3;
  WorldState s = makePlayingWorld(1, 0b11, content);
  REQUIRE(playerAt(s, kP0).hp == 3);
  const EntityId banana =
      spawnBanana(s, kP1, Vec2::zero(), Vec2::zero(), BoomerangPhase::Outbound, content);
  s.boomerangs.items[banana.index].damage = 2;
  const EventSink sink = resolve(
      s, contactsOf({touch(s, kP0, EntityKind::Boomerang, banana, Fixed::zero())}), content);
  CHECK(playerAt(s, kP0).hp == 1);
  CHECK_FALSE(playerAt(s, kP0).tags.has(Tag::Eliminated));
  CHECK(countEvents(sink, EventKind::PlayerEliminated) == 0);
  REQUIRE(findEvent(sink, EventKind::PlayerHit) != nullptr);
  CHECK(firstEvent(sink, EventKind::PlayerHit).b == 1);
}

TEST_CASE("damage saturates at zero hp", "[sim][interaction]") {
  WorldState s = makePlayingWorld(1, 0b11);
  const EntityId banana = spawnBanana(s, kP1, Vec2::zero(), Vec2::zero());
  s.boomerangs.items[banana.index].damage = 200;
  const EventSink sink =
      resolve(s, contactsOf({touch(s, kP0, EntityKind::Boomerang, banana, Fixed::zero())}));
  CHECK(playerAt(s, kP0).hp == 0);
  CHECK(countEvents(sink, EventKind::PlayerEliminated) == 1);
}

TEST_CASE("piercing banana keeps flying after a hit", "[sim][interaction]") {
  WorldState s = makePlayingWorld(1, 0b11);
  const Vec2 pos{Fixed::fromInt(1), Fixed::fromInt(1)};
  const Vec2 vel{Fixed::fromRatio(1, 4), Fixed::zero()};
  const EntityId banana = spawnBanana(s, kP1, pos, vel);
  s.boomerangs.items[banana.index].pierce = 1;
  const EventSink sink = resolve(
      s, contactsOf({touch(s, kP0, EntityKind::Boomerang, banana, ticks(0.5), Vec2::zero())}));
  CHECK(countEvents(sink, EventKind::PlayerHit) == 1);
  const Boomerang& b = s.boomerangs.items[banana.index];
  CHECK(b.phase == BoomerangPhase::Outbound);
  CHECK(b.transform.pos == pos);
  CHECK(b.transform.vel == vel);
}

TEST_CASE("HitStun blocks a second hit; contacts resolve by time of impact", "[sim][interaction]") {
  ContentDB content = defaultContent();
  content.player.maxHp = 3;
  WorldState s = makePlayingWorld(1, 0b111, content);
  const EntityId late =
      spawnBanana(s, kP1, Vec2::zero(), Vec2::zero(), BoomerangPhase::Outbound, content);
  const EntityId early =
      spawnBanana(s, kP2, Vec2::zero(), Vec2::zero(), BoomerangPhase::Outbound, content);
  // Listed late-first: the system must reorder by tHit.
  const EventSink sink =
      resolve(s,
              contactsOf({touch(s, kP0, EntityKind::Boomerang, late, ticks(0.6)),
                          touch(s, kP0, EntityKind::Boomerang, early, ticks(0.2))}),
              content);
  REQUIRE(countEvents(sink, EventKind::PlayerHit) == 1);
  CHECK(firstEvent(sink, EventKind::PlayerHit).entity == early);
  CHECK(firstEvent(sink, EventKind::PlayerHit).a == kP2.value);
  CHECK(playerAt(s, kP0).hp == 2);
  CHECK(s.boomerangs.items[late.index].phase == BoomerangPhase::Outbound);
  CHECK(s.boomerangs.items[early.index].phase == BoomerangPhase::Grounded);
}

TEST_CASE("Invulnerable players are not damaged", "[sim][interaction]") {
  WorldState s = makePlayingWorld(1, 0b11);
  playerAt(s, kP0).tags.set(Tag::Invulnerable);
  const EntityId banana = spawnBanana(s, kP1, Vec2::zero(), Vec2::zero());
  const EventSink sink =
      resolve(s, contactsOf({touch(s, kP0, EntityKind::Boomerang, banana, Fixed::zero())}));
  CHECK(sink.count == 0);
  CHECK(playerAt(s, kP0).hp == 1);
}

TEST_CASE("an eliminated player cannot take a pickup later in the same tick",
          "[sim][interaction]") {
  WorldState s = makePlayingWorld(1, 0b11);
  playerAt(s, kP0).ammo = 0;
  const EntityId banana = spawnBanana(s, kP1, Vec2::zero(), Vec2::zero());
  const EntityId pickup = spawnPickup(s, Vec2::zero());
  const EventSink sink =
      resolve(s, contactsOf({touch(s, kP0, EntityKind::Boomerang, banana, ticks(0.1)),
                             touch(s, kP0, EntityKind::Pickup, pickup, ticks(0.9))}));
  CHECK(countEvents(sink, EventKind::PlayerEliminated) == 1);
  CHECK(countEvents(sink, EventKind::PickupTaken) == 0);
  CHECK(s.pickups.isAlive(pickup));
  CHECK(playerAt(s, kP0).ammo == 0);
}

TEST_CASE("two perfect dashers on one banana: earlier press wins, then lower slot",
          "[sim][interaction]") {
  WorldState s = makePlayingWorld(5, 0b111);
  playerAt(s, kP0).ammo = 0;
  playerAt(s, kP1).ammo = 0;
  const EntityId banana = spawnBanana(s, kP2, Vec2::zero(), Vec2::zero());

  PlayerId expected = kP0;
  SECTION("slot 1 pressed earlier in the same tick") {
    givePerfectDash(s, kP0, s.tick, 100);
    givePerfectDash(s, kP1, s.tick, 10);
    expected = kP1;
  }
  SECTION("slot 1 pressed a tick earlier, with a larger sub-tick") {
    givePerfectDash(s, kP0, s.tick, 0);
    givePerfectDash(s, kP1, s.tick.plus(-1), 255);
    expected = kP1;
  }
  SECTION("identical press: lower slot wins") {
    givePerfectDash(s, kP0, s.tick, 50);
    givePerfectDash(s, kP1, s.tick, 50);
    expected = kP0;
  }
  // Slot 1's contact is listed first so list order cannot decide it.
  const EventSink sink =
      resolve(s, contactsOf({touch(s, kP1, EntityKind::Boomerang, banana, ticks(0.5)),
                             touch(s, kP0, EntityKind::Boomerang, banana, ticks(0.5))}));
  REQUIRE(countEvents(sink, EventKind::BoomerangCaught) == 1);
  CHECK(firstEvent(sink, EventKind::BoomerangCaught).player == expected);
  CHECK(countEvents(sink, EventKind::PlayerHit) == 0);
  CHECK(playerAt(s, expected).ammo == 1);
  CHECK(playerAt(s, expected == kP0 ? kP1 : kP0).ammo == 0);
}

TEST_CASE("earlier time of impact beats an earlier press", "[sim][interaction]") {
  WorldState s = makePlayingWorld(5, 0b111);
  playerAt(s, kP0).ammo = 0;
  playerAt(s, kP1).ammo = 0;
  const EntityId banana = spawnBanana(s, kP2, Vec2::zero(), Vec2::zero());
  givePerfectDash(s, kP0, s.tick.plus(-1), 0);  // pressed a tick earlier
  givePerfectDash(s, kP1, s.tick, 0);
  const EventSink sink =
      resolve(s, contactsOf({touch(s, kP0, EntityKind::Boomerang, banana, ticks(0.6)),
                             touch(s, kP1, EntityKind::Boomerang, banana, ticks(0.4))}));
  REQUIRE(findEvent(sink, EventKind::BoomerangCaught) != nullptr);
  CHECK(firstEvent(sink, EventKind::BoomerangCaught).player == kP1);
}

TEST_CASE("pickups restore ammo up to ammoMax; a full player leaves them", "[sim][interaction]") {
  WorldState s = makePlayingWorld(1, 0b11);
  const EntityId pickup = spawnPickup(s, Vec2::zero());

  SECTION("empty player takes it") {
    playerAt(s, kP0).ammo = 0;
    const EventSink sink =
        resolve(s, contactsOf({touch(s, kP0, EntityKind::Pickup, pickup, Fixed::zero())}));
    CHECK_FALSE(s.pickups.isAlive(pickup));
    CHECK(playerAt(s, kP0).ammo == 1);
    REQUIRE(sink.count == 1);
    CHECK(sink.events[0].kind == EventKind::PickupTaken);
    CHECK(sink.events[0].player == kP0);
    CHECK(sink.events[0].entity == pickup);
  }
  SECTION("full player leaves it for the next one") {
    playerAt(s, kP1).ammo = 0;
    const EventSink sink =
        resolve(s, contactsOf({touch(s, kP0, EntityKind::Pickup, pickup, ticks(0.1)),
                               touch(s, kP1, EntityKind::Pickup, pickup, ticks(0.2))}));
    CHECK(playerAt(s, kP0).ammo == 1);
    CHECK(playerAt(s, kP1).ammo == 1);
    REQUIRE(sink.count == 1);
    CHECK(sink.events[0].player == kP1);
    CHECK_FALSE(s.pickups.isAlive(pickup));
  }
}

TEST_CASE("contacts with despawned or stale entities are skipped", "[sim][interaction]") {
  WorldState s = makePlayingWorld(1, 0b11);
  const EntityId banana = spawnBanana(s, kP1, Vec2::zero(), Vec2::zero());
  const Contact stale = touch(s, kP0, EntityKind::Boomerang, banana, Fixed::zero());
  s.boomerangs.despawn(banana);
  const EntityId reused = spawnBanana(s, kP1, Vec2::zero(), Vec2::zero());
  REQUIRE(reused.index == banana.index);  // same slot, newer generation
  const EventSink sink = resolve(s, contactsOf({stale}));
  CHECK(sink.count == 0);
  CHECK(playerAt(s, kP0).hp == 1);
}

TEST_CASE("handlers decline when their entities are gone", "[sim][interaction]") {
  WorldState s = makePlayingWorld(1, 0b11);
  const ContentDB& content = defaultContent();
  EventSink sink;
  const EntityId player = s.players.idAt(0);
  const Contact c{};
  CHECK_FALSE(catchBoomerang(s, content, player, EntityId::invalid(), c, sink));
  CHECK_FALSE(damagePlayer(s, content, player, EntityId::invalid(), c, sink));
  CHECK_FALSE(autoCatch(s, content, player, EntityId::invalid(), c, sink));
  CHECK_FALSE(takePickup(s, content, player, EntityId::invalid(), c, sink));
  CHECK(sink.count == 0);
}

TEST_CASE("empty contact list is a no-op", "[sim][interaction]") {
  WorldState s = makePlayingWorld(1, 0b11);
  const uint32_t before = checksum(s);
  const EventSink sink = resolve(s, Contacts{});
  CHECK(sink.count == 0);
  CHECK(checksum(s) == before);
}

TEST_CASE("interaction resolution is deterministic", "[sim][interaction]") {
  const auto run = [] {
    WorldState s = makePlayingWorld(11, 0b1111);
    Contacts cs;
    for (uint8_t i = 0; i < 4; ++i) {
      const PlayerId owner{static_cast<uint8_t>((i + 1) % 4)};
      const EntityId b = spawnBanana(s, owner, Vec2::zero(), Vec2::zero());
      for (uint8_t p = 0; p < 4; ++p) {
        cs.push(touch(s, PlayerId{p}, EntityKind::Boomerang, b,
                      Fixed::fromRatio((p * 7 + i * 3) % 5, 5)));
      }
    }
    givePerfectDash(s, PlayerId{2}, s.tick, 77);
    EventSink sink;
    interactionSystem(s, defaultContent(), cs, sink);
    return checksum(s);
  };
  CHECK(run() == run());
}

TEST_CASE("no contact resolves outside the Playing phase", "[sim][interaction]") {
  for (const MatchPhase phase :
       {MatchPhase::RoundStarting, MatchPhase::RoundEnded, MatchPhase::MatchEnded}) {
    WorldState s = makePlayingWorld(1, 0b11);
    const EntityId b = spawnBanana(s, kP1, Vec2::zero(), Vec2::zero());
    s.rules.phase = phase;
    const EventSink sink = resolve(s, contactsOf({touch(s, kP0, EntityKind::Boomerang, b, {})}));
    CHECK(sink.count == 0);
    CHECK(playerAt(s, kP0).hp == defaultContent().player.maxHp);
    CHECK(s.boomerangs.isAlive(b));
  }
}

// --- dash attack (row 25) ---------------------------------------------------------

TEST_CASE("a dasher holding ammo damages the player it touches", "[sim][interaction][dash]") {
  WorldState s = makePlayingWorld(7, 0b111);
  giveDash(s, kP1, s.tick.plus(-6), 0);  // the dasher is B in the listed contact
  const uint32_t pickupsBefore = s.pickups.size();
  const EventSink sink = resolve(s, contactsOf({playersTouch(s, kP0, kP1, ticks(0.4))}));

  REQUIRE(sink.count == 2);
  CHECK(sink.events[0].kind == EventKind::PlayerDashHit);
  CHECK(sink.events[0].player == kP0);
  CHECK(sink.events[0].entity == s.players.idAt(kP1.value));
  CHECK(sink.events[0].a == kP1.value);
  CHECK(sink.events[0].b == 0);
  CHECK(sink.events[1].kind == EventKind::PlayerEliminated);
  CHECK(sink.events[1].player == kP0);
  CHECK(sink.events[1].entity == s.players.idAt(kP0.value));
  CHECK(sink.events[1].a == kP1.value);
  CHECK(countEvents(sink, EventKind::PlayerHit) == 0);

  const Player& victim = playerAt(s, kP0);
  CHECK(victim.hp == 0);
  CHECK(victim.tags.has(Tag::HitStun));
  CHECK(victim.tags.has(Tag::Eliminated));
  CHECK(victim.hitStunTicks == defaultContent().player.hitStunTicks);
  CHECK(playerAt(s, kP1).ammo == 1);  // kept by default
  CHECK_FALSE(playerAt(s, kP1).tags.has(Tag::HitStun));
  CHECK(s.pickups.size() == pickupsBefore);
}

TEST_CASE("a dash attack declines without ammo and nothing else applies",
          "[sim][interaction][dash]") {
  WorldState s = makePlayingWorld(7, 0b11);
  giveDash(s, kP0, s.tick, 0);
  playerAt(s, kP0).ammo = 0;
  const uint32_t before = checksum(s);
  const EventSink sink = resolve(s, contactsOf({playersTouch(s, kP0, kP1, Fixed::zero())}));
  CHECK(sink.count == 0);
  CHECK(checksum(s) == before);
}

TEST_CASE("players touching without a dash do nothing", "[sim][interaction][dash]") {
  WorldState s = makePlayingWorld(7, 0b11);
  const uint32_t before = checksum(s);
  const EventSink sink = resolve(s, contactsOf({playersTouch(s, kP0, kP1, Fixed::zero())}));
  CHECK(sink.count == 0);
  CHECK(checksum(s) == before);
}

TEST_CASE("a dash attack respects HitStun, Invulnerable and Eliminated",
          "[sim][interaction][dash]") {
  WorldState s = makePlayingWorld(7, 0b11);
  giveDash(s, kP0, s.tick, 0);
  SECTION("victim in HitStun") {
    playerAt(s, kP1).tags.set(Tag::HitStun);
  }
  SECTION("victim Invulnerable") {
    playerAt(s, kP1).tags.set(Tag::Invulnerable);
  }
  SECTION("victim already eliminated this tick") {
    playerAt(s, kP1).tags.set(Tag::Eliminated);
  }
  SECTION("attacker in HitStun") {
    playerAt(s, kP0).tags.set(Tag::HitStun);
  }
  const EventSink sink = resolve(s, contactsOf({playersTouch(s, kP0, kP1, ticks(0.5))}));
  CHECK(sink.count == 0);
  CHECK(playerAt(s, kP1).hp == 1);
}

TEST_CASE("mutual dash attack: one hit, earlier press wins, then lower slot",
          "[sim][interaction][dash]") {
  WorldState s = makePlayingWorld(7, 0b11);
  PlayerId winner = kP0;
  SECTION("slot 1 pressed earlier in the same tick") {
    giveDash(s, kP0, s.tick.plus(-3), 200);
    giveDash(s, kP1, s.tick.plus(-3), 20);
    winner = kP1;
  }
  SECTION("slot 1 pressed a tick earlier, with a larger sub-tick") {
    giveDash(s, kP0, s.tick.plus(-2), 0);
    giveDash(s, kP1, s.tick.plus(-3), 255);
    winner = kP1;
  }
  SECTION("slot 0 pressed earlier") {
    giveDash(s, kP0, s.tick.plus(-5), 0);
    giveDash(s, kP1, s.tick.plus(-1), 0);
    winner = kP0;
  }
  SECTION("identical press: lower slot wins") {
    giveDash(s, kP0, s.tick.plus(-2), 77);
    giveDash(s, kP1, s.tick.plus(-2), 77);
    winner = kP0;
  }
  const PlayerId loser = winner == kP0 ? kP1 : kP0;
  const EventSink sink = resolve(s, contactsOf({playersTouch(s, kP0, kP1, ticks(0.25))}));
  REQUIRE(countEvents(sink, EventKind::PlayerDashHit) == 1);
  CHECK(firstEvent(sink, EventKind::PlayerDashHit).player == loser);
  CHECK(firstEvent(sink, EventKind::PlayerDashHit).a == winner.value);
  CHECK(playerAt(s, loser).tags.has(Tag::Eliminated));
  CHECK_FALSE(playerAt(s, winner).tags.has(Tag::HitStun));
  CHECK(playerAt(s, winner).hp == 1);
}

TEST_CASE("mutual dash with a tougher loser: the stunned loser cannot retaliate",
          "[sim][interaction][dash]") {
  ContentDB content = defaultContent();
  content.player.maxHp = 3;
  WorldState s = makePlayingWorld(7, 0b11, content);
  giveDash(s, kP0, s.tick.plus(-4), 0);
  giveDash(s, kP1, s.tick.plus(-2), 0);
  const EventSink sink = resolve(s, contactsOf({playersTouch(s, kP0, kP1, ticks(0.25))}), content);
  REQUIRE(countEvents(sink, EventKind::PlayerDashHit) == 1);
  CHECK(firstEvent(sink, EventKind::PlayerDashHit).player == kP1);
  CHECK(firstEvent(sink, EventKind::PlayerDashHit).b == 2);
  CHECK(countEvents(sink, EventKind::PlayerEliminated) == 0);
  CHECK(playerAt(s, kP0).hp == 3);
  CHECK(playerAt(s, kP1).hp == 2);
  CHECK(playerAt(s, kP1).tags.has(Tag::HitStun));
}

TEST_CASE("an earlier banana hit stops the dasher's attack in the same tick",
          "[sim][interaction][dash]") {
  WorldState s = makePlayingWorld(7, 0b111);
  giveDash(s, kP0, s.tick.plus(-6), 0);
  const EntityId banana = spawnBanana(s, kP2, Vec2::zero(), Vec2::zero());
  const EventSink sink =
      resolve(s, contactsOf({playersTouch(s, kP0, kP1, ticks(0.6)),
                             touch(s, kP0, EntityKind::Boomerang, banana, ticks(0.3))}));
  CHECK(countEvents(sink, EventKind::PlayerHit) == 1);
  CHECK(countEvents(sink, EventKind::PlayerDashHit) == 0);
  CHECK(playerAt(s, kP1).hp == 1);
}

TEST_CASE("dash attack content switches: damage, minAmmo, consumesAmmo",
          "[sim][interaction][dash]") {
  ContentDB content = defaultContent();
  content.player.maxHp = 3;
  content.player.ammoMax = 2;
  SECTION("consumesAmmo spends a banana per hit") {
    content.dashAttack.consumesAmmo = 1;
    content.dashAttack.damage = 2;
    WorldState s = makePlayingWorld(7, 0b11, content);
    playerAt(s, kP0).ammo = 2;
    giveDash(s, kP0, s.tick, 0);
    const EventSink sink = resolve(s, contactsOf({playersTouch(s, kP0, kP1, ticks(0.5))}), content);
    CHECK(countEvents(sink, EventKind::PlayerDashHit) == 1);
    CHECK(playerAt(s, kP0).ammo == 1);
    CHECK(playerAt(s, kP1).hp == 1);
  }
  SECTION("minAmmo above what the dasher holds declines") {
    content.dashAttack.minAmmo = 2;
    WorldState s = makePlayingWorld(7, 0b11, content);
    playerAt(s, kP0).ammo = 1;
    giveDash(s, kP0, s.tick, 0);
    const EventSink sink = resolve(s, contactsOf({playersTouch(s, kP0, kP1, ticks(0.5))}), content);
    CHECK(sink.count == 0);
  }
  SECTION("minAmmo = 0 makes every dash an attack") {
    content.dashAttack.minAmmo = 0;
    content.dashAttack.consumesAmmo = 1;  // nothing to spend: ammo stays at 0
    WorldState s = makePlayingWorld(7, 0b11, content);
    playerAt(s, kP0).ammo = 0;
    giveDash(s, kP0, s.tick, 0);
    const EventSink sink = resolve(s, contactsOf({playersTouch(s, kP0, kP1, ticks(0.5))}), content);
    CHECK(countEvents(sink, EventKind::PlayerDashHit) == 1);
    CHECK(playerAt(s, kP0).ammo == 0);
  }
}
