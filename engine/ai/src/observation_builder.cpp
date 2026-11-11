// The observation builder (see observation_builder.hpp). Reads WorldState and
// MatchContent, writes only `out`. Integer and fixed-point arithmetic only.
#include "kfm/ai/observation_builder.hpp"

#include "kfm/sim/constants.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/content/arena.hpp"
#include "kfm/sim/content/mode.hpp"
#include "kfm/sim/content/powerup.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/spaces/grid_cells.hpp"
#include "kfm/sim/tags.hpp"

#include <cstdint>

namespace kfm::ai {

// The capacities are the sim's: growing a pool is an Observation version bump.
static_assert(kObsMaxPlayers == sim::kMaxPlayers);
static_assert(kObsMaxBananas == sim::kMaxBoomerangs);
static_assert(kObsMaxPickups == sim::kMaxPickups);
static_assert(kObsAbilitySlots == sim::kAbilitySlots);
static_assert(kObsMaxModifiers == sim::kMaxModifiers);
static_assert(kObsMaxSpawnPoints == sim::kMaxPlayers);  // ArenaDef::spawnPoints
static_assert(kObsMaxWalls == sim::kMaxArenaWalls);
static_assert(kObsMaxHazards == sim::kMaxArenaHazards);
static_assert(kObsMaxSpawners == sim::kMaxPickupSpawners);
static_assert(static_cast<uint32_t>(sim::Tag::kCount) <= 32);

// The byte values are the sim's enums, frozen: a renumbering in the sim must
// not silently change the ABI.
static_assert(static_cast<uint8_t>(sim::MatchPhase::Playing) ==
                  static_cast<uint8_t>(ObsMatchPhase::Playing) &&
              static_cast<uint8_t>(sim::MatchPhase::MatchEnded) ==
                  static_cast<uint8_t>(ObsMatchPhase::MatchEnded));
static_assert(
    static_cast<uint8_t>(sim::ModeKind::Survival) == static_cast<uint8_t>(ObsModeKind::Survival) &&
    static_cast<uint8_t>(sim::ModeKind::TeamRounds) ==
        static_cast<uint8_t>(ObsModeKind::TeamRounds) &&
    static_cast<uint8_t>(sim::ModeKind::TeamDeathmatch) ==
        static_cast<uint8_t>(ObsModeKind::TeamDeathmatch) &&
    static_cast<uint8_t>(sim::ModeKind::Hill) == static_cast<uint8_t>(ObsModeKind::Hill) &&
    static_cast<uint8_t>(sim::ModeKind::Capture) == static_cast<uint8_t>(ObsModeKind::Capture) &&
    static_cast<uint8_t>(sim::ModeKind::Control) == static_cast<uint8_t>(ObsModeKind::Control) &&
    static_cast<uint8_t>(sim::ModeKind::kCount) == 8);
static_assert(static_cast<uint8_t>(sim::AbilityKind::Parry) ==
                  static_cast<uint8_t>(ObsAbilityKind::Parry) &&
              static_cast<uint8_t>(sim::AbilityKind::Call) ==
                  static_cast<uint8_t>(ObsAbilityKind::Call) &&
              static_cast<uint8_t>(sim::AbilityKind::kCount) == 7);
static_assert(static_cast<uint8_t>(sim::AbilityPhase::Recovery) ==
              static_cast<uint8_t>(ObsAbilityPhase::Recovery));
static_assert(static_cast<uint8_t>(sim::BoomerangPhase::Grounded) ==
                  static_cast<uint8_t>(ObsBananaPhase::Grounded) &&
              static_cast<uint8_t>(sim::BoomerangPhase::Sinking) ==
                  static_cast<uint8_t>(ObsBananaPhase::Sinking));
static_assert(static_cast<uint8_t>(sim::PickupKind::PowerUp) ==
                  static_cast<uint8_t>(ObsPickupKind::PowerUp) &&
              static_cast<uint8_t>(sim::PickupKind::Flag) ==
                  static_cast<uint8_t>(ObsPickupKind::Flag));
static_assert(static_cast<uint8_t>(sim::HazardKind::Slow) ==
              static_cast<uint8_t>(ObsHazardKind::Slow));

namespace {

template <class E>
[[nodiscard]] constexpr uint8_t byteOf(E e) {
  return static_cast<uint8_t>(e);
}

[[nodiscard]] constexpr uint8_t angle8(Angle a) {
  // Player facing is an aim byte shifted to BAM (InputSystem), so this is exact.
  return static_cast<uint8_t>(a.bam >> 8);
}

[[nodiscard]] constexpr uint64_t squaredAt(int64_t rx, int64_t ry, int64_t vx, int64_t vy,
                                           int64_t k) {
  const int64_t dx = rx + (k * vx);
  const int64_t dy = ry + (k * vy);
  return static_cast<uint64_t>(dx * dx) + static_cast<uint64_t>(dy * dy);
}

[[nodiscard]] Fixed lengthOfSquaredRaw(uint64_t sq) {
  const uint32_t r = isqrt(sq);
  return Fixed::fromRaw(r > static_cast<uint32_t>(INT32_MAX) ? INT32_MAX : static_cast<int32_t>(r));
}

// bot-abi.md §3, filled since architecture v0.32: Ally through the match's
// teams (MatchContent::relation; a free-for-all has none).
[[nodiscard]] uint8_t relationTo(const sim::MatchContent& content, PlayerId self, PlayerId other) {
  if (!other.isValid() || other.value >= sim::kMaxPlayers) return byteOf(ObsRelation::None);
  switch (content.relation(self, other)) {
    case sim::Relation::Self:
      return byteOf(ObsRelation::Self);
    case sim::Relation::Ally:
      return byteOf(ObsRelation::Ally);
    case sim::Relation::Enemy:
      break;
  }
  return byteOf(ObsRelation::Enemy);
}

// The index of `def` in `defs[0, count)`, else 0 (MatchContent always
// references an entry of its DB).
template <class T, class Array>
[[nodiscard]] uint8_t indexOf(const T& def, const Array& defs, uint8_t count) {
  for (uint8_t i = 0; i < count && i < defs.size(); ++i) {
    if (&defs[i] == &def) return i;
  }
  return 0;
}

void fillMatch(const sim::WorldState& world, const sim::MatchContent& content, uint32_t hash,
               uint8_t inputDelayTicks, ObsMatch& m) {
  const sim::MatchRules& r = world.rules;
  const sim::ModeDef& mode = content.mode;
  m.tick = world.tick.value;
  m.ticksInPhase = r.ticksInPhase;
  m.timeLimitTicks = mode.kind == sim::ModeKind::Rounds ? 0 : mode.timeLimitTicks;
  m.contentHash = hash;
  m.modeIndex = indexOf(mode, content.db.modes, content.db.modeCount);
  m.modeKind = byteOf(mode.kind);
  m.arenaIndex = indexOf(content.arena, content.db.arenas, content.db.arenaCount);
  m.phase = byteOf(r.phase);
  m.round = r.round;
  m.roundsToWin = mode.roundsToWin;
  m.scoreToWin = mode.scoreToWin;
  m.presentMask = r.present;
  m.inputDelayTicks = inputDelayTicks;
  m.lastRoundWinner = r.lastRoundWinner.isValid() ? r.lastRoundWinner.value : kObsNoSlot;
  m.roundStartTicks = mode.roundStartTicks;
  m.roundEndTicks = mode.roundEndTicks;
  m.respawnDelayTicks = mode.respawnTicks;
  m.respawnInvulnTicks = mode.respawnInvulnTicks;
  m.bananaRegrowTicks = mode.bananaRegrowTicks;  // v0.57
  uint8_t alive = 0;
  uint16_t aliveMask = 0;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    m.scores[i] = r.scores[i];
    m.deaths[i] = r.deaths[i];
    m.respawnTicks[i] = r.respawnTicks[i];
    if (world.playerBySlot(PlayerId{i}) != nullptr) {
      ++alive;
      aliveMask = static_cast<uint16_t>(aliveMask | (1u << i));
    }
  }
  m.aliveCount = alive;
  m.aliveMask = aliveMask;
  // bot-abi.md §3, filled since architecture v0.31: the mode's own state.
  // MatchRules has kModeDataWords of it; the rest of the reserve stays 0.
  static_assert(sim::kModeDataWords <= kObsModeDataWords);
  for (uint8_t i = 0; i < sim::kModeDataWords; ++i)
    m.reservedModeData[i] = r.modeData[i];
  // bot-abi.md §3, filled since architecture v0.69: the arena's first zones
  // (the hill is zone 0, a capture team t's home zone t) in the rest.
  static_assert(sim::kModeDataWords <= kObsZoneWord);
  for (uint8_t k = 0; k < kObsZones && k < content.arena.zoneCount; ++k) {
    const sim::Zone& z = content.arena.zones[k];
    uint32_t* const box = &m.reservedModeData[kObsZoneWord + (4u * k)];
    box[0] = static_cast<uint32_t>(z.min.x.raw);
    box[1] = static_cast<uint32_t>(z.min.y.raw);
    box[2] = static_cast<uint32_t>(z.max.x.raw);
    box[3] = static_cast<uint32_t>(z.max.y.raw);
  }
  // bot-abi.md §3, filled since architecture v0.32: every team's score by
  // team index (the slot's own score in a free-for-all, team i = slot i).
  for (uint8_t t = 0; t < sim::kMaxPlayers; ++t)
    m.reservedTeamScores[t] = content.teamScore(r.scores, t);
  // bot-abi.md §3, filled since architecture v0.35: a grid mode's turn clock.
  const TurnClock clock = turnClock(world, content);
  m.reservedTurnTicks = clock.turnTicks;
  m.reservedTicksToLatch = clock.ticksToLatch;
}

void fillRules(const sim::ContentDB& db, ObsRules& r) {
  r.playerRadius = db.player.radius.raw;
  r.playerMoveSpeed = db.player.moveSpeed.raw;
  r.bananaRadius = db.boomerang.radius.raw;
  r.bananaSpeed = db.boomerang.speed.raw;
  r.bananaSpawnOffset = db.boomerang.spawnOffset.raw;
  r.bananaRestitution = db.boomerang.restitution.raw;
  r.pickupRadius = db.pickup.radius.raw;
  r.bananaOutboundTicks = db.boomerang.outboundTicks;
  r.bananaMaxReturnTicks = db.boomerang.maxReturnTicks;
  r.bananaTurnRateBam = db.boomerang.turnRateBam;
  r.hitStunTicks = db.player.hitStunTicks;
  r.bananaDamage = db.boomerang.damage;
  r.dashAttackDamage = db.dashAttack.damage;
  r.dashAttackMinAmmo = db.dashAttack.minAmmo;
  r.dashAttackConsumesAmmo = db.dashAttack.consumesAmmo;
  r.playerMaxHp = db.player.maxHp;
  r.playerAmmoMax = db.player.ammoMax;
  r.playerAmmoStart = db.player.ammoStart;
}

void fillAbility(const sim::AbilityState& st, const sim::ContentDB& db, uint8_t slot,
                 ObsAbility& a) {
  const sim::AbilityDef& def = db.abilityAt(st.defIndex);
  a.defIndex = st.defIndex;
  a.kind = byteOf(def.kind);
  a.phase = byteOf(st.phase);
  a.button = sim::slotButton(slot);
  a.phaseTicks = st.phaseTicks;
  a.cooldownTicks = st.cooldownTicks;
  a.activatedTick = st.activatedTick.value;
  a.activatedSubTick = st.activatedSubTick;
  a.defCooldownTicks = def.cooldownTicks;
  a.defStartupTicks = def.startupTicks;
  a.defActiveTicks = def.activeTicks;
  a.defRecoveryTicks = def.recoveryTicks;
  a.defPerfectWindowTicks = def.perfectWindowTicks;
  a.defSpeed = def.speed.raw;
  a.defParam0 = def.param0.raw;
  a.defParam1 = def.param1.raw;
  a.dirX = st.dir.x.raw;
  a.dirY = st.dir.y.raw;
}

void fillSelf(const sim::WorldState& world, const sim::MatchContent& content, PlayerId self,
              ObsSelf& o) {
  o.slot = self.value;
  // bot-abi.md §3, filled since architecture v0.31: the loadout index the
  // slot plays (v0.32: its role's in a team mode with roles).
  const uint8_t role = content.loadoutIndexFor(self);
  o.reservedRole = role < content.db.loadoutCount ? role : 0;
  if (self.isValid() && self.value < sim::kMaxPlayers) {
    o.respawnTicks = world.rules.respawnTicks[self.value];
    o.reservedTeam = content.teams[self.value];  // filled since v0.32
  }
  const sim::Player* p = world.playerBySlot(self);
  if (p == nullptr) return;
  const sim::Transform& t = p->transform;
  o.alive = 1;
  o.hp = p->hp;
  o.maxHp = p->stats.maxHp;
  o.ammo = p->ammo;
  o.ammoMax = p->stats.ammoMax;
  o.lastButtons = p->prevButtons;
  o.posX = t.pos.x.raw;
  o.posY = t.pos.y.raw;
  o.prevPosX = t.prevPos.x.raw;
  o.prevPosY = t.prevPos.y.raw;
  o.velX = t.vel.x.raw;
  o.velY = t.vel.y.raw;
  o.moveSpeed = p->stats.moveSpeed.raw;
  o.radius = p->stats.radius.raw;  // v0.34: the radius stat
  o.knockbackX = p->knockback.x.raw;
  o.knockbackY = p->knockback.y.raw;
  o.tags = p->tags.bits;
  o.hitStunTicks = p->hitStunTicks;
  o.knockbackTicks = p->knockbackTicks;
  o.invulnTicks = world.rules.invulnTicks[self.value];
  o.facing = angle8(t.facing);
  // bot-abi.md §3, filled since architecture v0.35: the cell and the facing
  // octant in a grid mode (the cone's direction, §8).
  if (content.mode.space == sim::Space::Grid && sim::grid::hasCells(content.arena)) {
    const Cell cell = sim::grid::cellOf(content.arena, t.pos);
    o.reservedCellX = static_cast<int16_t>(cell.col);
    o.reservedCellY = static_cast<int16_t>(cell.row);
    o.reservedFacingOctant = facingOctant(t.facing);
  }
  for (uint8_t s = 0; s < sim::kAbilitySlots; ++s) {
    fillAbility(p->abilities[s], content.db, s, o.abilities[s]);
  }
  const uint8_t count =
      p->modifiers.count < sim::kMaxModifiers ? p->modifiers.count : sim::kMaxModifiers;
  o.modifierCount = count;
  for (uint8_t i = 0; i < count; ++i) {
    o.modifiers[i].powerUp = p->modifiers.items[i].powerUp;
    o.modifiers[i].ticksLeft = p->modifiers.items[i].ticksLeft;
  }
}

void fillPlayers(const sim::WorldState& world, const sim::MatchContent& content, PlayerId self,
                 ObsPlayer (&players)[kObsMaxPlayers]) {
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    const PlayerId id{i};
    ObsPlayer& o = players[i];
    const bool present = sim::maskHas(world.rules.present, id);
    const sim::Player* p = world.playerBySlot(id);
    if (!present && p == nullptr) continue;  // an empty slot stays all zero
    o.present = present ? 1 : 0;
    o.relation = relationTo(content, self, id);
    o.reservedTeam = content.teams[i];  // filled since v0.32
    o.respawnTicks = world.rules.respawnTicks[i];
    if (p == nullptr) continue;
    const sim::Transform& t = p->transform;
    o.alive = 1;
    o.hp = p->hp;
    o.maxHp = p->stats.maxHp;
    o.ammo = p->ammo;
    o.ammoMax = p->stats.ammoMax;
    o.facing = angle8(t.facing);
    o.dashing = p->tags.has(sim::Tag::Dashing) ? 1 : 0;
    o.tags = p->tags.bits;
    o.posX = t.pos.x.raw;
    o.posY = t.pos.y.raw;
    o.velX = t.vel.x.raw;
    o.velY = t.vel.y.raw;
    o.hitStunTicks = p->hitStunTicks;
    o.invulnTicks = world.rules.invulnTicks[i];
    for (uint8_t s = 0; s < sim::kAbilitySlots; ++s) {
      const sim::AbilityState& st = p->abilities[s];
      o.abilityKind[s] = byteOf(content.db.abilityAt(st.defIndex).kind);
      o.abilityPhase[s] = byteOf(st.phase);
      o.abilityCooldownTicks[s] = st.cooldownTicks;
      o.abilityPhaseTicks[s] = st.phaseTicks;
    }
  }
}

