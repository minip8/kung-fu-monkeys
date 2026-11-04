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
  // v0.32, the team kinds (MatchConfig::teams; ModeDef::teams is the team
  // count the hosts distribute the slots over). Team rounds: a round ends
  // when at most one team has monkeys left (two or more teams present); that
  // team wins it, credited to its lowest surviving slot, and the first team
  // whose slots' round wins sum to roundsToWin takes the match.
  TeamRounds,
  // Team deathmatch: deathmatch's clock, respawns and scoring, won by the
  // first team whose eliminations sum to scoreToWin, else the leading team
  // when timeLimitTicks runs out (a tied lead is a draw). Eliminating an
  // ally scores nothing.
  TeamDeathmatch,
  // v0.33, the objective kinds (team modes on an arena with zones). Hill:
  // one long round on a clock with deathmatch's respawns; a team scores a
  // point for every hillTicks it holds zone 0 alone (a contested or empty
  // zone holds every clock, nothing resets), the first team to scoreToWin
  // wins, else the leading team when timeLimitTicks runs out.
  Hill,
  // Capture: hill's round, clock and respawns; team t's flag starts in zone
  // t, an enemy carries it into its own team's zone while its own flag is
  // home to score; an eliminated carrier drops it where it stood, a dropped
  // flag left alone for flagReturnTicks goes home.
  Capture,
  kCount,  // NOLINT(readability-identifier-naming): table-size sentinel, not a kind
};

// Whether a kind plays one long round on a clock with respawns, its
// MatchRules::scores counting points (eliminations, hill points, captures)
// rather than round wins (v0.33; the hosts' HUD and tools read it).
[[nodiscard]] constexpr bool scoresPoints(ModeKind k) {
  return k == ModeKind::Deathmatch || k == ModeKind::TeamDeathmatch || k == ModeKind::Hill ||
         k == ModeKind::Capture;
}

// How monkeys move in a mode (architecture §5.12, v0.29): the SpaceBehavior
// table (kfm/sim/spaces/registry.hpp) is indexed by it.
enum class Space : uint8_t {
  Continuous = 0,  // the sim as built: stick velocity, walls slide
  Grid,            // one of eight neighbouring cells per turn of turnTicks
  kCount,          // NOLINT(readability-identifier-naming): table-size sentinel, not a space
};

// ModeDef::baseline when the mode grants no baseline power-up (v0.32).
inline constexpr uint8_t kNoBaseline = 0xFF;

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
  // v0.32: 0 = free-for-all, else the number of teams (2..kMaxPlayers) the
  // hosts split the slots into (MatchConfig::teams, sim::teamsFor()).
  uint8_t teams{0};
  // v0.32: a power-up (index into ContentDB::powerUps) every monkey holds
  // from its spawn on with no expiry (a permanent ModifierList entry,
  // kPermanentModifierTicks), or kNoBaseline. How a mode switches a rule
  // row on: friendly fire is a baseline that grants Tag::FriendlyFire.
  uint8_t baseline{kNoBaseline};
  // v0.32, team modes only: loadouts (indices into ContentDB::loadouts)
  // handed out by a slot's position within its team, round-robin over
  // roles[0..roleCount). None: every slot plays the match's loadout.
  std::array<uint8_t, kMaxPlayers> roles{};
  uint8_t roleCount{0};
  // v0.33, hill: ticks of holding the zone alone that score a point (>= 1).
  uint16_t hillTicks{300};
  // v0.33, capture: ticks a dropped flag lies untouched before it goes home (>= 1).
  uint16_t flagReturnTicks{600};

  [[nodiscard]] constexpr bool hasBaseline() const { return baseline != kNoBaseline; }
};

}  // namespace kfm::sim
