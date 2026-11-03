// Parry end to end (architecture §5.5, §5.6, §5.11, v0.21): a timed window on
// the ability button. Row 15 reflects a non-own banana inside the exact
// window (owner swap, back at the thrower, fresh outbound); outside it the
// row declines and the banana hits (the fall-through); the perfect catch
// (row 10) wins a tie.
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

// Parrier P0 at the centre, thrower P1 at (8, 0), P2 out of the way. P1's
// banana starts at (6, 0) flying at P0: it touches P0 about 19.3 ticks later.
struct Duel {
  WorldState s;
  EntityId banana;
  EventLog log;
};

Duel duel(const ContentDB& c) {
  Duel d{makePlayingWorld(13, 0b111, c), {}, {}};
  placePlayer(d.s, kP0, at(0, 0));
  placePlayer(d.s, kP1, at(8, 0));
  placePlayer(d.s, kP2, at(-12, 7));
  d.banana = spawnBanana(d.s, kP1, at(6, 0), Vec2{-c.boomerang.speed, Fixed::zero()});
  return d;
}

// Idles `before` ticks, presses `buttons` once, idles until `total` ticks ran.
void pressAt(Duel& d, const ContentDB& c, int before, uint8_t buttons, int total = 40) {
  idle(d.s, before, c, d.log);
  pressOnce(d.s, kP0, buttons, c, d.log, 0, 0);
  idle(d.s, total - before - 1, c, d.log);
}

}  // namespace

TEST_CASE("parry: a timed parry sends the banana back as the parrier's", "[scenario][parry]") {
  const ContentDB c = withExtraSlot(parryDef());
  Duel d = duel(c);
  // The window [press + 2, press + 10) covers the touch at ~19.3.
  idle(d.s, 12, c, d.log);
  pressOnce(d.s, kP0, kAbility, c, d.log);
  CHECK(countEvents(d.log, EventKind::PlayerParryStarted) == 1);
  idle(d.s, 9, c, d.log);
  const SimEvent& parried = firstEvent(d.log, EventKind::BoomerangParried);
  CHECK(parried.player == kP0);
  CHECK(parried.entity == d.banana);
  CHECK(parried.a == kP1.value);
  const Boomerang* b = d.s.boomerangs.get(d.banana);
  REQUIRE(b != nullptr);
  CHECK(b->owner == kP0);
  CHECK(b->phase == BoomerangPhase::Outbound);
  CHECK(b->transform.vel.x > Fixed::zero());  // back toward P1
  CHECK(countEvents(d.log, EventKind::PlayerHit) == 0);
  CHECK(playerAt(d.s, kP0).hp == c.player.maxHp);
  // It flies back and hits its thrower, credited to the parrier.
  idle(d.s, 40, c, d.log);
  const SimEvent& hit = firstEvent(d.log, EventKind::PlayerHit);
  CHECK(hit.player == kP1);
  CHECK(hit.a == kP0.value);
}

TEST_CASE("parry: too early or too late, the banana hits", "[scenario][parry]") {
  const ContentDB c = withExtraSlot(parryDef());
  SECTION("too early: the window closed before the touch") {
    Duel d = duel(c);
    pressAt(d, c, 0, kAbility);
    CHECK(countEvents(d.log, EventKind::BoomerangParried) == 0);
    CHECK(firstEvent(d.log, EventKind::PlayerHit).player == kP0);
  }
  SECTION("too late: the touch came during the startup") {
    Duel d = duel(c);
    pressAt(d, c, 18, kAbility);
    CHECK(countEvents(d.log, EventKind::BoomerangParried) == 0);
    CHECK(firstEvent(d.log, EventKind::PlayerHit).player == kP0);
  }
}

