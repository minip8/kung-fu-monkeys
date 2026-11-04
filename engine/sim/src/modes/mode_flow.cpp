// The match flow every mode shares (modes/mode_flow.hpp).
#include "modes/mode_flow.hpp"

#include "kfm/sim/constants.hpp"
#include "systems/modifier_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

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
  p.stats = Stats{content.db.player.moveSpeed, content.db.player.maxHp, content.db.player.ammoMax};
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
  uint8_t& score = r.scores[winner.value];
  ++score;
  if (content.teamScore(r.scores, content.teamOf(winner)) >= content.mode.roundsToWin) {
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

PlayerId scoreLeader(const MatchRules& r, uint8_t& best) {
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

PlayerId teamScoreLeader(const WorldState& s, const MatchContent& content, uint16_t& best) {
  const MatchRules& r = s.rules;
  best = 0;
  bool any = false;
  bool tied = false;
  uint8_t leaderTeam = 0;
  const uint16_t teams = teamsPresent(s, content);
  for (uint8_t t = 0; t < kMaxPlayers; ++t) {
    if ((teams & (1u << t)) == 0) continue;
    const uint16_t score = content.teamScore(r.scores, t);
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
