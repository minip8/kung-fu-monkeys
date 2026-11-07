// The match flow every mode shares (architecture §5.3, v0.23): spawning,
// respawning, ending a round or the match, the clock. Internal to
// engine/sim/src: RulesSystem, LifecycleSystem and the mode behaviours.
#pragma once

#include "kfm/core/types.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/world_state.hpp"

#include <cstdint>

namespace kfm::sim {

// A fresh monkey for slot `id` at `pos` (full hp, starting ammo, the slot's
// loadout (its role's in a team mode), the mode's baseline power-up, facing
// the arena centre). Keeps p.prevButtons so a button held across a round
// boundary is not a fresh press.
void resetPlayer(Player& p, PlayerId id, Vec2 pos, const MatchContent& content);

// The spawn points slot `id` may use (v0.32): its team's spawn group
// (team mod ArenaDef::spawnGroups), or every point with one group.
[[nodiscard]] SpawnRange spawnRangeOf(const MatchContent& content, PlayerId id);

// Respawns the dead slot `id` (LifecycleSystem): at the spawn point of its
// group (spawnRangeOf) farthest from every alive monkey, the world RNG
// breaking ties, with the mode's spawn
// protection (Tag::Invulnerable for respawnInvulnTicks). Pushes
// PlayerRespawned. Returns false if the slot could not be spawned.
bool respawnPlayer(WorldState& s, const MatchContent& content, PlayerId id, EventSink& sink);

// Whether any monkey is falling down a pit (v0.57). The round kinds (rounds,
// survival, team rounds) end no round while one is: it is still in the pool
// but already out, so the round waits the fall out and then judges.
[[nodiscard]] bool anyFalling(const WorldState& s);

// The only monkey left, or invalid when none or several are.
[[nodiscard]] PlayerId soleSurvivor(const WorldState& s);

// Ends the current round with `winner` (invalid: a draw): RoundEnded, then
// a round win for the winner's slot, then MatchEnded once the winner's team
// score (content.teamScore: the slot's own score in a free-for-all, where
// every team is one slot) reaches content.mode.roundsToWin.
void endRound(WorldState& s, const MatchContent& content, PlayerId winner, EventSink& sink);

// Ends the match at once with `winner` (invalid: a draw): RoundEnded, then
// MatchEnded. Scores are left as they are.
void endMatch(WorldState& s, PlayerId winner, EventSink& sink);

// The present slot with the strictly highest score, or invalid on a tie
// (or with nobody present). `best` receives the highest score.
[[nodiscard]] PlayerId scoreLeader(const MatchRules& r, uint8_t& best);

// Teams (v0.32): bit t set when a present slot (teamsPresent) or a monkey
// still in the arena (teamsAlive) belongs to team t.
[[nodiscard]] uint16_t teamsPresent(const WorldState& s, const MatchContent& content);
[[nodiscard]] uint16_t teamsAlive(const WorldState& s, const MatchContent& content);
// The lowest slot of team `team` with a monkey still in the arena, or
// invalid.
[[nodiscard]] PlayerId lowestAliveOf(const WorldState& s, const MatchContent& content,
                                     uint8_t team);
// The team (among those with a present slot) with the strictly highest
// team score, as its present slot with the highest own score (the lowest
// slot on a tie); invalid on a tie for the lead or with nobody present.
// `best` receives the highest team score.
[[nodiscard]] PlayerId teamScoreLeader(const WorldState& s, const MatchContent& content,
                                       uint16_t& best);

// Deathmatch roster (v0.23, shared with team deathmatch in v0.32): a present
// slot without a monkey and without a pending respawn (a late joiner) is
// scheduled like a respawn; a dead slot that left loses its pending timers
// and, when forfeitScore, its score and deaths.
void deathmatchRoster(WorldState& s, const MatchContent& content, bool forfeitScore);
// The respawning kinds' bookkeeping of an elimination (v0.33, hill and
// capture): one more death for `victim` and its respawn due in
// respawnDelay(); nobody scores.
void countDeath(WorldState& s, const MatchContent& content, PlayerId victim);
// Deathmatch respawn delay (validation keeps respawnTicks >= 1).
[[nodiscard]] uint16_t respawnDelay(const ModeDef& mode);

// The mode clock runs over the Playing phase (ticksInPhase). Pushes
// TimerWarning at 10, 3, 2 and 1 seconds left; nothing for limit 0.
void clockWarnings(const WorldState& s, uint32_t limitTicks, EventSink& sink);
[[nodiscard]] constexpr bool clockExpired(const MatchRules& r, uint32_t limitTicks) {
  return limitTicks != 0 && r.ticksInPhase >= limitTicks;
}

}  // namespace kfm::sim