void fillBananas(const sim::WorldState& world, const sim::MatchContent& content, PlayerId self,
                 ObsBanana (&bananas)[kObsMaxBananas]) {
  const sim::Player* me = world.playerBySlot(self);
  const sim::BoomerangDef& def = content.db.boomerang;
  world.boomerangs.forEachAlive([&](EntityId id, const sim::Boomerang& b) {
    ObsBanana& o = bananas[id.index];
    const sim::Transform& t = b.transform;
    o.valid = 1;
    o.phase = byteOf(b.phase);
    o.owner = b.owner.value;
    o.ownerRelation = relationTo(content, self, b.owner);
    o.damage = b.damage;
    o.headingBam = t.facing.bam;
    o.ticksAlive = b.ticksAlive;
    o.ticksInPhase = b.ticksInPhase;
    o.posX = t.pos.x.raw;
    o.posY = t.pos.y.raw;
    o.velX = t.vel.x.raw;
    o.velY = t.vel.y.raw;
    o.speed = (def.speed * b.speedMul).raw;
    o.radius = (def.radius * b.radiusMul).raw;
    if (me == nullptr) {
      o.approachTicks = kObsNoApproach;
    } else {
      const ClosestApproach a = closestApproach(t.pos - me->transform.pos, t.vel);
      o.approachTicks = a.ticks;
      o.approachDistance = a.distance.raw;
    }
  });
}

