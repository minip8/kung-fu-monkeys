// The match flow every mode shares (modes/mode_flow.hpp).
#include "modes/mode_flow.hpp"

#include "kfm/sim/constants.hpp"
#include "kfm/sim/modes/registry.hpp"
#include "systems/modifier_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>

namespace kfm::sim {
namespace {

// Deterministic facing at spawn: toward the arena centre along x.
[[nodiscard]] Angle facingTowardCentre(Vec2 pos) {
  return pos.x > Fixed::zero() ? Angle::fromBam(Angle::kHalf) : Angle{};
}

// Squared distance (raw Q32.32) from `pos` to the nearest monkey still in
// play, or the maximum when there is none.
[[nodiscard]] uint64_t nearestEnemySq(const WorldState& s, Vec2 pos) {
  uint64_t best = std::numeric_limits<uint64_t>::max();
  s.players.forEachAlive([&](EntityId /*id*/, const Player& p) {
    if (p.tags.has(Tag::Eliminated)) return;
    best = std::min(best, lengthSqRaw(p.transform.pos - pos));
  });
  return best;
}

// Index of the spawn point in [range.begin, range.end) farthest from the
// nearest monkey; ties (all of them in an empty arena) are broken by the
// world RNG, which is drawn once per respawn whatever the tie count. With
// one spawn group the range is the whole list (v0.32).
[[nodiscard]] uint8_t farthestSpawnPoint(WorldState& s, const ArenaDef& arena, SpawnRange range) {
  std::array<uint64_t, kMaxPlayers> dist{};
  uint64_t best = 0;
  for (uint8_t k = range.begin; k < range.end; ++k) {
    dist[k] = nearestEnemySq(s, arena.spawnPoints[k]);
    best = std::max(best, dist[k]);
  }
  uint32_t ties = 0;
  for (uint8_t k = range.begin; k < range.end; ++k) {
    if (dist[k] == best) ++ties;
  }
  uint32_t pick = s.rng.below(ties > 0 ? ties : 1);
  for (uint8_t k = range.begin; k < range.end; ++k) {
    if (dist[k] != best) continue;
    if (pick == 0) return k;
    --pick;
  }
  return range.begin;
}

}  // namespace

void resetPlayer(Player& p, PlayerId id, Vec2 pos, const MatchContent& content) {
  const uint8_t prevButtons = p.prevButtons;
  p = Player{};
  p.id = id;
  p.prevButtons = prevButtons;
  p.transform.pos = pos;
  p.transform.prevPos = pos;
  p.transform.facing = facingTowardCentre(pos);
  p.stats = Stats{content.db.player.moveSpeed, content.db.player.maxHp, content.db.player.ammoMax,
                  content.db.player.radius};
  p.hp = content.db.player.maxHp;
  p.ammo = content.db.player.ammoStart;
  const std::array<uint8_t, kAbilitySlots>& loadout = content.loadout(id);
  for (uint8_t slot = 0; slot < kAbilitySlots; ++slot) {
    p.abilities[slot].defIndex = loadout[slot];
  }
  grantBaseline(p, content);  // v0.32: a no-op without ModeDef::baseline
}

SpawnRange spawnRangeOf(const MatchContent& content, PlayerId id) {
  const ArenaDef& arena = content.arena;
  if (arena.spawnGroups <= 1) return SpawnRange{0, arena.spawnPointCount};
  return spawnGroupRange(arena, content.teamOf(id));
}

bool respawnPlayer(WorldState& s, const MatchContent& content, PlayerId id, EventSink& sink) {
  if (!id.isValid() || id.value >= kMaxPlayers || s.players.isAliveIndex(id.value)) return false;
  const ArenaDef& arena = content.arena;
  const SpawnRange range = spawnRangeOf(content, id);
  const uint8_t point = range.end > range.begin ? farthestSpawnPoint(s, arena, range) : 0;
  const Vec2 pos = arena.spawnPointCount > 0 ? arena.spawnPoints[point] : Vec2::zero();
  const EntityId e = s.players.spawnAt(id.value);
  if (!e.isValid()) return false;
  Player& p = s.players.items[e.index];
  resetPlayer(p, id, pos, content);
  const uint16_t invuln = content.mode.respawnInvulnTicks;
  s.rules.invulnTicks[id.value] = invuln;
  if (invuln > 0) p.tags.set(Tag::Invulnerable);
  sink.push(SimEvent{EventKind::PlayerRespawned, s.tick, id, e, point, 0});
  return true;
}

bool anyFalling(const WorldState& s) {
  bool falling = false;
  s.players.forEachAlive([&](EntityId /*id*/, const Player& p) {
    if (p.tags.has(Tag::Falling) && !p.tags.has(Tag::Eliminated)) falling = true;
  });
  return falling;
}

namespace {

// A power-up that tops ammo up when taken (rules/powerup.cpp).
[[nodiscard]] bool laysBananas(const PowerUpDef& def) {
  for (uint8_t g = 0; g < def.grantCount && g < kMaxPowerUpGrants; ++g) {
    const PowerUpGrant& grant = def.grants[g];
    if (grant.kind == GrantKind::Stat && grant.stat.stat == StatKind::AmmoMax &&
        grant.stat.add > Fixed::zero()) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool bananaLeft(const WorldState& s, const MatchContent& content) {
  if (content.mode.bananaRegrowTicks > 0) return true;  // an empty hand grows one
  bool left = false;
  s.players.forEachAlive([&](EntityId /*id*/, const Player& p) {
    if (!p.tags.has(Tag::Eliminated) && p.ammo > 0) left = true;
  });
  s.boomerangs.forEachAlive([&](EntityId /*id*/, const Boomerang& b) {
    if (b.phase != BoomerangPhase::Sinking) left = true;
  });
  const ContentDB& db = content.db;
  s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& k) {
    if (k.kind == PickupKind::Banana) left = true;
    if (k.kind == PickupKind::PowerUp && k.powerUpIndex < db.powerUpCount &&
        laysBananas(db.powerUpAt(k.powerUpIndex))) {
      left = true;
    }
  });
  if (left) return true;
  // A spawner with a beat still to come (LifecycleSystem::runSpawners) that
  // may draw a power-up laying bananas.
  const ArenaDef& arena = content.arena;
  for (uint8_t i = 0; i < arena.pickupSpawnerCount && i < kMaxPickupSpawners; ++i) {
    const PickupSpawner& sp = arena.pickupSpawners[i];
    if (sp.intervalTicks == 0 && s.rules.ticksInPhase > sp.firstTicks) continue;
    const uint32_t all = db.powerUpCount >= 32 ? UINT32_MAX : (uint32_t{1} << db.powerUpCount) - 1u;
    const uint32_t set =
        (sp.powerUpSet == 0 ? all : (uint32_t{sp.powerUpSet} & all)) & content.powerUps;
    for (uint8_t d = 0; d < db.powerUpCount && d < kMaxPowerUps; ++d) {
      if ((set & (1u << d)) != 0 && laysBananas(db.powerUpAt(d))) return true;
    }
  }
  return false;
}

[[nodiscard]] bool arenaHasPit(const ArenaDef& arena) {
  for (uint8_t i = 0; i < arena.hazardCount && i < kMaxArenaHazards; ++i) {
    if (arena.hazards[i].kind == HazardKind::Pit) return true;
  }
  return false;
}

}  // namespace

bool roundRanDry(WorldState& s, const MatchContent& content) {
  uint16_t& dry = s.rules.dryTicks;
  if (bananaLeft(s, content)) {
    dry = 0;
    return false;
  }
  const uint16_t grace = arenaHasPit(content.arena) ? content.mode.dryRoundTicks : uint16_t{0};
  if (dry < grace) ++dry;
  return dry >= grace;
}

PlayerId soleSurvivor(const WorldState& s) {
  PlayerId winner = PlayerId::invalid();
  if (s.players.size() != 1) return winner;
  s.players.forEachAlive([&](EntityId /*id*/, const Player& p) { winner = p.id; });
  return winner;
}

void endRound(WorldState& s, const MatchContent& content, PlayerId winner, EventSink& sink) {
  MatchRules& r = s.rules;
  r.lastRoundWinner = winner;
  r.ticksInPhase = 0;
  r.phase = MatchPhase::RoundEnded;
  sink.push(SimEvent{EventKind::RoundEnded, s.tick, winner, EntityId{}, r.round, 0});
  if (!winner.isValid()) return;  // draw: nobody scores
  int8_t& score = r.scores[winner.value];
  if (score < INT8_MAX) ++score;
  if (std::cmp_greater_equal(content.teamScore(r.scores, content.teamOf(winner)),
                             content.mode.roundsToWin)) {
    r.phase = MatchPhase::MatchEnded;
    sink.push(SimEvent{EventKind::MatchEnded, s.tick, winner, EntityId{}, 0, 0});
  }
}

void endMatch(WorldState& s, PlayerId winner, EventSink& sink) {
  MatchRules& r = s.rules;
  r.lastRoundWinner = winner;
  r.ticksInPhase = 0;
  r.phase = MatchPhase::MatchEnded;
  sink.push(SimEvent{EventKind::RoundEnded, s.tick, winner, EntityId{}, r.round, 0});
  sink.push(SimEvent{EventKind::MatchEnded, s.tick, winner, EntityId{}, 0, 0});
}

PlayerId scoreLeader(const MatchRules& r, int8_t& best) {
  best = 0;
  PlayerId leader = PlayerId::invalid();
  bool tied = false;
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    if (!maskHas(r.present, PlayerId{i})) continue;
    if (!leader.isValid() || r.scores[i] > best) {
      best = r.scores[i];
      leader = PlayerId{i};
      tied = false;
    } else if (r.scores[i] == best) {
      tied = true;
    }
  }
  return tied ? PlayerId::invalid() : leader;
}

