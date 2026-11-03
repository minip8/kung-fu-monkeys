// ModifierSystem (contract step 8, architecture §5.6, §5.8): derived stats,
// HP clamp, modifier expiry, HitStun countdown.
#include "kfm/sim/systems.hpp"
#include "sim_test_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

const PlayerId kP0{0};

StatModifier mod(StatKind stat, Fixed mul, Fixed add, uint16_t ticksLeft) {
  StatModifier m;
  m.stat = stat;
  m.mul = mul;
  m.add = add;
  m.ticksLeft = ticksLeft;
  return m;
}

void addMod(Player& p, const StatModifier& m) {
  REQUIRE(p.modifiers.count < kMaxModifiers);
  p.modifiers.items[p.modifiers.count++] = m;
}

Player& freshPlayer(WorldState& s) {
  return *s.playerBySlot(kP0);
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
  addMod(p, mod(StatKind::MoveSpeed, Fixed::fromInt(3), Fixed::zero(), 100));
  addMod(p, mod(StatKind::MoveSpeed, Fixed::half(), quarter, 100));
  addMod(p, mod(StatKind::MoveSpeed, Fixed::one(), quarter, 100));
  // mul on an integer stat is ignored (content validation rejects it later).
  addMod(p, mod(StatKind::MaxHp, Fixed::fromInt(10), Fixed::fromInt(2), 100));
  addMod(p, mod(StatKind::AmmoMax, Fixed::one(), Fixed::fromInt(2), 100));
  modifierSystem(s, content);
  // 2 × (3 × 0.5 × 1) + (0.25 + 0.25) = 3.5
  CHECK(p.stats.moveSpeed == Fixed::fromRatio(7, 2));
  CHECK(p.stats.maxHp == 3);
  CHECK(p.stats.ammoMax == 3);
}

TEST_CASE("moveSpeed never goes negative and integer stats stay in range", "[sim][modifier]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(3, 0b1);
  Player& p = freshPlayer(s);
  addMod(p, mod(StatKind::MoveSpeed, Fixed::one(), -Fixed::fromInt(5), 10));
  addMod(p, mod(StatKind::MaxHp, Fixed::one(), -Fixed::fromInt(5), 10));
  addMod(p, mod(StatKind::AmmoMax, Fixed::one(), -Fixed::fromInt(5), 10));
  modifierSystem(s, content);
  CHECK(p.stats.moveSpeed == Fixed::zero());
  CHECK(p.stats.maxHp == 1);
  CHECK(p.stats.ammoMax == 0);
  CHECK(p.ammo == 0);  // clamped to the new ammoMax
}

TEST_CASE("maxHp up raises hp by the same amount; down clamps but never kills", "[sim][modifier]") {
  const ContentDB& content = defaultContent();
  REQUIRE(content.player.maxHp == 1);
  WorldState s = makePlayingWorld(4, 0b1);
  Player& p = freshPlayer(s);
  REQUIRE(p.hp == 1);
  REQUIRE(p.stats.maxHp == 1);

  addMod(p, mod(StatKind::MaxHp, Fixed::one(), Fixed::fromInt(2), 3));  // armour for 3 ticks
  modifierSystem(s, content);
  CHECK(p.stats.maxHp == 3);
  CHECK(p.hp == 3);

  p.hp = 2;                    // took a hit while armoured
  modifierSystem(s, content);  // armour tick 2: unchanged
  CHECK(p.hp == 2);
  modifierSystem(s, content);  // tick 3: still applies, then expires
  CHECK(p.stats.maxHp == 3);
  CHECK(p.modifiers.count == 0);
  modifierSystem(s, content);  // armour gone: hp = max(1, min(2, 1))
  CHECK(p.stats.maxHp == 1);
  CHECK(p.hp == 1);

  // Down while already at 1: losing a modifier never eliminates.
  addMod(p, mod(StatKind::MaxHp, Fixed::one(), Fixed::fromInt(4), 1));
  modifierSystem(s, content);
  CHECK(p.hp == 5);
  p.hp = 1;
  modifierSystem(s, content);
  CHECK(p.stats.maxHp == 1);
  CHECK(p.hp == 1);
}

TEST_CASE("an eliminated player at hp 0 is not revived by a stat change", "[sim][modifier]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(5, 0b1);
  Player& p = freshPlayer(s);
  addMod(p, mod(StatKind::MaxHp, Fixed::one(), Fixed::fromInt(2), 1));
  modifierSystem(s, content);
  REQUIRE(p.hp == 3);
  p.hp = 0;  // killed this tick by InteractionSystem
  modifierSystem(s, content);
  CHECK(p.hp == 0);
  p.modifiers.count = 0;
  p.stats.maxHp = 1;
  addMod(p, mod(StatKind::MaxHp, Fixed::one(), Fixed::one(), 1));
  modifierSystem(s, content);  // maxHp rises
  CHECK(p.hp == 0);
}

TEST_CASE("expired modifiers are dropped, compacting in slot order", "[sim][modifier]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(6, 0b1);
  Player& p = freshPlayer(s);
  const Fixed q = Fixed::fromRatio(1, 64);
  addMod(p, mod(StatKind::MoveSpeed, Fixed::one(), q * 1, 2));
  addMod(p, mod(StatKind::MoveSpeed, Fixed::one(), q * 2, 1));
  addMod(p, mod(StatKind::MoveSpeed, Fixed::one(), q * 3, 3));
  addMod(p, mod(StatKind::MoveSpeed, Fixed::one(), q * 4, 1));
  addMod(p, mod(StatKind::MoveSpeed, Fixed::one(), q * 5, 0));  // 0 counts as one last tick

  modifierSystem(s, content);  // all five applied, then 2/4/5 expire
  CHECK(p.stats.moveSpeed == content.player.moveSpeed + q * 15);
  REQUIRE(p.modifiers.count == 2);
  CHECK(p.modifiers.items[0].add == q * 1);
  CHECK(p.modifiers.items[0].ticksLeft == 1);
  CHECK(p.modifiers.items[1].add == q * 3);
  CHECK(p.modifiers.items[1].ticksLeft == 2);
  for (uint8_t i = 2; i < kMaxModifiers; ++i) {
    CHECK(p.modifiers.items[i].ticksLeft == 0);  // slots past count are canonical
    CHECK(p.modifiers.items[i].add == Fixed::zero());
  }

  modifierSystem(s, content);
  CHECK(p.stats.moveSpeed == content.player.moveSpeed + q * 4);
  REQUIRE(p.modifiers.count == 1);
  CHECK(p.modifiers.items[0].add == q * 3);

  modifierSystem(s, content);
  CHECK(p.stats.moveSpeed == content.player.moveSpeed + q * 3);
  CHECK(p.modifiers.count == 0);

  modifierSystem(s, content);
  CHECK(p.stats.moveSpeed == content.player.moveSpeed);
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