void fillPickups(const sim::WorldState& world, const sim::MatchContent& content,
                 ObsPickup (&pickups)[kObsMaxPickups]) {
  world.pickups.forEachAlive([&](EntityId id, const sim::Pickup& k) {
    ObsPickup& o = pickups[id.index];
    o.valid = 1;
    o.kind = byteOf(k.kind);
    o.powerUp = k.powerUpIndex;
    o.spawner = k.spawner;
    o.posX = k.pos.x.raw;
    o.posY = k.pos.y.raw;
    o.radius = content.db.pickup.radius.raw;
  });
}

[[nodiscard]] ObsBox boxOf(Vec2 min, Vec2 max) {
  return ObsBox{min.x.raw, min.y.raw, max.x.raw, max.y.raw};
}

void fillArena(const sim::ArenaDef& a, ObsArena& o) {
  o.halfExtents = ObsPoint{a.halfExtents.x.raw, a.halfExtents.y.raw};
  o.spawnPointCount = a.spawnPointCount;
  o.wallCount = a.wallCount;
  o.hazardCount = a.hazardCount;
  o.spawnerCount = a.pickupSpawnerCount;
  for (uint32_t i = 0; i < a.spawnPointCount && i < kObsMaxSpawnPoints; ++i) {
    o.spawnPoints[i] = ObsPoint{a.spawnPoints[i].x.raw, a.spawnPoints[i].y.raw};
  }
  for (uint32_t i = 0; i < a.wallCount && i < kObsMaxWalls; ++i) {
    o.walls[i] = boxOf(a.walls[i].min, a.walls[i].max);
  }
  for (uint32_t i = 0; i < a.hazardCount && i < kObsMaxHazards; ++i) {
    const sim::Hazard& h = a.hazards[i];
    o.hazards[i].box = boxOf(h.min, h.max);
    o.hazards[i].speedMul = h.speedMul.raw;
    o.hazards[i].kind = byteOf(h.kind);
  }
  for (uint32_t i = 0; i < a.pickupSpawnerCount && i < kObsMaxSpawners; ++i) {
    const sim::PickupSpawner& s = a.pickupSpawners[i];
    ObsSpawner& d = o.spawners[i];
    d.pos = ObsPoint{s.pos.x.raw, s.pos.y.raw};
    d.powerUpSet = s.powerUpSet;
    d.firstTicks = s.firstTicks;
    d.intervalTicks = s.intervalTicks;
    d.maxAlive = s.maxAlive;
  }
}

