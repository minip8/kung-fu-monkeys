// Power-ups end to end (architecture §5.8): pickups through the rules table,
// grants resolved by ModifierSystem, projectile modifiers riding on the
// banana, the shield rows, arena spawners and the scripted bots. One test
// per shipped power-up; the defs below mirror content/powerups.toml (the sim
// tests never depend on the loader; tests/unit/content checks the TOML).
#include "kfm/core/rng.hpp"
#include "kfm/sim/scripted_inputs.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kThrow = buttonBit(Button::Throw);
constexpr uint8_t kDash = buttonBit(Button::Dash);
constexpr uint8_t kAimRight = 0;
const PlayerId kP0{0};
const PlayerId kP1{1};
const PlayerId kP2{2};

// Indices of the shipped power-ups, in powerups.toml order.
constexpr uint8_t kSpeed = 0;
constexpr uint8_t kBig = 1;
constexpr uint8_t kTriple = 2;
constexpr uint8_t kShield = 3;
constexpr uint8_t kFastHands = 4;

// defaultContent() plus the throw_fast ability and the five power-ups.
ContentDB powerUpContent() {
  ContentDB c = defaultContent();
  AbilityDef fast = c.abilities[c.player.abilities[kAbilitySlotThrow]];
  fast.name = ContentName::of("throw_fast");
  fast.cooldownTicks = 8;
  fast.startupTicks = 1;
  fast.activeTicks = 1;
  fast.recoveryTicks = 3;
  const uint8_t fastIndex = c.abilityCount++;
  c.abilities[fastIndex] = fast;

  ProjectileModifier big;
  big.radiusMul = Fixed::fromInt(2);
  big.speedMul = Fixed::fromRatio(85, 100);
  addPowerUp(c, "speed_boost", 480,
             {statGrant(StatKind::MoveSpeed, Fixed::fromRatio(3, 2), Fixed::zero())});
  addPowerUp(c, "big_banana", 600, {projectileGrant(big)});
  addPowerUp(c, "triple_banana", 900,
             {statGrant(StatKind::AmmoMax, Fixed::one(), Fixed::fromInt(2))});
  addPowerUp(c, "shield", 900, {tagGrant(Tag::Shielded)});
  addPowerUp(c, "fast_hands", 600, {abilityGrant(kAbilitySlotThrow, fastIndex)});
  return c;
}

Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

// Steps one tick with player 0's input, everyone else idle.
EventLog stepWith(WorldState& s, const ContentDB& content, PlayerInput in0 = {}) {
  InputFrame f = frameFor(s);
  f.inputs[0] = in0;
  EventLog log;
  runTicks(s, f, 1, content, log);
  return log;
}

EventLog idle(WorldState& s, const ContentDB& content, int ticks) {
  EventLog log;
  runTicks(s, frameFor(s), ticks, content, log);
  return log;
}

// Player 0 throws to the right; returns the events up to and including the
// throw tick (startup ticks later) and the banana's id.
EntityId throwRight(WorldState& s, const ContentDB& content, EventLog& log) {
  EventLog l = stepWith(s, content, PlayerInput{0, 0, kAimRight, kThrow, 0});
  log.insert(log.end(), l.begin(), l.end());
  for (int i = 0; i < 8 && findEvent(log, EventKind::BoomerangThrown) == nullptr; ++i) {
    l = stepWith(s, content, PlayerInput{0, 0, kAimRight, 0, 0});
    log.insert(log.end(), l.begin(), l.end());
  }
  return firstEvent(log, EventKind::BoomerangThrown).entity;
}

// Places player 0 on a power-up pickup and steps one tick: the pickup tick.
EventLog pickUp(WorldState& s, const ContentDB& content, uint8_t powerUp, Vec2 pos) {
  placePlayer(s, kP0, pos);
  spawnPowerUpPickup(s, pos + Vec2{Fixed::half(), Fixed::zero()}, powerUp);
  return stepWith(s, content);
}

}  // namespace

