#include "kfm/sim/content.hpp"

#include "kfm/core/field_count.hpp"
#include "kfm/sim/constants.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

using namespace kfm;
using namespace kfm::sim;

TEST_CASE("contentHash is stable and sensitive to every def", "[sim][content]") {
  const ContentDB& c = defaultContent();
  const uint32_t h = contentHash(c);
  CHECK(h == contentHash(c));
  CHECK(h == contentHash(defaultContent()));

  ContentDB copy = c;
  CHECK(contentHash(copy) == h);

  copy.abilities[static_cast<uint8_t>(AbilityKind::Dash)].perfectWindowTicks += 1;
  CHECK(contentHash(copy) != h);

  copy = c;
  copy.boomerang.speed += Fixed::epsilon();
  CHECK(contentHash(copy) != h);

  copy = c;
  copy.arenas[0].spawnPoints[9].y += Fixed::epsilon();
  CHECK(contentHash(copy) != h);

  copy = c;
  copy.pickup.radius += Fixed::epsilon();
  CHECK(contentHash(copy) != h);

  copy = c;
  copy.modes[0].roundsToWin = 5;
  CHECK(contentHash(copy) != h);

  copy = c;
  copy.abilities[static_cast<uint8_t>(AbilityKind::Throw)].tagsActive.set(Tag::Shielded);
  CHECK(contentHash(copy) != h);

  copy = c;
  copy.dashAttack.minAmmo = 0;
  CHECK(contentHash(copy) != h);

  copy = c;
  copy.dashAttack.consumesAmmo = 1;
  CHECK(contentHash(copy) != h);

  copy = c;
  copy.dashAttack.damage = 2;
  CHECK(contentHash(copy) != h);
}

namespace {
// One mutation per field of T, in declaration order, each of which must move
// the content hash; the count is pinned to fieldCount<T>() like the visit()
// overloads (visit.hpp), so a new content field fails to compile here until
// it has one. Every array slot is hashed whatever the def counts say, so a
// mutation may touch a slot past them.
// The list is a braced list bound to an array reference, not std::to_array:
// MSVC 19.51 will not deduce to_array<Mutation>({lambda, ...}) (it tries to
// deduce the element type from each lambda although it is given).
using ContentMutation = void (*)(ContentDB&);
template <class T, size_t N>
void everyFieldReachesTheHash(const ContentMutation (&mutations)[N]) {
  static_assert(N == fieldCount<T>(), "one mutation per field");
  const ContentDB& base = defaultContent();
  const uint32_t before = contentHash(base);
  for (size_t i = 0; i < N; ++i) {
    ContentDB c = base;  // NOLINT(misc-const-correctness): mutated through the pointer
    mutations[i](c);
    INFO("field " << i);
    CHECK(contentHash(c) != before);
  }
}
}  // namespace

