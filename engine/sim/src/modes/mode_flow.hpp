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

// A fresh monkey for slot `id` at `pos` (full hp, starting ammo, the
// match's loadout, facing the arena centre). Keeps p.prevButtons so a button held
// across a round boundary is not a fresh press.
void resetPlayer(Player& p, PlayerId id, Vec2 pos, const MatchContent& content);

// Respawns the dead slot `id` (LifecycleSystem): at the spawn point farthest
// from every alive monkey, the world RNG breaking ties, with the mode's spawn
// protection (Tag::Invulnerable for respawnInvulnTicks). Pushes
// PlayerRespawned. Returns false if the slot could not be spawned.
bool respawnPlayer(WorldState& s, const MatchContent& content, PlayerId id, EventSink& sink);

// The only monkey left, or invalid when none or several are.
[[nodiscard]] PlayerId soleSurvivor(const WorldState& s);

// Ends the current round with `winner` (invalid: a draw): RoundEnded, then
// a round win, then MatchEnded once the winner has content.mode.roundsToWin.
void endRound(WorldState& s, const MatchContent& content, PlayerId winner, EventSink& sink);

// Ends the match at once with `winner` (invalid: a draw): RoundEnded, then
// MatchEnded. Scores are left as they are.
void endMatch(WorldState& s, PlayerId winner, EventSink& sink);

// The present slot with the strictly highest score, or invalid on a tie
// (or with nobody present). `best` receives the highest score.
[[nodiscard]] PlayerId scoreLeader(const MatchRules& r, uint8_t& best);

// The mode clock runs over the Playing phase (ticksInPhase). Pushes
// TimerWarning at 10, 3, 2 and 1 seconds left; nothing for limit 0.
void clockWarnings(const WorldState& s, uint32_t limitTicks, EventSink& sink);
[[nodiscard]] constexpr bool clockExpired(const MatchRules& r, uint32_t limitTicks) {
  return limitTicks != 0 && r.ticksInPhase >= limitTicks;
}

}  // namespace kfm::sim
