// Observation v1 and its builder (architecture §8, docs/bot-abi.md): the
// documented layout, a hand-built world mapped field by field, the
// closest-approach math against a brute-force reference, every
// present/absent/dead combination and the host delay copied through.
#include "kfm/ai/observation.hpp"

#include "../sim/sim_test_util.hpp"
#include "kfm/ai/observation_builder.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/sim/content.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

using namespace kfm;
using namespace kfm::ai;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};
constexpr PlayerId kP2{2};

[[nodiscard]] Fixed fx(int32_t num, int32_t den = 1) {
  return Fixed::fromRatio(num, den);
}

// Integer brute force: the smallest k in [0, horizon] minimising |rel + k vel|².
[[nodiscard]] ClosestApproach bruteForce(Vec2 rel, Vec2 vel, uint16_t horizon) {
  uint64_t best = std::numeric_limits<uint64_t>::max();
  uint16_t bestK = 0;
  for (uint32_t k = 0; k <= horizon; ++k) {
    const int64_t dx = int64_t{rel.x.raw} + (int64_t{k} * vel.x.raw);
    const int64_t dy = int64_t{rel.y.raw} + (int64_t{k} * vel.y.raw);
    const uint64_t d = static_cast<uint64_t>(dx * dx) + static_cast<uint64_t>(dy * dy);
    if (d < best) {
      best = d;
      bestK = static_cast<uint16_t>(k);
    }
  }
  return ClosestApproach{bestK, Fixed::fromRaw(static_cast<int32_t>(isqrt(best)))};
}

}  // namespace

TEST_CASE("Observation v1 matches the documented layout", "[ai][observation]") {
  // A handful of offsets from the table in docs/bot-abi.md; the full set is
  // static_asserted in observation.hpp.
  CHECK(sizeof(Observation) == 5748);
  CHECK(alignof(Observation) == 4);
  CHECK(offsetof(Observation, header) + offsetof(ObsHeader, size) == 4);
  CHECK(offsetof(Observation, match) + offsetof(ObsMatch, inputDelayTicks) == 44);
  CHECK(offsetof(Observation, match) + offsetof(ObsMatch, scores) == 60);
  CHECK(offsetof(Observation, match) + offsetof(ObsMatch, reservedModeData) == 120);
  CHECK(offsetof(Observation, rules) + offsetof(ObsRules, bananaTurnRateBam) == 216);
  CHECK(offsetof(Observation, self) + offsetof(ObsSelf, posX) == 236);
  CHECK(offsetof(Observation, self) + offsetof(ObsSelf, abilities) == 296);
  CHECK(offsetof(Observation, self) + offsetof(ObsSelf, modifiers) == 440);
  CHECK(offsetof(Observation, players) == 472);
  CHECK(offsetof(Observation, players) + (3 * sizeof(ObsPlayer)) + offsetof(ObsPlayer, posX) ==
        680);
  CHECK(offsetof(Observation, bananas) == 1112);
  CHECK(offsetof(Observation, bananas) + offsetof(ObsBanana, approachTicks) == 1126);
  CHECK(offsetof(Observation, pickups) == 3928);
  CHECK(offsetof(Observation, arena) + offsetof(ObsArena, walls) == 4532);
  CHECK(offsetof(Observation, arena) + offsetof(ObsArena, spawners) == 5428);
  CHECK(offsetof(Observation, reservedHeardCalls) == 5556);
}