TEST_CASE("every content field reaches the content hash", "[sim][content]") {
  everyFieldReachesTheHash<ContentDB>({
      [](ContentDB& c) { c.abilities[kMaxAbilityDefs - 1].cooldownTicks += 1; },
      [](ContentDB& c) { c.abilityCount += 1; },
      [](ContentDB& c) { c.boomerang.damage += 1; },
      [](ContentDB& c) { c.pickup.radius += Fixed::epsilon(); },
      [](ContentDB& c) { c.player.maxHp += 1; },
      [](ContentDB& c) { c.dashAttack.damage += 1; },
      [](ContentDB& c) { c.arenas[kMaxArenas - 1].wallCount += 1; },
      [](ContentDB& c) { c.arenaCount += 1; },
      [](ContentDB& c) { c.modes[kMaxModes - 1].teams += 1; },
      [](ContentDB& c) { c.modeCount += 1; },
      [](ContentDB& c) { c.powerUps[kMaxPowerUps - 1].grantCount += 1; },
      [](ContentDB& c) { c.powerUpCount += 1; },
      [](ContentDB& c) { c.loadouts[kMaxLoadouts - 1].abilities[2] += 1; },
      [](ContentDB& c) { c.loadoutCount += 1; },
  });
  everyFieldReachesTheHash<ContentName>({
      [](ContentDB& c) { c.modes[0].name.chars[15] = 'x'; },
  });
  everyFieldReachesTheHash<AbilityDef>({
      [](ContentDB& c) { c.abilities[1].name.chars[0] ^= 1; },
      [](ContentDB& c) { c.abilities[1].kind = AbilityKind::Call; },
      [](ContentDB& c) { c.abilities[1].cooldownTicks += 1; },
      [](ContentDB& c) { c.abilities[1].startupTicks += 1; },
      [](ContentDB& c) { c.abilities[1].activeTicks += 1; },
      [](ContentDB& c) { c.abilities[1].recoveryTicks += 1; },
      [](ContentDB& c) { c.abilities[1].perfectWindowTicks += 1; },
      [](ContentDB& c) { c.abilities[1].speed += Fixed::epsilon(); },
      [](ContentDB& c) { c.abilities[1].param0 += Fixed::epsilon(); },
      [](ContentDB& c) { c.abilities[1].param1 += Fixed::epsilon(); },
      [](ContentDB& c) { c.abilities[1].tagsStartup.set(Tag::Shielded); },
      [](ContentDB& c) { c.abilities[1].tagsActive.set(Tag::Shielded); },
      [](ContentDB& c) { c.abilities[1].tagsRecovery.set(Tag::Shielded); },
      [](ContentDB& c) { c.abilities[1].rootPhases ^= 1; },
  });
  everyFieldReachesTheHash<BoomerangDef>({
      [](ContentDB& c) { c.boomerang.radius += Fixed::epsilon(); },
      [](ContentDB& c) { c.boomerang.speed += Fixed::epsilon(); },
      [](ContentDB& c) { c.boomerang.outboundTicks += 1; },
      [](ContentDB& c) { c.boomerang.maxReturnTicks += 1; },
      [](ContentDB& c) { c.boomerang.turnRateBam += 1; },
      [](ContentDB& c) { c.boomerang.restitution += Fixed::epsilon(); },
      [](ContentDB& c) { c.boomerang.spawnOffset += Fixed::epsilon(); },
      [](ContentDB& c) { c.boomerang.damage += 1; },
      [](ContentDB& c) { c.boomerang.pitSinkTicks += 1; },
  });
  everyFieldReachesTheHash<PickupDef>({
      [](ContentDB& c) { c.pickup.radius += Fixed::epsilon(); },
  });
  everyFieldReachesTheHash<PlayerDef>({
      [](ContentDB& c) { c.player.radius += Fixed::epsilon(); },
      [](ContentDB& c) { c.player.moveSpeed += Fixed::epsilon(); },
      [](ContentDB& c) { c.player.maxHp += 1; },
      [](ContentDB& c) { c.player.ammoMax += 1; },
      [](ContentDB& c) { c.player.ammoStart += 1; },
      [](ContentDB& c) { c.player.hitStunTicks += 1; },
      [](ContentDB& c) { c.player.fallTicks += 1; },
      [](ContentDB& c) { c.player.abilities[2] += 1; },
  });
  everyFieldReachesTheHash<DashAttackDef>({
      [](ContentDB& c) { c.dashAttack.damage += 1; },
      [](ContentDB& c) { c.dashAttack.minAmmo += 1; },
      [](ContentDB& c) { c.dashAttack.consumesAmmo ^= 1; },
  });
  everyFieldReachesTheHash<ArenaDef>({
      [](ContentDB& c) { c.arenas[0].name.chars[0] ^= 1; },
      [](ContentDB& c) { c.arenas[0].halfExtents.x += Fixed::epsilon(); },
      [](ContentDB& c) { c.arenas[0].spawnPoints[9].y += Fixed::epsilon(); },
      [](ContentDB& c) { c.arenas[0].spawnPointCount -= 1; },
      [](ContentDB& c) { c.arenas[0].walls[31].max.x += Fixed::epsilon(); },
      [](ContentDB& c) { c.arenas[0].wallCount += 1; },
      [](ContentDB& c) { c.arenas[0].hazards[15].speedMul += Fixed::epsilon(); },
      [](ContentDB& c) { c.arenas[0].hazardCount += 1; },
      [](ContentDB& c) { c.arenas[0].pickupSpawners[7].maxAlive += 1; },
      [](ContentDB& c) { c.arenas[0].pickupSpawnerCount += 1; },
      [](ContentDB& c) { c.arenas[0].spawnGroups += 1; },
      [](ContentDB& c) { c.arenas[0].zones[7].min.y += Fixed::epsilon(); },
      [](ContentDB& c) { c.arenas[0].zoneCount += 1; },
      [](ContentDB& c) { c.arenas[0].cellSize += Fixed::epsilon(); },
  });
  everyFieldReachesTheHash<Wall>({
      [](ContentDB& c) { c.arenas[1].walls[0].min.x += Fixed::epsilon(); },
      [](ContentDB& c) { c.arenas[1].walls[0].max.y += Fixed::epsilon(); },
  });
  everyFieldReachesTheHash<Hazard>({
      [](ContentDB& c) { c.arenas[1].hazards[0].min.x += Fixed::epsilon(); },
      [](ContentDB& c) { c.arenas[1].hazards[0].max.y += Fixed::epsilon(); },
      [](ContentDB& c) { c.arenas[1].hazards[0].kind = HazardKind::Slow; },
      [](ContentDB& c) { c.arenas[1].hazards[0].speedMul += Fixed::epsilon(); },
  });
  everyFieldReachesTheHash<Zone>({
      [](ContentDB& c) { c.arenas[1].zones[0].min.x += Fixed::epsilon(); },
      [](ContentDB& c) { c.arenas[1].zones[0].max.y += Fixed::epsilon(); },
  });
  everyFieldReachesTheHash<PickupSpawner>({
      [](ContentDB& c) { c.arenas[1].pickupSpawners[0].pos.x += Fixed::epsilon(); },
      [](ContentDB& c) { c.arenas[1].pickupSpawners[0].powerUpSet ^= 1; },
      [](ContentDB& c) { c.arenas[1].pickupSpawners[0].firstTicks += 1; },
      [](ContentDB& c) { c.arenas[1].pickupSpawners[0].intervalTicks += 1; },
      [](ContentDB& c) { c.arenas[1].pickupSpawners[0].maxAlive += 1; },
  });
  everyFieldReachesTheHash<ModeDef>({
      [](ContentDB& c) { c.modes[0].name.chars[0] ^= 1; },
      [](ContentDB& c) { c.modes[0].kind = ModeKind::Capture; },
      [](ContentDB& c) { c.modes[0].roundsToWin += 1; },
      [](ContentDB& c) { c.modes[0].roundEndTicks += 1; },
      [](ContentDB& c) { c.modes[0].roundStartTicks += 1; },
      [](ContentDB& c) { c.modes[0].timeLimitTicks += 1; },
      [](ContentDB& c) { c.modes[0].scoreToWin += 1; },
      [](ContentDB& c) { c.modes[0].respawnTicks += 1; },
      [](ContentDB& c) { c.modes[0].respawnInvulnTicks += 1; },
      [](ContentDB& c) { c.modes[0].teams += 1; },
      [](ContentDB& c) { c.modes[0].baseline = 3; },
      [](ContentDB& c) { c.modes[0].roles[9] += 1; },
      [](ContentDB& c) { c.modes[0].roleCount += 1; },
      [](ContentDB& c) { c.modes[0].hillTicks += 1; },
      [](ContentDB& c) { c.modes[0].flagReturnTicks += 1; },
      [](ContentDB& c) { c.modes[0].space = Space::Grid; },
      [](ContentDB& c) { c.modes[0].turnTicks += 1; },
      [](ContentDB& c) { c.modes[0].dashCells += 1; },
      [](ContentDB& c) { c.modes[0].arena = 2; },
      [](ContentDB& c) { c.modes[0].vision.cone += 1; },
      [](ContentDB& c) { c.modes[0].dryRoundTicks += 1; },
      [](ContentDB& c) { c.modes[0].deathBananas = DeathBananas::Vanish; },
      [](ContentDB& c) { c.modes[0].catchScore = -1; },
      [](ContentDB& c) { c.modes[0].suicideScore = -1; },
      [](ContentDB& c) { c.modes[0].bananaRegrowTicks += 1; },
  });
  everyFieldReachesTheHash<VisionDef>({
      [](ContentDB& c) { c.modes[1].vision.kind = VisionKind::Team; },
      [](ContentDB& c) { c.modes[1].vision.peripheral += 1; },
      [](ContentDB& c) { c.modes[1].vision.cone += 1; },
      [](ContentDB& c) { c.modes[1].vision.focus += 1; },
      [](ContentDB& c) { c.modes[1].vision.hearing += 1; },
      [](ContentDB& c) { c.modes[1].vision.bananaSight += 1; },
  });
  everyFieldReachesTheHash<PowerUpDef>({
      [](ContentDB& c) { c.powerUps[0].name.chars[0] ^= 1; },
      [](ContentDB& c) { c.powerUps[0].durationTicks += 1; },
      [](ContentDB& c) { c.powerUps[0].visual.color += 1; },
      [](ContentDB& c) { c.powerUps[0].grants[3].slot += 1; },
      [](ContentDB& c) { c.powerUps[0].grantCount += 1; },
  });
  everyFieldReachesTheHash<PowerUpVisual>({
      [](ContentDB& c) { c.powerUps[1].visual.color += 1; },
      [](ContentDB& c) { c.powerUps[1].visual.shape = PowerUpShape::Coin; },
  });
  everyFieldReachesTheHash<PowerUpGrant>({
      [](ContentDB& c) { c.powerUps[1].grants[0].kind = GrantKind::Tag; },
      [](ContentDB& c) { c.powerUps[1].grants[0].stat.add += Fixed::epsilon(); },
      [](ContentDB& c) { c.powerUps[1].grants[0].slot += 1; },
      [](ContentDB& c) { c.powerUps[1].grants[0].ability += 1; },
      [](ContentDB& c) { c.powerUps[1].grants[0].projectile.damageAdd += 1; },
      [](ContentDB& c) { c.powerUps[1].grants[0].tag = Tag::FriendlyFire; },
  });
  everyFieldReachesTheHash<StatModifier>({
      [](ContentDB& c) { c.powerUps[2].grants[1].stat.stat = StatKind::Radius; },
      [](ContentDB& c) { c.powerUps[2].grants[1].stat.mul += Fixed::epsilon(); },
      [](ContentDB& c) { c.powerUps[2].grants[1].stat.add += Fixed::epsilon(); },
  });
  everyFieldReachesTheHash<ProjectileModifier>({
      [](ContentDB& c) { c.powerUps[2].grants[1].projectile.speedMul += Fixed::epsilon(); },
      [](ContentDB& c) { c.powerUps[2].grants[1].projectile.radiusMul += Fixed::epsilon(); },
      [](ContentDB& c) { c.powerUps[2].grants[1].projectile.damageAdd += 1; },
  });
  everyFieldReachesTheHash<LoadoutDef>({
      [](ContentDB& c) { c.loadouts[0].name.chars[0] ^= 1; },
      [](ContentDB& c) { c.loadouts[0].abilities[1] += 1; },
  });
}