uint16_t teamsPresent(const WorldState& s, const MatchContent& content) {
  uint16_t mask = 0;
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const PlayerId id{i};
    if (maskHas(s.rules.present, id))
      mask = static_cast<uint16_t>(mask | (1u << content.teamOf(id)));
  }
  return mask;
}

uint16_t teamsAlive(const WorldState& s, const MatchContent& content) {
  uint16_t mask = 0;
  s.players.forEachAlive([&](EntityId /*id*/, const Player& p) {
    if (p.tags.has(Tag::Eliminated) || p.id.value >= kMaxPlayers) return;
    mask = static_cast<uint16_t>(mask | (1u << content.teamOf(p.id)));
  });
  return mask;
}

PlayerId lowestAliveOf(const WorldState& s, const MatchContent& content, uint8_t team) {
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const Player* p = s.playerBySlot(PlayerId{i});
    if (p != nullptr && !p->tags.has(Tag::Eliminated) && content.teamOf(p->id) == team) {
      return p->id;
    }
  }
  return PlayerId::invalid();
}

PlayerId teamScoreLeader(const WorldState& s, const MatchContent& content, int16_t& best) {
  const MatchRules& r = s.rules;
  best = 0;
  bool any = false;
  bool tied = false;
  uint8_t leaderTeam = 0;
  const uint16_t teams = teamsPresent(s, content);
  for (uint8_t t = 0; t < kMaxPlayers; ++t) {
    if ((teams & (1u << t)) == 0) continue;
    const int16_t score = content.teamScore(r.scores, t);
    if (!any || score > best) {
      best = score;
      leaderTeam = t;
      any = true;
      tied = false;
    } else if (score == best) {
      tied = true;
    }
  }
  if (!any || tied) return PlayerId::invalid();
  PlayerId leader = PlayerId::invalid();
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const PlayerId id{i};
    if (!maskHas(r.present, id) || content.teamOf(id) != leaderTeam) continue;
    if (!leader.isValid() || r.scores[i] > r.scores[leader.value]) leader = id;
  }
  return leader;
}

