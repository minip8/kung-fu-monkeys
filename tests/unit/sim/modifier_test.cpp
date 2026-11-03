// ModifierSystem (contract step 8, architecture §5.6, §5.8): power-up
// expiry, derived stats, HP clamp, ability overrides, granted tags, HitStun
// countdown.
#include "kfm/sim/systems.hpp"
#include "sim_test_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

const PlayerId kP0{0};

// A fresh one-grant power-up in `c` given to `p` with `ticksLeft`.
void addMod(ContentDB& c, Player& p, StatKind stat, Fixed mul, Fixed add, uint16_t ticksLeft) {
  const uint8_t index =
      addPowerUp(c, "m" + std::to_string(c.powerUpCount), ticksLeft, {statGrant(stat, mul, add)});
  givePowerUp(p, index, ticksLeft);
}

Player& freshPlayer(WorldState& s) {
  return playerAt(s, kP0);
}

}  // namespace

TEST_CASE("no modifiers gives the content base stats", "[sim][modifier]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(1, 0b1);
  Player& p = freshPlayer(s);
  p.stats = Stats{};
  p.hp = 1;
  modifierSystem(s, content);
  CHECK(p.stats.moveSpeed == content.player.moveSpeed);
  CHECK(p.stats.maxHp == content.player.maxHp);
  CHECK(p.stats.ammoMax == content.player.ammoMax);
}

TEST_CASE("stats are base x product of mul + sum of add, integers add-only", "[sim][modifier]") {
  ContentDB content = defaultContent();
  content.player.moveSpeed = Fixed::fromInt(2);
  content.player.maxHp = 1;
  content.player.ammoMax = 1;
  WorldState s = makePlayingWorld(2, 0b1, content);
  Player& p = freshPlayer(s);
  const Fixed quarter = Fixed::fromRatio(1, 4);
  addMod(content, p, StatKind::MoveSpeed, Fixed::fromInt(3), Fixed::zero(), 100);
  addMod(content, p, StatKind::MoveSpeed, Fixed::half(), quarter, 100);
  addMod(content, p, StatKind::MoveSpeed, Fixed::one(), quarter, 100);
  // mul on an integer stat is ignored (content validation rejects it).
  addMod(content, p, StatKind::MaxHp, Fixed::fromInt(10), Fixed::fromInt(2), 100);
  addMod(content, p, StatKind::AmmoMax, Fixed::one(), Fixed::fromInt(2), 100);
  modifierSystem(s, content);
  // 2 × (3 × 0.5 × 1) + (0.25 + 0.25) = 3.5
  CHECK(p.stats.moveSpeed == Fixed::fromRatio(7, 2));
  CHECK(p.stats.maxHp == 3);
  CHECK(p.stats.ammoMax == 3);
}

TEST_CASE("a power-up with several grants applies all of them", "[sim][modifier]") {
  ContentDB content = defaultContent();
  WorldState s = makePlayingWorld(2, 0b1, content);
  Player& p = freshPlayer(s);
  const uint8_t both = addPowerUp(content, "both", 10,
                                  {statGrant(StatKind::MoveSpeed, Fixed::fromInt(2), Fixed::zero()),
                                   statGrant(StatKind::MaxHp, Fixed::one(), Fixed::fromInt(1))});
  givePowerUp(p, both, 10);
  modifierSystem(s, content);
  CHECK(p.stats.moveSpeed == content.player.moveSpeed * Fixed::fromInt(2));
  CHECK(p.stats.maxHp == content.player.maxHp + 1);
}

TEST_CASE("moveSpeed never goes negative and integer stats stay in range", "[sim][modifier]") {
  ContentDB content = defaultContent();
  WorldState s = makePlayingWorld(3, 0b1);
  Player& p = freshPlayer(s);
  addMod(content, p, StatKind::MoveSpeed, Fixed::one(), -Fixed::fromInt(5), 10);
  addMod(content, p, StatKind::MaxHp, Fixed::one(), -Fixed::fromInt(5), 10);
  addMod(content, p, StatKind::AmmoMax, Fixed::one(), -Fixed::fromInt(5), 10);
  modifierSystem(s, content);
  CHECK(p.stats.moveSpeed == Fixed::zero());
  CHECK(p.stats.maxHp == 1);
  CHECK(p.stats.ammoMax == 0);
  CHECK(p.ammo == 0);  // clamped to the new ammoMax
}

