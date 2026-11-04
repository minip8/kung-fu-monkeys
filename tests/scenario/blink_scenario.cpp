// Blink end to end (architecture §5.5, v0.21): a teleport dash in the third
// slot, fired by the ability button. Activation, the startup telegraph, the
// jump (walls stop it, nothing on the path is touched), cooldown, and the
// two ways to reach it: a loadout (MatchConfig::loadout) and a power-up override of a slot.
#include "loadout_util.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

const PlayerId kP0{0};
const PlayerId kP1{1};
constexpr uint8_t kAbility = buttonBit(Button::Ability1);

Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

// Player 0 at `from` facing +x, player 1 far away in a corner.
WorldState blinkWorld(const ContentDB& c, Vec2 from) {
  WorldState s = makePlayingWorld(5, 0b11, c);
  placePlayer(s, kP0, from);
  placePlayer(s, kP1, at(-12, -7));
  return s;
}

}  // namespace

TEST_CASE("blink: the ability button jumps speed x activeTicks after the startup",
          "[scenario][blink]") {
  const ContentDB c = withExtraSlot(blinkDef());
  const AbilityDef& def = blinkDef();
  WorldState s = blinkWorld(c, at(0, 0));
  EventLog log;
  // The dash button still dashes; the blink lives on the ability button.
  REQUIRE(c.player.abilities[kAbilitySlotExtra] != 0);
  const Tick pressTick = s.tick;
  pressOnce(s, kP0, kAbility, c, log);
  const AbilityState& slot = playerAt(s, kP0).abilities[kAbilitySlotExtra];
  CHECK(slot.phase == AbilityPhase::Startup);
  CHECK(slot.cooldownTicks == def.cooldownTicks);
  CHECK(countEvents(log, EventKind::PlayerDashed) == 0);
  // Startup: no jump yet, and no tags (a telegraphed blinker is no dasher).
  idle(s, def.startupTicks - 1, c, log);
  CHECK(playerAt(s, kP0).transform.pos == at(0, 0));
  CHECK(playerAt(s, kP0).tags == Tags{});
  CHECK(countEvents(log, EventKind::PlayerBlinked) == 0);
  // The Startup -> Active tick: 10 ticks x 0.5 units along +x.
  idle(s, 1, c, log);
  const SimEvent& blinked = firstEvent(log, EventKind::PlayerBlinked);
  CHECK(blinked.tick.minus(pressTick) == def.startupTicks);
  CHECK(blinked.player == kP0);
  CHECK(static_cast<int32_t>(blinked.a) == 0);  // origin x
  CHECK(static_cast<int32_t>(blinked.b) == 0);  // origin y
  const Vec2 landed = playerAt(s, kP0).transform.pos;
  CHECK(landed == Vec2{def.speed * Fixed::fromInt(def.activeTicks), Fixed::zero()});
  CHECK(landed.x > Fixed::fromRatio(49, 10));
  // The slot runs Active, then Recovery, then goes idle; the cooldown holds.
  idle(s, def.activeTicks + def.recoveryTicks, c, log);
  CHECK(playerAt(s, kP0).abilities[kAbilitySlotExtra].phase == AbilityPhase::Idle);
  CHECK(countEvents(log, EventKind::PlayerBlinked) == 1);
}

TEST_CASE("blink: walls and the arena box stop the jump", "[scenario][blink]") {
  const ContentDB c = withExtraSlot(blinkDef());
  WorldState s = blinkWorld(c, at(13, 0));
  EventLog log;
  pressOnce(s, kP0, kAbility, c, log);
  idle(s, blinkDef().startupTicks, c, log);
  REQUIRE(countEvents(log, EventKind::PlayerBlinked) == 1);
  // 16 - 0.5: the box minus the player's radius, not 13 + 5.
  CHECK(playerAt(s, kP0).transform.pos == Vec2{Fixed::fromRatio(31, 2), Fixed::zero()});
}