// Fog (v0.35): unseen entries are zeroed, seen ones flagged. An unseen
// monkey keeps what the roster and the scoreboard say anyway.
void applyFog(const sim::WorldState& world, const ObsVisibility& seen, PlayerId self,
              Observation& out) {
  out.header.schemaFlags |= kObsFlagFog;
  for (uint8_t i = 0; i < kObsMaxPlayers; ++i) {
    ObsPlayer& o = out.players[i];
    if (o.alive == 0) continue;
    if (i == self.value || (seen.players & (1u << i)) != 0) {
      o.reservedVisible = 1;
      continue;
    }
    const ObsPlayer kept = o;
    o = ObsPlayer{};
    o.present = kept.present;
    o.relation = kept.relation;
    o.reservedTeam = kept.reservedTeam;
    o.respawnTicks = kept.respawnTicks;
  }
  if (self.isValid() && self.value < kObsMaxPlayers) out.players[self.value].reservedVisible = 1;
  world.boomerangs.forEachAlive([&](EntityId id, const sim::Boomerang& /*b*/) {
    ObsBanana& o = out.bananas[id.index];
    if (seen.bananas[id.index] == id) {
      o.reservedVisible = 1;
    } else {
      o = ObsBanana{};
    }
  });
  world.pickups.forEachAlive([&](EntityId id, const sim::Pickup& /*k*/) {
    if (seen.pickups[id.index] != id) out.pickups[id.index] = ObsPickup{};
  });
}