TEST_CASE("maxHp up raises hp by the same amount; down clamps but never kills", "[sim][modifier]") {
  ContentDB content = defaultContent();
  REQUIRE(content.player.maxHp == 1);
  WorldState s = makePlayingWorld(4, 0b1);
  Player& p = freshPlayer(s);
  REQUIRE(p.hp == 1);
  REQUIRE(p.stats.maxHp == 1);

  addMod(content, p, StatKind::MaxHp, Fixed::one(), Fixed::fromInt(2), 3);  // armour for 3 ticks
  modifierSystem(s, content);
  CHECK(p.stats.maxHp == 3);
  CHECK(p.hp == 3);

  p.hp = 2;                    // took a hit while armoured
  modifierSystem(s, content);  // armour tick 2: unchanged
  CHECK(p.hp == 2);
  modifierSystem(s, content);  // tick 3: the last one it applies on
  CHECK(p.stats.maxHp == 3);
  CHECK(p.modifiers.count == 1);
  CHECK(p.modifiers.items[0].ticksLeft == 0);
  modifierSystem(s, content);  // armour gone: hp = max(1, min(2, 1))
  CHECK(p.modifiers.count == 0);
  CHECK(p.stats.maxHp == 1);
  CHECK(p.hp == 1);

  // Down while already at 1: losing a modifier never eliminates.
  addMod(content, p, StatKind::MaxHp, Fixed::one(), Fixed::fromInt(4), 1);
  modifierSystem(s, content);
  CHECK(p.hp == 5);
  p.hp = 1;
  modifierSystem(s, content);
  CHECK(p.stats.maxHp == 1);
  CHECK(p.hp == 1);
}

TEST_CASE("an eliminated player at hp 0 is not revived by a stat change", "[sim][modifier]") {
  ContentDB content = defaultContent();
  WorldState s = makePlayingWorld(5, 0b1);
  Player& p = freshPlayer(s);
  addMod(content, p, StatKind::MaxHp, Fixed::one(), Fixed::fromInt(2), 2);
  modifierSystem(s, content);
  REQUIRE(p.hp == 3);
  p.hp = 0;  // killed this tick by InteractionSystem
  modifierSystem(s, content);
  CHECK(p.hp == 0);
  p.modifiers = ModifierList{};
  p.stats.maxHp = 1;
  addMod(content, p, StatKind::MaxHp, Fixed::one(), Fixed::one(), 1);
  modifierSystem(s, content);  // maxHp rises
  CHECK(p.hp == 0);
}

TEST_CASE("expired modifiers are dropped, compacting in slot order", "[sim][modifier]") {
  ContentDB content = defaultContent();
  WorldState s = makePlayingWorld(6, 0b1);
  Player& p = freshPlayer(s);
  const Fixed q = Fixed::fromRatio(1, 64);
  addMod(content, p, StatKind::MoveSpeed, Fixed::one(), q * 1, 2);  // power-up 0
  addMod(content, p, StatKind::MoveSpeed, Fixed::one(), q * 2, 1);  // 1
  addMod(content, p, StatKind::MoveSpeed, Fixed::one(), q * 3, 3);  // 2
  addMod(content, p, StatKind::MoveSpeed, Fixed::one(), q * 4, 1);  // 3
  addMod(content, p, StatKind::MoveSpeed, Fixed::one(), q * 5, 0);  // 4: already expired

  modifierSystem(s, content);  // 4 dropped, 0-3 applied, then counted down
  CHECK(p.stats.moveSpeed == content.player.moveSpeed + q * 10);
  REQUIRE(p.modifiers.count == 4);
  CHECK(p.modifiers.items[0].ticksLeft == 1);
  CHECK(p.modifiers.items[1].ticksLeft == 0);
  CHECK(p.modifiers.items[2].ticksLeft == 2);
  CHECK(p.modifiers.items[3].ticksLeft == 0);
  CHECK(p.modifiers.items[4].ticksLeft == 0);  // slots past count are canonical
  CHECK(p.modifiers.items[4].powerUp == 0);

  modifierSystem(s, content);  // 1 and 3 dropped, the list compacts
  CHECK(p.stats.moveSpeed == content.player.moveSpeed + q * 4);
  REQUIRE(p.modifiers.count == 2);
  CHECK(p.modifiers.items[0].powerUp == 0);
  CHECK(p.modifiers.items[1].powerUp == 2);
  for (uint8_t i = 2; i < kMaxModifiers; ++i) {
    CHECK(p.modifiers.items[i].ticksLeft == 0);
    CHECK(p.modifiers.items[i].powerUp == 0);
  }

  modifierSystem(s, content);
  CHECK(p.stats.moveSpeed == content.player.moveSpeed + q * 3);
  REQUIRE(p.modifiers.count == 1);
  CHECK(p.modifiers.items[0].powerUp == 2);

  modifierSystem(s, content);
  CHECK(p.stats.moveSpeed == content.player.moveSpeed);
  CHECK(p.modifiers.count == 0);
}