TEST_CASE("default content holds per-tick speeds and the M1 numbers", "[sim][content]") {
  const ContentDB& c = defaultContent();
  const auto perTick = [](int32_t unitsPerSecond) {
    return Fixed::fromRatio(unitsPerSecond, static_cast<int32_t>(kTickHz));
  };
  CHECK(c.player.moveSpeed == perTick(7));
  CHECK(c.boomerang.speed == perTick(16));
  const AbilityDef& dash = c.abilityForSlot(kAbilitySlotDash);
  const AbilityDef& thr = c.abilityForSlot(kAbilitySlotThrow);
  CHECK(dash.kind == AbilityKind::Dash);
  CHECK(thr.kind == AbilityKind::Throw);
  CHECK(dash.speed == perTick(22));
  // Every speed is well below one radius per tick, so swept tests stay sane.
  CHECK(c.player.moveSpeed < c.player.radius);
  CHECK(dash.speed < Fixed::one());
  CHECK(c.boomerang.speed < Fixed::one());

  CHECK(dash.tagsActive.has(Tag::Dashing));
  CHECK(dash.perfectWindowTicks <= dash.activeTicks);
  CHECK(thr.activeTicks >= 1);
  CHECK(c.abilityCount == 3);  // none, dash, throw: later kinds are TOML-only
  CHECK(c.player.ammoStart <= c.player.ammoMax);
  CHECK(c.boomerang.damage >= c.player.maxHp);  // base game: one hit kills
  CHECK(c.modes[0].roundsToWin == 3);
  CHECK(c.pickup.radius == Fixed::half());
  // A fresh banana spawns clear of its thrower.
  CHECK(c.boomerang.spawnOffset > c.player.radius + c.boomerang.radius);
  // Dash attack (§5.6): needs a banana in hand, keeps it, one hit kills.
  CHECK(c.dashAttack.minAmmo == 1);
  CHECK(c.dashAttack.consumesAmmo == 0);
  CHECK(c.dashAttack.damage >= c.player.maxHp);
}

TEST_CASE("arena spawn points are distinct and inside the arena", "[sim][content]") {
  const ContentDB& c = defaultContent();
  const ArenaDef& a = c.arenas[0];
  REQUIRE(a.spawnPointCount == kMaxPlayers);
  const Fixed limitX = a.halfExtents.x - c.player.radius;
  const Fixed limitY = a.halfExtents.y - c.player.radius;
  for (uint8_t i = 0; i < a.spawnPointCount; ++i) {
    const Vec2 p = a.spawnPoints[i];
    CHECK(abs(p.x) < limitX);
    CHECK(abs(p.y) < limitY);
    for (uint8_t j = 0; j < i; ++j) {
      CHECK(distance(p, a.spawnPoints[j]) > c.player.radius * 4);
    }
  }
}