void fillCalls(const ObservationContext& context, bool fog, const Observation& obs,
               ObsHeardCall (&calls)[kObsMaxHeardCalls]) {
  const uint32_t n = context.calls == nullptr ? 0u : context.callCount;
  for (uint32_t i = 0; i < n && i < kObsMaxHeardCalls; ++i) {
    const HeardCall& h = context.calls[i];
    ObsHeardCall& o = calls[i];
    o.valid = 1;
    o.slot = h.slot;
    o.symbol = h.symbol;
    o.tick = h.tick;
    const bool seen = !fog || (h.slot < kObsMaxPlayers && obs.players[h.slot].reservedVisible != 0);
    const bool cell = h.hasCell && seen;
    o.cellX = cell ? static_cast<int16_t>(h.cell.col) : kObsNoCell;
    o.cellY = cell ? static_cast<int16_t>(h.cell.row) : kObsNoCell;
  }
}

}  // namespace

void ObsVisibility::add(const Sight& sight, const sim::WorldState& world) {
  players = static_cast<uint16_t>(players | sight.players);
  world.boomerangs.forEachAlive([&](EntityId id, const sim::Boomerang& /*b*/) {
    if ((sight.bananas >> id.index & 1u) != 0) bananas[id.index] = id;
  });
  world.pickups.forEachAlive([&](EntityId id, const sim::Pickup& /*k*/) {
    if ((sight.pickups >> id.index & 1u) != 0) pickups[id.index] = id;
  });
}

