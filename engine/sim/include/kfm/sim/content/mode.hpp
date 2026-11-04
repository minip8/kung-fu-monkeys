// Game modes (architecture §5.3, §5.10, v0.23). A ModeDef is content: a kind
// that selects the mode's behaviour in the RulesSystem function table
// (modes/registry.hpp) plus the numbers that behaviour reads. Every kind
// shares the phase machine (Lobby, RoundStarting, Playing, RoundEnded,
// MatchEnded) and the round timers; a kind ignores the numbers it does not
// use.
#pragma once

#include "kfm/sim/constants.hpp"
#include "kfm/sim/content/name.hpp"

#include <array>
#include <cstdint>

namespace kfm::sim {

enum class ModeKind : uint8_t {
  // Free-for-all rounds: the last monkey standing wins the round, the first
  // to roundsToWin round wins takes the match (the M1 rules).
  Rounds = 0,
  // One long round on a clock: an elimination scores for the attacker, the
  // eliminated respawn after respawnTicks with respawnInvulnTicks of spawn
  // protection; the first to scoreToWin wins, else the leader when
  // timeLimitTicks runs out (a tie for the lead is a draw).
  Deathmatch,
  // Rounds on a clock: the last monkey standing wins the round; when a
  // round's timeLimitTicks runs out, the survivor with the most hp wins it,
  // then the one with the fewest deaths this match, else it is a draw.
  Survival,
  kCount,  // NOLINT(readability-identifier-naming): table-size sentinel, not a kind
};

struct ModeDef {
  ContentName name{};
  ModeKind kind{ModeKind::Rounds};
  // Rounds, Survival: round wins needed to take the match.
  uint8_t roundsToWin{3};
  // Shared: the pause after a round ends before the next starts, and the
  // countdown at round start (players spawned, inputs ignored).
  uint16_t roundEndTicks{120};
  uint16_t roundStartTicks{60};
  // Deathmatch: length of the match's Playing phase. Survival: length of
  // each round. 0 = no clock. Rounds: unused.
  uint32_t timeLimitTicks{0};
  // Deathmatch: eliminations needed to win (at least 1).
  uint8_t scoreToWin{10};
  // Deathmatch: ticks from an elimination to the respawn (at least 1), and
  // the spawn protection after it (Tag::Invulnerable; 0 = none).
  uint16_t respawnTicks{120};
  uint16_t respawnInvulnTicks{90};
  // v0.21's per-mode loadout (abilities, hasAbilities) moved to its own axis
  // in v0.31: LoadoutDef and MatchConfig::loadout (content/loadout.hpp).
};

}  // namespace kfm::sim