void addScore(WorldState& s, PlayerId id, int delta, ScoreReason reason, EventSink& sink) {
  if (delta == 0 || !id.isValid() || id.value >= kMaxPlayers) return;
  int8_t& score = s.rules.scores[id.value];
  score = static_cast<int8_t>(std::clamp<int>(score + delta, INT8_MIN, INT8_MAX));
  sink.push(SimEvent{EventKind::ScoreChanged, s.tick, id, EntityId{},
                     static_cast<uint32_t>(int32_t{score}), static_cast<uint32_t>(reason)});
}

void reportElimination(WorldState& s, const MatchContent& content, PlayerId victim, PlayerId killer,
                       EventSink& sink) {
  modeBehaviorOf(content).onEliminated(s, content, victim, killer, sink);
  if (!killer.isValid() || killer == victim) {
    addScore(s, victim, content.mode.suicideScore, ScoreReason::Suicide, sink);
  }
}

void countDeath(WorldState& s, const MatchContent& content, PlayerId victim) {
  if (!victim.isValid() || victim.value >= kMaxPlayers) return;
  MatchRules& r = s.rules;
  if (r.deaths[victim.value] < UINT8_MAX) ++r.deaths[victim.value];
  r.respawnTicks[victim.value] = respawnDelay(content.mode);
}

uint16_t respawnDelay(const ModeDef& mode) {
  return std::max<uint16_t>(mode.respawnTicks, 1);
}

void deathmatchRoster(WorldState& s, const MatchContent& content, bool forfeitScore) {
  MatchRules& r = s.rules;
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    if (s.players.isAliveIndex(i)) continue;
    if (maskHas(r.present, PlayerId{i})) {
      if (r.respawnTicks[i] == 0) r.respawnTicks[i] = respawnDelay(content.mode);
    } else {
      if (forfeitScore) {
        r.scores[i] = 0;
        r.deaths[i] = 0;
      }
      r.respawnTicks[i] = 0;
      r.invulnTicks[i] = 0;
    }
  }
}

void clockWarnings(const WorldState& s, uint32_t limitTicks, EventSink& sink) {
  const MatchRules& r = s.rules;
  if (limitTicks == 0 || r.ticksInPhase >= limitTicks) return;
  const uint32_t left = limitTicks - r.ticksInPhase;
  for (const uint32_t seconds : {10u, 3u, 2u, 1u}) {
    if (left == seconds * kTickHz) {
      sink.push(
          SimEvent{EventKind::TimerWarning, s.tick, PlayerId::invalid(), EntityId{}, seconds, 0});
    }
  }
}

}  // namespace kfm::sim