ClosestApproach closestApproach(Vec2 rel, Vec2 vel, uint16_t horizon) {
  const int64_t rx = rel.x.raw;
  const int64_t ry = rel.y.raw;
  const int64_t vx = vel.x.raw;
  const int64_t vy = vel.y.raw;
  const int64_t vv = (vx * vx) + (vy * vy);
  const int64_t rv = (rx * vx) + (ry * vy);
  // |rel + k·vel|² = |rel|² + 2k(rel·vel) + k²|vel|² is a convex parabola in k
  // with its vertex at t* = -(rel·vel) / |vel|²; the integer minimiser is
  // floor(t*) or floor(t*) + 1, clamped to the horizon.
  int64_t best = 0;
  if (vv != 0 && rv < 0) {
    const int64_t lo = (-rv) / vv;  // floor(t*), both operands positive
    if (lo >= int64_t{horizon}) {
      best = int64_t{horizon};
    } else {
      best = lo;
      if (squaredAt(rx, ry, vx, vy, lo + 1) < squaredAt(rx, ry, vx, vy, lo)) best = lo + 1;
    }
  }
  return ClosestApproach{static_cast<uint16_t>(best),
                         lengthOfSquaredRaw(squaredAt(rx, ry, vx, vy, best))};
}

void buildObservation(const sim::WorldState& world, const sim::MatchContent& content,
                      uint32_t contentHash, PlayerId self, uint8_t inputDelayTicks,
                      Observation& out) {
  buildObservation(world, content, contentHash, self, inputDelayTicks, ObservationContext{}, out);
}