TEST_CASE("parry: the window is judged to the press sub-tick", "[scenario][parry]") {
  const ContentDB c = withExtraSlot(parryDef());
  // The parry pressed half a tick into tick T opens at T + 2.5: on tick T + 2
  // a touch at tHit 0.25 is early (damage), one at 0.75 is parried.
  const auto resolve = [&](Fixed tHit) {
    WorldState s = makePlayingWorld(13, 0b111, c);
    placePlayer(s, kP0, at(0, 0));
    Player& p = playerAt(s, kP0);
    AbilityState& slot = p.abilities[kAbilitySlotExtra];
    slot.phase = AbilityPhase::Active;
    slot.phaseTicks = 0;
    slot.activatedTick = Tick{s.tick.value - 2};
    slot.activatedSubTick = 128;
    p.tags.set(Tag::Parrying);
    const EntityId banana = spawnBanana(s, kP1, Vec2{Fixed::fromRatio(17, 20), Fixed::zero()},
                                        Vec2{-c.boomerang.speed, Fixed::zero()});
    Contact contact;
    contact.kindA = EntityKind::Player;
    contact.a = s.players.idAt(kP0.value);
    contact.kindB = EntityKind::Boomerang;
    contact.b = banana;
    contact.tHit = tHit;
    contact.point = Vec2{Fixed::fromRatio(17, 20), Fixed::zero()};
    contact.normal = Vec2{Fixed::one(), Fixed::zero()};
    EventLog log;
    EventSink sink;
    interactionResolveContact(s, c, contact, sink);
    log.insert(log.end(), sink.begin(), sink.end());
    return log;
  };
  const EventLog early = resolve(Fixed::fromRatio(1, 4));
  CHECK(countEvents(early, EventKind::BoomerangParried) == 0);
  CHECK(countEvents(early, EventKind::PlayerHit) == 1);
  const EventLog inside = resolve(Fixed::fromRatio(3, 4));
  CHECK(countEvents(inside, EventKind::BoomerangParried) == 1);
  CHECK(countEvents(inside, EventKind::PlayerHit) == 0);
}

TEST_CASE("parry: the perfect catch wins a tie, the parry takes it at full ammo",
          "[scenario][parry]") {
  const ContentDB c = withExtraSlot(parryDef());
  // Dash (window [press, press + 4)) at the banana and parry (window [press +
  // 2, press + 10)) pressed together 12 ticks in: dash and banana close at
  // 0.63 units a tick, so they touch about 3 ticks after the press, in both.
  const uint8_t both = buttonBit(Button::Dash) | kAbility;
  SECTION("room in hand: caught") {
    Duel d = duel(c);
    playerAt(d.s, kP0).ammo = 0;
    pressAt(d, c, 12, both, 20);
    CHECK(countEvents(d.log, EventKind::BoomerangCaught) == 1);
    CHECK(countEvents(d.log, EventKind::BoomerangParried) == 0);
  }
  SECTION("full ammo: the catch declines, the parry reflects") {
    Duel d = duel(c);
    REQUIRE(playerAt(d.s, kP0).ammo == playerAt(d.s, kP0).stats.ammoMax);
    pressAt(d, c, 12, both, 20);
    CHECK(countEvents(d.log, EventKind::BoomerangCaught) == 0);
    CHECK(countEvents(d.log, EventKind::BoomerangParried) == 1);
    CHECK(countEvents(d.log, EventKind::PlayerHit) == 0);
  }
}

TEST_CASE("parry: an own banana is not parried; the cooldown holds", "[scenario][parry]") {
  const ContentDB c = withExtraSlot(parryDef());
  WorldState s = makePlayingWorld(13, 0b111, c);
  placePlayer(s, kP0, at(0, 0));
  placePlayer(s, kP1, at(8, 0));
  placePlayer(s, kP2, at(-12, 7));
  playerAt(s, kP0).ammo = 0;
  // P0's own banana returning to it: auto-caught (row 40) during the parry.
  const EntityId own = spawnBanana(s, kP0, at(2, 0), Vec2{-c.boomerang.speed, Fixed::zero()},
                                   BoomerangPhase::Returning);
  s.boomerangs.items[own.index].transform.facing = Angle::fromBam(32768);  // heading -x
  EventLog log;
  pressOnce(s, kP0, kAbility, c, log);
  idle(s, 8, c, log);
  CHECK(countEvents(log, EventKind::BoomerangParried) == 0);
  CHECK(countEvents(log, EventKind::BoomerangCaught) == 1);
  // A second press inside the cooldown starts nothing.
  pressOnce(s, kP0, kAbility, c, log);
  CHECK(countEvents(log, EventKind::PlayerParryStarted) == 1);
  idle(s, 60, c, log);
  pressOnce(s, kP0, kAbility, c, log);
  CHECK(countEvents(log, EventKind::PlayerParryStarted) == 2);
}