TEST_CASE("closestApproach equals an integer brute force", "[ai][observation]") {
  // Hand-picked cases: head-on, passing, receding, still, beyond the horizon.
  CHECK(closestApproach(Vec2{fx(10), Fixed{}}, Vec2{fx(-1), Fixed{}}).ticks == 10);
  CHECK(closestApproach(Vec2{fx(10), Fixed{}}, Vec2{fx(-1), Fixed{}}).distance == Fixed{});
  const ClosestApproach pass = closestApproach(Vec2{fx(4), fx(3)}, Vec2{fx(-1, 2), Fixed{}});
  CHECK(pass.ticks == 8);
  CHECK(pass.distance == fx(3));
  const ClosestApproach away = closestApproach(Vec2{fx(4), fx(3)}, Vec2{fx(1), Fixed{}});
  CHECK(away.ticks == 0);
  CHECK(away.distance == fx(5));
  CHECK(closestApproach(Vec2{fx(4), fx(3)}, Vec2{}).distance == fx(5));
  CHECK(closestApproach(Vec2{fx(1000), Fixed{}}, Vec2{fx(-1, 100), Fixed{}}).ticks ==
        kObsApproachHorizonTicks);

  // Random vectors in the arena's range, short horizons so the brute force is cheap.
  Rng rng = Rng::seeded(0xA11CE);
  for (int i = 0; i < 4000; ++i) {
    const Vec2 rel{Fixed::fromRaw(rng.range(-40 * 65536, 40 * 65536)),
                   Fixed::fromRaw(rng.range(-40 * 65536, 40 * 65536))};
    const Vec2 vel{Fixed::fromRaw(rng.range(-65536, 65536)),
                   Fixed::fromRaw(rng.range(-65536, 65536))};
    const auto horizon = static_cast<uint16_t>(rng.range(0, 160));
    const ClosestApproach got = closestApproach(rel, vel, horizon);
    const ClosestApproach want = bruteForce(rel, vel, horizon);
    INFO("case " << i);
    REQUIRE(got.ticks == want.ticks);
    REQUIRE(got.distance == want.distance);
  }
}