TEST_CASE("blink: nothing on the path is touched, but the startup can be hit",
          "[scenario][blink]") {
  const ContentDB c = withExtraSlot(blinkDef());
  SECTION("a banana hovering on the path is jumped over") {
    WorldState s = blinkWorld(c, at(0, 0));
    // A slow banana of player 1 drifting up across x = 2.5, the middle of the path.
    spawnBanana(s, kP1, Vec2{Fixed::fromRatio(5, 2), Fixed::fromRatio(-4, 5)},
                Vec2{Fixed::zero(), Fixed::fromRatio(1, 8)});
    EventLog log;
    pressOnce(s, kP0, kAbility, c, log);
    idle(s, blinkDef().startupTicks + 4, c, log);
    REQUIRE(countEvents(log, EventKind::PlayerBlinked) == 1);
    CHECK(countEvents(log, EventKind::PlayerHit) == 0);
    CHECK(playerAt(s, kP0).hp == c.player.maxHp);
  }
  SECTION("a banana arriving during the telegraph hits") {
    WorldState s = blinkWorld(c, at(0, 0));
    spawnBanana(s, kP1, Vec2{Fixed::fromRatio(3, 2), Fixed::zero()},
                Vec2{-c.boomerang.speed, Fixed::zero()});
    EventLog log;
    pressOnce(s, kP0, kAbility, c, log);
    idle(s, blinkDef().startupTicks, c, log);
    CHECK(countEvents(log, EventKind::PlayerHit) == 1);
    CHECK(countEvents(log, EventKind::PlayerBlinked) == 0);  // eliminated before the jump
  }
}

TEST_CASE("blink: the cooldown is counted from the press", "[scenario][blink]") {
  const ContentDB c = withExtraSlot(blinkDef());
  WorldState s = blinkWorld(c, at(-10, 0));
  EventLog log;
  pressOnce(s, kP0, kAbility, c, log);
  idle(s, 29, c, log);
  pressOnce(s, kP0, kAbility, c, log);  // tick 30: still cooling down
  idle(s, 59, c, log);
  CHECK(countEvents(log, EventKind::PlayerBlinked) == 1);
  pressOnce(s, kP0, kAbility, c, log);  // tick 90: ready again
  idle(s, blinkDef().startupTicks, c, log);
  CHECK(countEvents(log, EventKind::PlayerBlinked) == 2);
}

TEST_CASE("blink: reachable through a loadout and a power-up override", "[scenario][blink]") {
  uint8_t blink = 0;
  ContentDB c = withAbility(blinkDef(), blink);
  SECTION("a loadout puts it on the ability button for the matches that pick it") {
    const uint8_t loadout = addLoadout(c, "blink", blink);
    const MatchContent blinkMatch = withLoadoutMatch(c, loadout);
    WorldState s = playingWorld(5, 0b11, blinkMatch);
    CHECK(playerAt(s, kP0).abilities[kAbilitySlotExtra].defIndex == blink);
    EventLog log;
    pressOnce(s, kP0, kAbility, blinkMatch, log);
    idle(s, blinkDef().startupTicks, blinkMatch, log);
    CHECK(countEvents(log, EventKind::PlayerBlinked) == 1);
    // The default loadout keeps the base kit: the ability button does nothing.
    const MatchContent rounds(c, 0, 0);
    WorldState r = playingWorld(5, 0b11, rounds);
    CHECK(playerAt(r, kP0).abilities[kAbilitySlotExtra].defIndex == 0);
    EventLog none;
    pressOnce(r, kP0, kAbility, rounds, none);
    idle(r, blinkDef().startupTicks, rounds, none);
    CHECK(countEvents(none, EventKind::PlayerBlinked) == 0);
  }
  SECTION("an override of the dash slot turns the dash button into a blink") {
    const uint8_t charm =
        addPowerUp(c, "blink_charm", 600, {abilityGrant(kAbilitySlotDash, blink)});
    WorldState s = blinkWorld(c, at(0, 0));
    givePowerUp(playerAt(s, kP0), charm, 600);
    EventLog log;
    idle(s, 1, c, log);  // ModifierSystem swaps the idle slot
    REQUIRE(playerAt(s, kP0).abilities[kAbilitySlotDash].defIndex == blink);
    pressOnce(s, kP0, buttonBit(Button::Dash), c, log);
    idle(s, blinkDef().startupTicks, c, log);
    CHECK(countEvents(log, EventKind::PlayerBlinked) == 1);
    CHECK(countEvents(log, EventKind::PlayerDashed) == 0);
  }
}