TEST_CASE("speed boost: picked up, multiplies move speed, expires after its duration",
          "[scenario][powerup]") {
  const ContentDB content = powerUpContent();
  const PowerUpDef& def = content.powerUps[kSpeed];
  WorldState s = makePlayingWorld(41, 0b11, content);
  placePlayer(s, kP1, at(10, 6));

  const EventLog log = pickUp(s, content, kSpeed, at(-4, 0));
  const SimEvent& taken = firstEvent(log, EventKind::PickupTaken);
  CHECK(taken.player == kP0);
  CHECK(taken.a == static_cast<uint32_t>(PickupKind::PowerUp));
  CHECK(taken.b == kSpeed);
  CHECK(s.pickups.size() == 0);
  Player& p = playerAt(s, kP0);
  REQUIRE(p.modifiers.count == 1);
  CHECK(p.modifiers.items[0].powerUp == kSpeed);
  const Fixed boosted = content.player.moveSpeed * Fixed::fromRatio(3, 2);
  CHECK(p.stats.moveSpeed == boosted);

  // Full stick right moves by the boosted speed.
  const Fixed x0 = p.transform.pos.x;
  (void)stepWith(s, content, PlayerInput{0, 255, kAimRight, 0, 0});
  CHECK(playerAt(s, kP0).transform.pos.x - x0 == boosted);

  // In effect after the pickup tick and the next D-1 ticks, gone after T+D.
  (void)idle(s, content, def.durationTicks - 2);
  CHECK(playerAt(s, kP0).stats.moveSpeed == boosted);
  (void)idle(s, content, 1);
  CHECK(playerAt(s, kP0).stats.moveSpeed == content.player.moveSpeed);
  CHECK(playerAt(s, kP0).modifiers.count == 0);
}

TEST_CASE("big banana: wider and slower bananas that hit what a normal one misses",
          "[scenario][powerup]") {
  ContentDB content = powerUpContent();
  removeThrowCharge(content);  // v0.48: the power-up's multiplier alone
  for (const bool powered : {false, true}) {
    INFO("powered " << powered);
    WorldState s = makePlayingWorld(42, 0b111, content);
    placePlayer(s, kP0, at(-6, 0));
    // One unit off the flight line: outside 0.5 + 0.35, inside 0.5 + 0.7.
    placePlayer(s, kP1, at(0, 1));
    placePlayer(s, kP2, at(0, 8));
    if (powered) givePowerUp(playerAt(s, kP0), kBig, 600);
    EventLog log;
    const EntityId id = throwRight(s, content, log);
    const Boomerang* b = s.boomerangs.get(id);
    REQUIRE(b != nullptr);
    const Fixed speed =
        content.boomerang.speed * (powered ? Fixed::fromRatio(85, 100) : Fixed::one());
    CHECK(b->transform.vel == Vec2{speed, Fixed::zero()});
    CHECK(b->radiusMul == (powered ? Fixed::fromInt(2) : Fixed::one()));
    log = idle(s, content, 50);
    CHECK(countEvents(log, EventKind::PlayerHit) == (powered ? 1 : 0));
  }
}

// v0.34: the contact radius is a stat. A monkey shrunk to 0.25 is missed
// by a banana that passes 0.7 off its centre (inside 0.5 + 0.35, outside
// 0.25 + 0.35); without the modifier the same throw hits.
TEST_CASE("radius stat: the contact disc follows the modifier", "[scenario][powerup]") {
  ContentDB content = powerUpContent();
  const uint8_t small = addPowerUp(content, "small", 600,
                                   {statGrant(StatKind::Radius, Fixed::half(), Fixed::zero())});
  for (const bool shrunk : {false, true}) {
    INFO("shrunk " << shrunk);
    WorldState s = makePlayingWorld(43, 0b111, content);
    placePlayer(s, kP0, at(-6, 0));
    placePlayer(s, kP1, Vec2{Fixed::zero(), Fixed::fromRatio(7, 10)});
    placePlayer(s, kP2, at(0, 8));
    if (shrunk) givePowerUp(playerAt(s, kP1), small, 600);
    (void)idle(s, content, 1);  // ModifierSystem derives the stat
    CHECK(playerAt(s, kP1).stats.radius ==
          (shrunk ? content.player.radius * Fixed::half() : content.player.radius));
    EventLog log;
    (void)throwRight(s, content, log);
    log = idle(s, content, 50);
    CHECK(countEvents(log, EventKind::PlayerHit) == (shrunk ? 0 : 1));
  }
}