TEST_CASE("a hand-built world maps field by field", "[ai][observation]") {
  // Content with a second arena (walls, hazards, a spawner) and a second mode,
  // selected by index, so the indices and the statics are not all defaults.
  ContentDB c = defaultContent();
  ArenaDef& arena = c.arenas[1];
  arena = c.arenas[0];
  arena.name = ContentName::of("test");
  arena.walls[0] = Wall{Vec2{fx(-1), fx(-2)}, Vec2{fx(1), fx(2)}};
  arena.wallCount = 1;
  arena.hazards[0] = Hazard{Vec2{fx(5), fx(5)}, Vec2{fx(7), fx(6)}, HazardKind::Slow, fx(1, 2)};
  arena.hazardCount = 1;
  arena.pickupSpawners[0] = PickupSpawner{Vec2{fx(3), fx(-4)}, 0x5, 60, 300, 2};
  arena.pickupSpawnerCount = 1;
  c.arenaCount = 2;
  ModeDef& mode = c.modes[1];
  mode.name = ContentName::of("dm");
  mode.kind = ModeKind::Deathmatch;
  mode.timeLimitTicks = 10800;
  mode.scoreToWin = 7;
  mode.respawnTicks = 120;
  mode.respawnInvulnTicks = 90;
  c.modeCount = 2;
  const uint8_t shield = addPowerUp(c, "shield", 600, {tagGrant(Tag::Shielded)});
  c.loadouts[1] = LoadoutDef{ContentName::of("pair"), c.player.abilities};
  c.loadoutCount = 2;
  const MatchContent content(c, 1, 1, kAllPowerUps, 1);

  WorldState s = makePlayingWorld(7, 0b0111, defaultContent());
  s.rules.scores = {1, 2, 3};
  s.rules.deaths[1] = 4;
  s.rules.respawnTicks[2] = 33;
  s.rules.invulnTicks[0] = 12;
  s.rules.modeData[0] = 0xA1B2C3D4;
  s.rules.modeData[kModeDataWords - 1] = 9;
  Player& me = placePlayer(s, kP0, Vec2{fx(2), fx(1)});
  me.transform.prevPos = Vec2{fx(19, 10), fx(1)};
  me.transform.vel = Vec2{fx(1, 10), Fixed{}};
  me.transform.facing = Angle::fromInput8(77);
  me.hp = 1;
  me.ammo = 0;
  me.prevButtons = buttonBit(Button::Dash);
  me.tags.set(Tag::Dashing);
  me.hitStunTicks = 3;
  me.knockback = Vec2{Fixed{}, fx(1, 4)};
  me.knockbackTicks = 5;
  me.abilities[kAbilitySlotDash].phase = AbilityPhase::Active;
  me.abilities[kAbilitySlotDash].phaseTicks = 2;
  me.abilities[kAbilitySlotDash].cooldownTicks = 43;
  me.abilities[kAbilitySlotDash].activatedTick = Tick{90};
  me.abilities[kAbilitySlotDash].activatedSubTick = 128;
  me.abilities[kAbilitySlotDash].dir = Vec2{Fixed::one(), Fixed{}};
  givePowerUp(me, shield, 250);
  Player& other = placePlayer(s, kP1, Vec2{fx(-6), fx(3)});
  other.transform.vel = Vec2{Fixed{}, fx(-1, 8)};
  other.ammo = 1;
  // Slot 2 is present but its monkey is gone (eliminated, respawn pending).
  s.players.despawn(s.players.idAt(2));
  const EntityId banana =
      spawnBanana(s, kP1, Vec2{fx(-2), fx(1)}, Vec2{fx(1, 2), Fixed{}}, BoomerangPhase::Returning);
  s.boomerangs.items[banana.index].ticksAlive = 50;
  s.boomerangs.items[banana.index].ticksInPhase = 14;
  s.boomerangs.items[banana.index].transform.facing = Angle::fromBam(1234);
  s.boomerangs.items[banana.index].radiusMul = fx(2);
  const EntityId own = spawnBanana(s, kP0, Vec2{fx(8), fx(8)}, Vec2{});
  const EntityId pickup = spawnPowerUpPickup(s, Vec2{fx(3), fx(-4)}, shield, 0);

  Observation obs{};
  std::memset(&obs, 0xCD, sizeof obs);  // the builder must overwrite every byte
  buildObservation(s, content, 0xDEADBEEF, kP0, 3, obs);

  SECTION("header and match") {
    CHECK(obs.header.version == kObservationVersion);
    CHECK(obs.header.size == sizeof(Observation));
    CHECK(obs.header.schemaFlags == 0);
    CHECK(obs.header.reserved0 == 0);
    CHECK(obs.match.tick == s.tick.value);
    CHECK(obs.match.ticksInPhase == s.rules.ticksInPhase);
    CHECK(obs.match.timeLimitTicks == 10800);
    CHECK(obs.match.contentHash == 0xDEADBEEF);
    CHECK(obs.match.modeIndex == 1);
    CHECK(obs.match.modeKind == static_cast<uint8_t>(ObsModeKind::Deathmatch));
    CHECK(obs.match.arenaIndex == 1);
    CHECK(obs.match.phase == static_cast<uint8_t>(ObsMatchPhase::Playing));
    CHECK(obs.match.round == s.rules.round);
    CHECK(obs.match.scoreToWin == 7);
    CHECK(obs.match.aliveCount == 2);
    CHECK(obs.match.presentMask == 0b0111);
    CHECK(obs.match.aliveMask == 0b0011);
    CHECK(obs.match.inputDelayTicks == 3);
    CHECK(obs.match.lastRoundWinner == kObsNoSlot);
    CHECK(obs.match.respawnDelayTicks == 120);
    CHECK(obs.match.respawnInvulnTicks == 90);
    CHECK(obs.match.scores[2] == 3);
    CHECK(obs.match.deaths[1] == 4);
    CHECK(obs.match.respawnTicks[2] == 33);
    CHECK(obs.match.reservedTurnTicks == 0);
    CHECK(obs.match.reservedTicksToLatch == 0);
    for (const uint16_t v : obs.match.reservedTeamScores)
      CHECK(v == 0);
    // v0.31: MatchRules::modeData fills the first kModeDataWords words.
    CHECK(obs.match.reservedModeData[0] == 0xA1B2C3D4);
    CHECK(obs.match.reservedModeData[kModeDataWords - 1] == 9);
    for (uint32_t i = 1; i < kModeDataWords - 1; ++i)
      CHECK(obs.match.reservedModeData[i] == 0);
    for (uint32_t i = kModeDataWords; i < kObsModeDataWords; ++i)
      CHECK(obs.match.reservedModeData[i] == 0);
  }

  SECTION("rules") {
    CHECK(obs.rules.playerRadius == c.player.radius.raw);
    CHECK(obs.rules.playerMoveSpeed == c.player.moveSpeed.raw);
    CHECK(obs.rules.bananaSpeed == c.boomerang.speed.raw);
    CHECK(obs.rules.bananaOutboundTicks == c.boomerang.outboundTicks);
    CHECK(obs.rules.bananaTurnRateBam == c.boomerang.turnRateBam);
    CHECK(obs.rules.dashAttackMinAmmo == c.dashAttack.minAmmo);
    CHECK(obs.rules.playerAmmoMax == c.player.ammoMax);
    CHECK(obs.rules.pad0 == 0);
  }

  SECTION("self") {
    const ObsSelf& o = obs.self;
    CHECK(o.slot == 0);
    CHECK(o.alive == 1);
    CHECK(o.hp == 1);
    CHECK(o.ammo == 0);
    CHECK(o.ammoMax == me.stats.ammoMax);
    CHECK(o.lastButtons == buttonBit(Button::Dash));
    CHECK(o.posX == fx(2).raw);
    CHECK(o.posY == fx(1).raw);
    CHECK(o.prevPosX == fx(19, 10).raw);
    CHECK(o.velX == fx(1, 10).raw);
    CHECK(o.moveSpeed == me.stats.moveSpeed.raw);
    CHECK(o.radius == c.player.radius.raw);
    CHECK(o.knockbackY == fx(1, 4).raw);
    CHECK(o.knockbackTicks == 5);
    CHECK(o.tags == me.tags.bits);
    CHECK(o.hitStunTicks == 3);
    CHECK(o.invulnTicks == 12);
    CHECK(o.facing == 77);
    CHECK(o.reservedFacingOctant == 0);
    CHECK(o.reservedCellX == 0);
    CHECK(o.reservedRole == 1);  // v0.31: the loadout index the slot plays
    const ObsAbility& dash = o.abilities[kAbilitySlotDash];
    CHECK(dash.kind == static_cast<uint8_t>(ObsAbilityKind::Dash));
    CHECK(dash.phase == static_cast<uint8_t>(ObsAbilityPhase::Active));
    CHECK(dash.button == buttonBit(Button::Dash));
    CHECK(dash.phaseTicks == 2);
    CHECK(dash.cooldownTicks == 43);
    CHECK(dash.activatedTick == 90);
    CHECK(dash.activatedSubTick == 128);
    CHECK(dash.defPerfectWindowTicks == c.abilities[1].perfectWindowTicks);
    CHECK(dash.defSpeed == c.abilities[1].speed.raw);
    CHECK(dash.dirX == Fixed::one().raw);
    CHECK(o.abilities[kAbilitySlotThrow].button == buttonBit(Button::Throw));
    CHECK(o.abilities[kAbilitySlotThrow].kind == static_cast<uint8_t>(ObsAbilityKind::Throw));
    CHECK(o.abilities[kAbilitySlotExtra].kind == static_cast<uint8_t>(ObsAbilityKind::None));
    CHECK(o.modifierCount == 1);
    CHECK(o.modifiers[0].powerUp == shield);
    CHECK(o.modifiers[0].ticksLeft == 250);
    CHECK(o.modifiers[1].ticksLeft == 0);
  }

  SECTION("players") {
    CHECK(obs.players[0].relation == static_cast<uint8_t>(ObsRelation::Self));
    const ObsPlayer& p1 = obs.players[1];
    CHECK(p1.present == 1);
    CHECK(p1.alive == 1);
    CHECK(p1.reservedVisible == 0);
    CHECK(p1.relation == static_cast<uint8_t>(ObsRelation::Enemy));
    CHECK(p1.reservedTeam == 0);
    CHECK(p1.ammo == 1);
    CHECK(p1.posX == fx(-6).raw);
    CHECK(p1.velY == fx(-1, 8).raw);
    CHECK(p1.abilityKind[kAbilitySlotThrow] == static_cast<uint8_t>(ObsAbilityKind::Throw));
    CHECK(p1.dashing == 0);
    CHECK(obs.players[0].dashing == 1);
    const ObsPlayer& p2 = obs.players[2];
    CHECK(p2.present == 1);
    CHECK(p2.alive == 0);
    CHECK(p2.relation == static_cast<uint8_t>(ObsRelation::Enemy));
    CHECK(p2.respawnTicks == 33);
    CHECK(p2.posX == 0);
    const ObsPlayer& p3 = obs.players[3];
    CHECK((p3.present | p3.alive | p3.relation) == 0);
  }

  SECTION("bananas and pickups") {
    const ObsBanana& b = obs.bananas[banana.index];
    CHECK(b.valid == 1);
    CHECK(b.phase == static_cast<uint8_t>(ObsBananaPhase::Returning));
    CHECK(b.owner == 1);
    CHECK(b.ownerRelation == static_cast<uint8_t>(ObsRelation::Enemy));
    CHECK(b.damage == c.boomerang.damage);
    CHECK(b.headingBam == 1234);
    CHECK(b.ticksAlive == 50);
    CHECK(b.ticksInPhase == 14);
    CHECK(b.posX == fx(-2).raw);
    CHECK(b.velX == fx(1, 2).raw);
    CHECK(b.speed == c.boomerang.speed.raw);
    CHECK(b.radius == (c.boomerang.radius * fx(2)).raw);
    // From (-2, 1) at +0.5/tick to (2, 1): 8 ticks, a direct hit.
    CHECK(b.approachTicks == 8);
    CHECK(b.approachDistance == 0);
    const ObsBanana& mine = obs.bananas[own.index];
    CHECK(mine.ownerRelation == static_cast<uint8_t>(ObsRelation::Self));
    CHECK(mine.approachTicks == 0);
    CHECK(mine.approachDistance == length(Vec2{fx(6), fx(7)}).raw);
    for (uint32_t i = 0; i < kObsMaxBananas; ++i) {
      if (i != banana.index && i != own.index) CHECK(obs.bananas[i].valid == 0);
    }
    const ObsPickup& k = obs.pickups[pickup.index];
    CHECK(k.valid == 1);
    CHECK(k.kind == static_cast<uint8_t>(ObsPickupKind::PowerUp));
    CHECK(k.powerUp == shield);
    CHECK(k.posY == fx(-4).raw);
    CHECK(k.radius == c.pickup.radius.raw);
  }

  SECTION("arena") {
    const ObsArena& a = obs.arena;
    CHECK(a.halfExtents.x == arena.halfExtents.x.raw);
    CHECK(a.spawnPointCount == arena.spawnPointCount);
    CHECK(a.spawnPoints[3].y == arena.spawnPoints[3].y.raw);
    CHECK(a.wallCount == 1);
    CHECK(a.walls[0].minY == fx(-2).raw);
    CHECK(a.walls[1].maxX == 0);
    CHECK(a.hazardCount == 1);
    CHECK(a.hazards[0].kind == static_cast<uint8_t>(ObsHazardKind::Slow));
    CHECK(a.hazards[0].speedMul == fx(1, 2).raw);
    CHECK(a.hazards[0].box.maxX == fx(7).raw);
    CHECK(a.spawnerCount == 1);
    CHECK(a.spawners[0].powerUpSet == 0x5);
    CHECK(a.spawners[0].intervalTicks == 300);
    CHECK(a.spawners[0].maxAlive == 2);
    for (const ObsHeardCall& call : obs.reservedHeardCalls)
      CHECK((call.valid | call.slot | call.tick) == 0);
  }

  SECTION("every padding and reserved byte is zero") {
    // Rebuild into a buffer pre-filled with another pattern: the two results
    // are identical byte for byte, so nothing of the old contents survives.
    Observation again{};
    std::memset(&again, 0x3A, sizeof again);
    buildObservation(s, content, 0xDEADBEEF, kP0, 3, again);
    CHECK(std::memcmp(&obs, &again, sizeof obs) == 0);
  }
}