void buildObservation(const sim::WorldState& world, const sim::MatchContent& content,
                      uint32_t contentHash, PlayerId self, uint8_t inputDelayTicks,
                      const ObservationContext& context, Observation& out) {
  out = Observation{};
  out.header.version = kObservationVersion;
  out.header.size = sizeof(Observation);
  // bot-abi.md §3.3 (version 2, v0.79): every zone of the arena.
  static_assert(sim::kMaxArenaZones <= kObsMaxZones);
  out.zones.count = std::min<uint8_t>(content.arena.zoneCount, sim::kMaxArenaZones);
  for (uint8_t k = 0; k < out.zones.count; ++k) {
    const sim::Zone& z = content.arena.zones[k];
    out.zones.zone[k] = ObsZone{z.min.x.raw, z.min.y.raw, z.max.x.raw, z.max.y.raw};
  }
  fillMatch(world, content, contentHash, inputDelayTicks, out.match);
  fillRules(content.db, out.rules);
  fillSelf(world, content, self, out.self);
  fillPlayers(world, content, self, out.players);
  fillBananas(world, content, self, out.bananas);
  fillPickups(world, content, out.pickups);
  fillArena(content.arena, out.arena);
  const bool fog = content.mode.vision.fog();
  if (fog) {
    if (context.visibility != nullptr) {
      applyFog(world, *context.visibility, self, out);
    } else {
      ObsVisibility now;
      now.add(visionOf(world, content, self), world);
      applyFog(world, now, self, out);
    }
  }
  fillCalls(context, fog, out, out.reservedHeardCalls);
}

void buildObservation(const sim::WorldState& world, const sim::MatchContent& content, PlayerId self,
                      uint8_t inputDelayTicks, Observation& out) {
  buildObservation(world, content, sim::contentHash(content.db), self, inputDelayTicks, out);
}

}  // namespace kfm::ai