TEST_CASE("triple banana: ammoMax +2 with a top-up, clamped back on expiry",
          "[scenario][powerup]") {
  const ContentDB content = powerUpContent();
  const PowerUpDef& def = content.powerUps[kTriple];
  WorldState s = makePlayingWorld(44, 0b11, content);
  placePlayer(s, kP1, at(10, 6));
  REQUIRE(playerAt(s, kP0).ammo == 1);
  (void)pickUp(s, content, kTriple, at(-4, 0));
  CHECK(playerAt(s, kP0).stats.ammoMax == 3);
  CHECK(playerAt(s, kP0).ammo == 3);

  // Three throws in a row (cooldown 20 apart), aimed away from everyone.
  int thrown = 0;
  for (int t = 0; t < 80; ++t) {
    const PlayerInput in{0, 0, 128, static_cast<uint8_t>(t % 20 == 0 ? kThrow : 0), 0};
    thrown += countEvents(stepWith(s, content, in), EventKind::BoomerangThrown);
  }
  CHECK(thrown == 3);

  // Expiry: back to one banana at most (here the returning bananas refilled
  // the hand, so the clamp bites).
  playerAt(s, kP0).ammo = 3;
  (void)idle(s, content, def.durationTicks);
  CHECK(playerAt(s, kP0).stats.ammoMax == 1);
  CHECK(playerAt(s, kP0).ammo == 1);
}

TEST_CASE("shield: absorbs exactly one banana hit, then hits land", "[scenario][powerup]") {
  const ContentDB content = powerUpContent();
  WorldState s = makePlayingWorld(45, 0b111, content);
  placePlayer(s, kP0, at(-6, 0));
  placePlayer(s, kP1, at(-1, 0));
  placePlayer(s, kP2, at(0, 8));
  givePowerUp(playerAt(s, kP1), kShield, 900);
  (void)idle(s, content, 1);
  REQUIRE(playerAt(s, kP1).tags.has(Tag::Shielded));

  EventLog log;
  const EntityId first = throwRight(s, content, log);
  log = idle(s, content, 30);
  const SimEvent& broken = firstEvent(log, EventKind::ShieldBroken);
  CHECK(broken.player == kP1);
  CHECK(broken.entity == first);
  CHECK(broken.a == 0);  // the thrower's slot
  CHECK(broken.b == kShield);
  CHECK(countEvents(log, EventKind::PlayerHit) == 0);
  CHECK(countEvents(log, EventKind::BoomerangGrounded) == 1);  // blocked bananas drop
  const Player& p1 = playerAt(s, kP1);
  CHECK(p1.hp == 1);
  CHECK(p1.modifiers.count == 0);
  CHECK_FALSE(p1.tags.has(Tag::Shielded));

  // A second banana falls through to damage.
  playerAt(s, kP0).ammo = 1;
  log.clear();
  (void)throwRight(s, content, log);
  log = idle(s, content, 30);
  CHECK(countEvents(log, EventKind::ShieldBroken) == 0);
  CHECK(countEvents(log, EventKind::PlayerHit) == 1);
  CHECK(s.playerBySlot(kP1) == nullptr);
}

TEST_CASE("shield: absorbs a dash attack and stops the dash", "[scenario][powerup]") {
  const ContentDB content = powerUpContent();
  WorldState s = makePlayingWorld(46, 0b111, content);
  placePlayer(s, kP0, at(-2, 0));
  placePlayer(s, kP1, at(0, 0));
  placePlayer(s, kP2, at(0, 8));
  givePowerUp(playerAt(s, kP1), kShield, 900);
  (void)idle(s, content, 1);
  REQUIRE(playerAt(s, kP0).ammo >= content.dashAttack.minAmmo);

  EventLog log = stepWith(s, content, PlayerInput{0, 255, kAimRight, kDash, 0});
  const EventLog rest = idle(s, content, 30);
  log.insert(log.end(), rest.begin(), rest.end());
  CHECK(countEvents(log, EventKind::ShieldBroken) == 1);
  CHECK(firstEvent(log, EventKind::ShieldBroken).entity.index == 0);  // the dasher
  CHECK(countEvents(log, EventKind::PlayerDashHit) == 0);
  CHECK(s.playerBySlot(kP1) != nullptr);
}