TEST_CASE("every present / alive combination maps to its flags", "[ai][observation]") {
  // Slots: 0 self, 1 present + alive, 2 present + dead, 3 left mid-round
  // (alive, not present), 4.. empty.
  WorldState s = makePlayingWorld(11, 0b1111);
  s.players.despawn(s.players.idAt(2));
  s.rules.present = 0b0011 | 0b0100;
  Observation obs{};
  buildObservation(s, defaultContent(), kP0, 0, obs);
  CHECK(obs.match.presentMask == 0b0111);
  CHECK(obs.match.aliveMask == 0b1011);
  CHECK(obs.match.aliveCount == 3);
  const auto flags = [&](uint32_t i) {
    return std::pair{obs.players[i].present, obs.players[i].alive};
  };
  CHECK(flags(0) == std::pair<uint8_t, uint8_t>{1, 1});
  CHECK(flags(1) == std::pair<uint8_t, uint8_t>{1, 1});
  CHECK(flags(2) == std::pair<uint8_t, uint8_t>{1, 0});
  CHECK(flags(3) == std::pair<uint8_t, uint8_t>{0, 1});
  for (uint32_t i = 4; i < kObsMaxPlayers; ++i) {
    CHECK(flags(i) == std::pair<uint8_t, uint8_t>{0, 0});
    CHECK(obs.players[i].relation == static_cast<uint8_t>(ObsRelation::None));
  }
  CHECK(obs.players[3].relation == static_cast<uint8_t>(ObsRelation::Enemy));

  // Seen from the dead slot: self is not alive, and no approach is reported.
  spawnBanana(s, kP1, Vec2{}, Vec2{Fixed::one(), Fixed{}});
  buildObservation(s, defaultContent(), kP2, 0, obs);
  CHECK(obs.self.slot == 2);
  CHECK(obs.self.alive == 0);
  CHECK(obs.self.posX == 0);
  CHECK(obs.self.abilities[0].kind == 0);
  CHECK(obs.players[2].relation == static_cast<uint8_t>(ObsRelation::Self));
  CHECK(obs.players[0].relation == static_cast<uint8_t>(ObsRelation::Enemy));
  CHECK(obs.bananas[0].approachTicks == kObsNoApproach);
  CHECK(obs.bananas[0].approachDistance == 0);
  CHECK(obs.bananas[0].ownerRelation == static_cast<uint8_t>(ObsRelation::Enemy));

  // Seen from the owner: its banana is its own.
  buildObservation(s, defaultContent(), kP1, 0, obs);
  CHECK(obs.bananas[0].ownerRelation == static_cast<uint8_t>(ObsRelation::Self));
  CHECK(obs.self.alive == 1);
  CHECK(obs.self.slot == 1);
}

TEST_CASE("the host delay and the content hash are copied through", "[ai][observation]") {
  const WorldState s = makePlayingWorld(3, 0b11);
  Observation obs{};
  for (const uint8_t delay : {uint8_t{0}, uint8_t{1}, uint8_t{6}, uint8_t{255}}) {
    buildObservation(s, defaultContent(), kP1, delay, obs);
    CHECK(obs.match.inputDelayTicks == delay);
    CHECK(obs.match.contentHash == contentHash(defaultContent()));
  }
  // The observation is a pure function of its arguments.
  Observation twin{};
  buildObservation(s, defaultContent(), kP1, 255, twin);
  CHECK(std::memcmp(&obs, &twin, sizeof obs) == 0);
}
