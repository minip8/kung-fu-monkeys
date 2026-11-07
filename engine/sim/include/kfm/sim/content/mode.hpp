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

// What a bot sees (architecture §8, v0.35): the observation builder's fog,
// never read by the sim. All: everything (every continuous mode, and every
// mode before v0.35). Team: the union of the allies' sight. Own: the
// monkey's own sight alone. The radii are in cells (Chebyshev), so a fog
// kind needs an arena with cells (ContentDB::compatible).
enum class VisionKind : uint8_t {
  All = 0,
  Team,
  Own,
  kCount,  // NOLINT(readability-identifier-naming): table-size sentinel, not a kind
};

// Upper bounds the loader enforces (the builder's cone and lines stay small).
inline constexpr uint8_t kMaxVisionCone = 32;
inline constexpr uint8_t kMaxVisionRadius = 16;  // peripheral, focus, banana sight
inline constexpr uint8_t kMaxVisionHearing = 32;

struct VisionDef {
  VisionKind kind{VisionKind::All};
  // Chebyshev radius around the monkey's cell seen whatever the facing.
  uint8_t peripheral{0};
  // The cone: the cells within 45 degrees of the facing octant, inclusive,
  // out to Chebyshev distance `cone`, each behind a clear line of sight.
  uint8_t cone{0};
  // Extra cone length for a monkey at rest that did not move last tick.
  uint8_t focus{0};
  // Chebyshev radius within which a banana in flight is heard (seen) whatever
  // the facing and the walls.
  uint8_t hearing{0};
  // Chebyshev radius around each of the monkey's own bananas in flight.
  uint8_t bananaSight{0};

  [[nodiscard]] constexpr bool fog() const { return kind != VisionKind::All; }
  constexpr bool operator==(const VisionDef&) const = default;
};

// v0.57, what becomes of a dead monkey's bananas (ModeDef::deathBananas).
// Drop: the ones away from it come down where they are and the held ones
// scatter around it (the round kinds, where a round ends when none is left).
// Vanish: every banana it owns, held or in flight, goes with it (the respawn
// kinds: the respawn hands out the starting banana again).
enum class DeathBananas : uint8_t {
  Drop = 0,
  Vanish,
  kCount,  // NOLINT(readability-identifier-naming): table-size sentinel, not a choice
};

// ModeDef::baseline when the mode grants no baseline power-up (v0.32).
inline constexpr uint8_t kNoBaseline = 0xFF;
// ModeDef::arena when the mode names no arena of its own (v0.34).
inline constexpr uint8_t kNoArena = 0xFF;

struct ModeDef {
  ContentName name{};
  ModeKind kind{ModeKind::Rounds};
  // Rounds, Survival: round wins needed to take the match.
  uint8_t roundsToWin{3};
  // Shared: the pause after a round ends before the next starts (the round
  // kinds; a clock kind ends the match with no pause), and the countdown at
  // round start (players spawned, inputs ignored).
  uint16_t roundEndTicks{180};
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
  // v0.34, the space (§5.12): how monkeys move. A grid mode plays only on an
  // arena with cells (ArenaDef::cellSize > 0, ContentDB::compatible).
  Space space{Space::Continuous};
  // Grid: the ticks of a turn (1..60): a monkey at rest takes a commitment
  // on every Playing tick with ticksInPhase % turnTicks == 0, and a walk
  // slides one cell over a whole turn.
  uint8_t turnTicks{15};
  // Grid: the cells a dash commits to (1..8), sliding over the dash def's
  // active ticks; a wall stops it on the last free cell.
  uint8_t dashCells{2};
  // v0.34: the arena (index into ContentDB::arenas) a host picks for the
  // mode when the match names none, or kNoArena: the first compatible one
  // (ContentDB::firstCompatibleArena). Never read by the sim.
  uint8_t arena{kNoArena};
  // v0.35, the squad contest (§8): what a bot sees. Defaults to everything.
  VisionDef vision{};
  // v0.57, the round kinds: a round in which no banana is left anywhere (none
  // in a hand, in flight or lying around, and no spawner that can still lay a
  // power-up that tops ammo up) ends like a clock-out, at once on an arena
  // without pits (nothing else can eliminate anyone) or after this many dry
  // ticks on one with pits (a monkey may still walk or be pushed in).
  uint16_t dryRoundTicks{180};
  // v0.57, the banana economy. What a death does to the monkey's bananas (a
  // pit fall always takes the held ones down; the ones in flight follow this
  // too, from the moment the fall begins).
  DeathBananas deathBananas{DeathBananas::Drop};
  // Points a point-scoring kind (scoresPoints) adds to a slot's score for a
  // perfect dash catch of an enemy's banana (the own one or an ally's scores
  // nothing), and for a suicide: an elimination with no killer (a pit fall).
  // 0 = none; a score may go below zero. The loader keeps both 0 elsewhere.
  int8_t catchScore{0};
  int8_t suicideScore{0};
  // A monkey that owns no banana (none in its hand, none of its own in
  // flight) for this many Playing ticks in a row grows one (BananaRegrown);
  // 0 = never. The clock stops while it falls and is not kept across a death.
  uint16_t bananaRegrowTicks{0};

  [[nodiscard]] constexpr bool hasBaseline() const { return baseline != kNoBaseline; }
  [[nodiscard]] constexpr bool hasArena() const { return arena != kNoArena; }
};

}  // namespace kfm::sim