TEST_CASE("fast hands: the throw slot runs the faster def while it lasts", "[scenario][powerup]") {
  const ContentDB content = powerUpContent();
  const uint8_t base = content.player.abilities[kAbilitySlotThrow];
  const AbilityDef& fast = content.abilities[content.powerUps[kFastHands].grants[0].ability];
  WorldState s = makePlayingWorld(47, 0b11, content);
  placePlayer(s, kP1, at(10, 6));
  (void)pickUp(s, content, kFastHands, at(-4, 0));
  const AbilityState& thr = playerAt(s, kP0).abilities[kAbilitySlotThrow];
  CHECK(thr.defIndex == content.powerUps[kFastHands].grants[0].ability);

  const Tick press = s.tick;
  EventLog log;
  (void)throwRight(s, content, log);
  CHECK(firstEvent(log, EventKind::BoomerangThrown).tick == press.plus(fast.startupTicks));
  // Cooldown from the press: the fast def's, not the loadout's 20.
  CHECK(playerAt(s, kP0).abilities[kAbilitySlotThrow].cooldownTicks ==
        fast.cooldownTicks - fast.startupTicks);

  (void)idle(s, content, 600);
  CHECK(playerAt(s, kP0).abilities[kAbilitySlotThrow].defIndex == base);
}

TEST_CASE("stacking: slot order, refresh in place, a full list declines", "[scenario][powerup]") {
  ContentDB content = powerUpContent();
  const uint8_t slow =
      addPowerUp(content, "slow", 100,
                 {statGrant(StatKind::MoveSpeed, Fixed::one(), -Fixed::fromRatio(1, 32))});
  const uint8_t armour = addPowerUp(content, "armour", 100,
                                    {statGrant(StatKind::MaxHp, Fixed::one(), Fixed::fromInt(2))});
  WorldState s = makePlayingWorld(48, 0b11, content);
  placePlayer(s, kP1, at(10, 6));

  (void)pickUp(s, content, kSpeed, at(-4, 0));
  (void)pickUp(s, content, slow, at(-4, 0));
  Player& p = playerAt(s, kP0);
  REQUIRE(p.modifiers.count == 2);
  // base × 1.5 − 1/32: Π mul then Σ add.
  CHECK(p.stats.moveSpeed ==
        content.player.moveSpeed * Fixed::fromRatio(3, 2) - Fixed::fromRatio(1, 32));

  // The same power-up again restarts its timer in its slot.
  (void)idle(s, content, 10);
  (void)pickUp(s, content, kSpeed, at(-4, 0));
  REQUIRE(playerAt(s, kP0).modifiers.count == 2);
  CHECK(playerAt(s, kP0).modifiers.items[0].powerUp == kSpeed);
  CHECK(playerAt(s, kP0).modifiers.items[0].ticksLeft ==
        content.powerUps[kSpeed].durationTicks - 1);

  // maxHp: armour raises hp, its expiry clamps without eliminating.
  (void)pickUp(s, content, armour, at(-4, 0));
  CHECK(playerAt(s, kP0).hp == 3);
  playerAt(s, kP0).hp = 2;  // took a hit while armoured
  (void)idle(s, content, 100);
  CHECK(playerAt(s, kP0).stats.maxHp == 1);
  CHECK(playerAt(s, kP0).hp == 1);

  // A full list leaves the pickup lying.
  Player& full = playerAt(s, kP0);
  full.modifiers = ModifierList{};
  for (uint8_t i = 0; i < kMaxModifiers; ++i)
    givePowerUp(full, kShield, 500);
  const EventLog log = pickUp(s, content, kSpeed, at(-4, 0));
  CHECK(countEvents(log, EventKind::PickupTaken) == 0);
  CHECK(s.pickups.size() == 1);
}