TEST_CASE("an ability override swaps the slot while idle and restores it on expiry",
          "[sim][modifier]") {
  ContentDB content = defaultContent();
  // A faster throw def at index 3 (kMaxAbilityDefs is 4).
  const uint8_t baseThrow = content.player.abilities[kAbilitySlotThrow];
  AbilityDef fast = content.abilities[baseThrow];
  REQUIRE(fast.kind == AbilityKind::Throw);
  fast.name = ContentName::of("throw_fast");
  fast.cooldownTicks = 5;
  const uint8_t fastIndex = content.abilityCount++;
  content.abilities[fastIndex] = fast;
  const uint8_t hands =
      addPowerUp(content, "fast_hands", 3, {abilityGrant(kAbilitySlotThrow, fastIndex)});

  WorldState s = makePlayingWorld(9, 0b1, content);
  Player& p = freshPlayer(s);
  AbilityState& thr = p.abilities[kAbilitySlotThrow];
  REQUIRE(thr.defIndex == baseThrow);

  SECTION("an idle slot switches at once, its cooldown capped at the new def's") {
    thr.cooldownTicks = 15;
    givePowerUp(p, hands, 3);
    modifierSystem(s, content);
    CHECK(thr.defIndex == fastIndex);
    CHECK(thr.cooldownTicks == 5);
    CHECK(p.abilities[kAbilitySlotDash].defIndex == content.player.abilities[kAbilitySlotDash]);
    modifierSystem(s, content);
    modifierSystem(s, content);
    CHECK(thr.defIndex == fastIndex);  // third and last tick
    thr.cooldownTicks = 4;
    modifierSystem(s, content);  // expired: the loadout's def again, cooldown kept
    CHECK(thr.defIndex == baseThrow);
    CHECK(thr.cooldownTicks == 4);
  }
  SECTION("a slot mid-activation finishes on its def and switches once idle") {
    thr.phase = AbilityPhase::Recovery;
    givePowerUp(p, hands, 3);
    modifierSystem(s, content);
    CHECK(thr.defIndex == baseThrow);
    thr.phase = AbilityPhase::Idle;
    modifierSystem(s, content);
    CHECK(thr.defIndex == fastIndex);
  }
  SECTION("the last override in slot order wins") {
    const uint8_t back =
        addPowerUp(content, "slow_hands", 3, {abilityGrant(kAbilitySlotThrow, baseThrow)});
    givePowerUp(p, hands, 3);
    givePowerUp(p, back, 3);
    modifierSystem(s, content);
    CHECK(thr.defIndex == baseThrow);
  }
}

TEST_CASE("granted tags follow the live entries", "[sim][modifier]") {
  ContentDB content = defaultContent();
  const uint8_t shield = addPowerUp(content, "shield", 2, {tagGrant(Tag::Shielded)});
  WorldState s = makePlayingWorld(10, 0b1, content);
  Player& p = freshPlayer(s);
  p.tags.set(Tag::Dashing);  // not a power-up's to clear
  givePowerUp(p, shield, 2);
  modifierSystem(s, content);
  CHECK(p.tags.has(Tag::Shielded));
  modifierSystem(s, content);
  CHECK(p.tags.has(Tag::Shielded));
  modifierSystem(s, content);
  CHECK_FALSE(p.tags.has(Tag::Shielded));
  CHECK(p.tags.has(Tag::Dashing));
}

TEST_CASE("hitStunTicks counts down and clears HitStun at zero", "[sim][modifier]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(7, 0b1);
  Player& p = freshPlayer(s);
  p.hitStunTicks = 3;
  p.tags.set(Tag::HitStun);
  p.tags.set(Tag::Dashing);  // unrelated tags are untouched
  modifierSystem(s, content);
  CHECK(p.hitStunTicks == 2);
  CHECK(p.tags.has(Tag::HitStun));
  modifierSystem(s, content);
  CHECK(p.hitStunTicks == 1);
  CHECK(p.tags.has(Tag::HitStun));
  modifierSystem(s, content);
  CHECK(p.hitStunTicks == 0);
  CHECK_FALSE(p.tags.has(Tag::HitStun));
  CHECK(p.tags.has(Tag::Dashing));
  modifierSystem(s, content);  // stays at zero
  CHECK(p.hitStunTicks == 0);

  // A HitStun tag with no timer behind it does not outlive the tick.
  p.tags.set(Tag::HitStun);
  modifierSystem(s, content);
  CHECK_FALSE(p.tags.has(Tag::HitStun));
}

TEST_CASE("ammo is clamped to ammoMax", "[sim][modifier]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(8, 0b1);
  Player& p = freshPlayer(s);
  p.ammo = 7;
  modifierSystem(s, content);
  CHECK(p.ammo == content.player.ammoMax);
}