TEST_CASE("spawners: first beat, interval, maxAlive, the def drawn from the set",
          "[scenario][powerup][spawner]") {
  ContentDB content = powerUpContent();
  PickupSpawner sp;
  sp.pos = at(5, 3);
  sp.firstTicks = 10;
  sp.intervalTicks = 20;
  sp.maxAlive = 1;
  sp.powerUpSet = static_cast<uint16_t>((1u << kShield) | (1u << kSpeed));
  content.arenas[0].pickupSpawners[0] = sp;
  content.arenas[0].pickupSpawnerCount = 1;

  WorldState s = makePlayingWorld(49, 0b11, content);
  // makePlayingWorld returns after the first Playing tick (ticksInPhase 0).
  REQUIRE(s.rules.ticksInPhase == 1);
  EventLog log = idle(s, content, 9);  // Playing ticks 1..9
  CHECK(countEvents(log, EventKind::PickupSpawned) == 0);
  log = idle(s, content, 1);  // Playing tick 10
  const SimEvent& spawned = firstEvent(log, EventKind::PickupSpawned);
  CHECK(spawned.a == static_cast<uint32_t>(PickupKind::PowerUp));
  CHECK((spawned.b == kShield || spawned.b == kSpeed));
  const Pickup* k = s.pickups.get(spawned.entity);
  REQUIRE(k != nullptr);
  CHECK(k->kind == PickupKind::PowerUp);
  CHECK(k->pos == sp.pos);
  CHECK(k->spawner == 0);

  // maxAlive 1: the next beat (tick 30) skips while it lies there.
  log = idle(s, content, 20);
  CHECK(countEvents(log, EventKind::PickupSpawned) == 0);
  // Taken away, the beat after spawns again; over many beats both defs show.
  bool seen[2] = {false, false};
  for (int beat = 0; beat < 24; ++beat) {
    s.pickups.clear();
    log = idle(s, content, 20);
    REQUIRE(countEvents(log, EventKind::PickupSpawned) == 1);
    const uint32_t def = firstEvent(log, EventKind::PickupSpawned).b;
    REQUIRE((def == kShield || def == kSpeed));
    seen[def == kShield ? 1 : 0] = true;
  }
  CHECK(seen[0]);
  CHECK(seen[1]);
}

TEST_CASE("the match's power-up set narrows every spawner's set",
          "[scenario][powerup][spawner][match_config]") {
  ContentDB content = powerUpContent();
  PickupSpawner sp;
  sp.pos = at(5, 3);
  sp.firstTicks = 10;
  sp.intervalTicks = 20;
  sp.maxAlive = 1;
  sp.powerUpSet = static_cast<uint16_t>((1u << kShield) | (1u << kSpeed));
  content.arenas[0].pickupSpawners[0] = sp;
  content.arenas[0].pickupSpawnerCount = 1;

  SECTION("intersected with the spawner's own set: only the common power-up spawns") {
    const auto set = static_cast<uint16_t>((1u << kShield) | (1u << kBig));
    const MatchContent match{content, 0, 0, set};
    WorldState s = makePlayingWorld(49, 0b11, content);
    EventLog log;
    for (int beat = 0; beat < 12; ++beat) {
      s.pickups.clear();
      log.clear();
      runTicks(s, frameFor(s), 20, match, log);
      REQUIRE(countEvents(log, EventKind::PickupSpawned) == 1);
      CHECK(firstEvent(log, EventKind::PickupSpawned).b == kShield);
    }
  }
  SECTION("disjoint from it, or empty: the spawner stays silent and draws no RNG") {
    const uint16_t set = GENERATE(uint16_t{0}, static_cast<uint16_t>(1u << kBig));
    WorldState s = makePlayingWorld(49, 0b11, content);
    WorldState none = s;
    EventLog log;
    runTicks(s, frameFor(s), 200, MatchContent{content, 0, 0, set}, log);
    CHECK(countEvents(log, EventKind::PickupSpawned) == 0);
    CHECK(s.pickups.size() == 0);
    // The same run on an arena without spawners: identical worlds.
    ContentDB bare = content;
    bare.arenas[0].pickupSpawnerCount = 0;
    EventLog bareLog;
    runTicks(none, frameFor(none), 200, bare, bareLog);
    CHECK(checksum(s) == checksum(none));
  }
  SECTION("kAllPowerUps (the default config) is the arena as defined") {
    CHECK(matchContent(content, MatchConfig{}).powerUps == kAllPowerUps);
    WorldState a = makePlayingWorld(49, 0b11, content);
    WorldState b = a;
    EventLog la;
    EventLog lb;
    runTicks(a, frameFor(a), 200, content, la);
    runTicks(b, frameFor(b), 200, MatchContent{content, 0, 0, kAllPowerUps}, lb);
    CHECK(countEvents(la, EventKind::PickupSpawned) > 0);
    CHECK(checksum(a) == checksum(b));
  }
}

TEST_CASE("power-up tags and ability-phase tags survive each other's expiry",
          "[scenario][powerup][tags]") {
  // The dash's active phase grants Dashing and, here, Shielded too, so the
  // two sources overlap on one bit.
  ContentDB content = powerUpContent();
  AbilityDef& dash = content.abilities[content.player.abilities[kAbilitySlotDash]];
  dash.tagsActive = Tags::of(Tag::Dashing) | Tag::Shielded;
  const uint8_t dashTicks = static_cast<uint8_t>(dash.activeTicks);
  WorldState s = makePlayingWorld(51, 0b11, content);
  placePlayer(s, kP0, at(-8, 0));
  placePlayer(s, kP1, at(8, 6));
  const PlayerInput dashRight{0, 255, kAimRight, kDash, 0};

  SECTION("the power-up expires mid-dash: the dash keeps Shielded and Dashing") {
    givePowerUp(playerAt(s, kP0), kShield, 3);
    (void)stepWith(s, content, dashRight);  // press: Active from this tick
    (void)idle(s, content, 4);              // the power-up is gone, the dash is not
    const Player& p = playerAt(s, kP0);
    REQUIRE(p.modifiers.count == 0);
    CHECK(p.grantedTags == Tags{});
    CHECK(p.tags.has(Tag::Dashing));
    CHECK(p.tags.has(Tag::Shielded));
    (void)idle(s, content, dashTicks);  // the dash ends: nobody holds Shielded
    CHECK_FALSE(playerAt(s, kP0).tags.has(Tag::Dashing));
    CHECK_FALSE(playerAt(s, kP0).tags.has(Tag::Shielded));
  }
  SECTION("the dash ends first: the power-up keeps Shielded") {
    givePowerUp(playerAt(s, kP0), kShield, 100);
    (void)stepWith(s, content, dashRight);
    CHECK(playerAt(s, kP0).tags.has(Tag::Dashing));
    CHECK(playerAt(s, kP0).grantedTags.has(Tag::Shielded));
    (void)idle(s, content, dashTicks + 2);
    const Player& p = playerAt(s, kP0);
    CHECK_FALSE(p.tags.has(Tag::Dashing));
    CHECK(p.tags.has(Tag::Shielded));
    (void)idle(s, content, 100);  // and the power-up's expiry clears it
    CHECK_FALSE(playerAt(s, kP0).tags.has(Tag::Shielded));
    CHECK(playerAt(s, kP0).grantedTags == Tags{});
  }
}

TEST_CASE("without spawners nothing spawns and the RNG is untouched in play",
          "[scenario][powerup][spawner]") {
  const ContentDB content = powerUpContent();
  WorldState s = makePlayingWorld(50, 0b11, content);
  placePlayer(s, kP0, at(-6, 0));
  placePlayer(s, kP1, at(6, 0));
  const Rng before = s.rng;
  const EventLog log = idle(s, content, 600);
  CHECK(countEvents(log, EventKind::PickupSpawned) == 0);
  CHECK(s.rng.s == before.s);
}

TEST_CASE("scripted bots walk to power-ups on a spawner arena", "[scenario][powerup][scripted]") {
  ContentDB content = powerUpContent();
  const std::array<Vec2, 4> spots = {at(-8, 4), at(8, -4), at(8, 4), at(-8, -4)};
  for (size_t i = 0; i < spots.size(); ++i) {
    PickupSpawner& sp = content.arenas[0].pickupSpawners[i];
    sp.pos = spots[i];
    sp.firstTicks = i < 2 ? 60 : 240;
    sp.intervalTicks = 480;
    sp.maxAlive = 1;
  }
  content.arenas[0].pickupSpawnerCount = 4;

  WorldState s = initialWorld(MatchConfig{0x4B464D20, 0b1111});
  Rng inputs = Rng::seeded(0x5C1220);
  EventLog log;
  EventSink sink;
  for (int t = 0; t < 1800; ++t) {
    const InputFrame f = scriptedFrame(inputs, s, content);
    sink.clear();
    step(s, f, content, sink);
    log.insert(log.end(), sink.begin(), sink.end());
  }
  int taken = 0;
  for (const SimEvent& e : log) {
    if (e.kind == EventKind::PickupTaken && e.a == static_cast<uint32_t>(PickupKind::PowerUp)) {
      ++taken;
    }
  }
  CHECK(taken >= 3);
}
