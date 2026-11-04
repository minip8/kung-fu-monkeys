// Observation v1 (architecture §8, normative byte layout in docs/bot-abi.md):
// the versioned, little-endian, fixed-layout POD every bot reads, written by
// the host into the module's obs buffer each tick and mirrored field for
// field by each SDK.
//
// Layout rules (they are what keeps the struct identical on GCC, Clang, MSVC
// and wasm32, checked by the static_asserts at the bottom):
//   - only fixed-width integers: no bool, no enum-typed field, no pointer,
//     no floating point (the enums below give the values a byte field holds);
//   - every field naturally aligned, every hole an explicit `pad*` byte, so
//     the compiler inserts nothing and alignof(Observation) is 4;
//   - arrays always at capacity, one `valid` (or `present`/`alive`) byte per
//     entry; an empty, absent or (with fog, later) unseen entry is all zero;
//   - every `reserved*` field is zero in v1 (the v0.29 teams, fog and calls,
//     §8 "Observation v1 reserves"), so the first fog or team mode fills them
//     without a schema bump.
//
// Units:
//   Q16.16  int32 raw fixed point: value / 65536 arena units (positions,
//           lengths, radii) or units per tick (velocities, speeds); a
//           multiplier (restitution, speedMul) is value / 65536 as well
//   angle8  uint8 1/256 turn, 0 = +x, counter-clockwise toward +y (the
//           PlayerInput encoding: a player's facing is its aim byte, exact)
//   bam     uint16 1/65536 turn, same orientation (a banana's heading, which
//           the sim steers at that precision)
//   ticks   60 per second; tick numbers are the sim's Tick
//   slot    player slot 0..9, 0xFF = none
//   tags    Tag bit set (bit i = kfm::sim::Tag value i: 0 Dashing,
//           1 PerfectDash, 2 HitStun, 3 Shielded, 4 Invulnerable,
//           5 Eliminated, 6 Slamming, 7 Parrying)
// The world coordinate frame is the sim's: the arena is the box
// [-halfExtents, +halfExtents] centred on the origin, +y up.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace kfm::ai {

inline constexpr uint32_t kObservationVersion = 1;

// Capacities (equal to the sim's pools and content tables; observation.cpp
// asserts it). Changing one is a schema change.
inline constexpr uint32_t kObsMaxPlayers = 10;
inline constexpr uint32_t kObsMaxBananas = 64;
inline constexpr uint32_t kObsMaxPickups = 32;
inline constexpr uint32_t kObsAbilitySlots = 3;
inline constexpr uint32_t kObsMaxModifiers = 8;
inline constexpr uint32_t kObsMaxSpawnPoints = 10;
inline constexpr uint32_t kObsMaxWalls = 32;
inline constexpr uint32_t kObsMaxHazards = 16;
inline constexpr uint32_t kObsMaxSpawners = 8;
inline constexpr uint32_t kObsMaxHeardCalls = 16;
inline constexpr uint32_t kObsModeDataWords = 16;

// No player in a slot field.
inline constexpr uint8_t kObsNoSlot = 0xFF;
// ObsBanana::approachTicks when there is no approach to report (self not alive).
inline constexpr uint16_t kObsNoApproach = 0xFFFF;
// The horizon of the closest-approach search, in ticks.
inline constexpr uint16_t kObsApproachHorizonTicks = 600;

// Values of the byte fields (the enums of the sim, frozen for the ABI).
enum class ObsRelation : uint8_t { None = 0, Self = 1, Ally = 2, Enemy = 3 };
enum class ObsMatchPhase : uint8_t {
  Lobby = 0,
  RoundStarting = 1,
  Playing = 2,
  RoundEnded = 3,
  MatchEnded = 4
};
enum class ObsModeKind : uint8_t { Rounds = 0, Deathmatch = 1, Survival = 2 };
enum class ObsAbilityKind : uint8_t {
  None = 0,
  Dash = 1,
  Throw = 2,
  Blink = 3,
  Slam = 4,
  Parry = 5
};
enum class ObsAbilityPhase : uint8_t { Idle = 0, Startup = 1, Active = 2, Recovery = 3 };
enum class ObsBananaPhase : uint8_t { Outbound = 0, Returning = 1, Grounded = 2 };
enum class ObsPickupKind : uint8_t { Banana = 0, PowerUp = 1 };
enum class ObsHazardKind : uint8_t { Pit = 0, Slow = 1 };

// ObsHeader::schemaFlags bits. None is set in v1 (full information).
inline constexpr uint32_t kObsFlagFog = 1u << 0;  // reserved: visibility fields are meaningful

// --- blocks -------------------------------------------------------------------

struct ObsHeader {
  uint32_t version;      // kObservationVersion
  uint32_t size;         // sizeof(Observation) in bytes
  uint32_t schemaFlags;  // kObsFlag* bits; 0 in v1
  uint32_t reserved0;
};

// The match: mode, clock, roster and scores (MatchRules, the ModeDef).
struct ObsMatch {
  uint32_t tick;          // ticks: the tick this observation is for (the input's tick)
  uint32_t ticksInPhase;  // ticks spent in the current phase (the mode clock while Playing)
  uint32_t
      timeLimitTicks;    // ticks: the mode's clock (deathmatch: match, survival: round), 0 = none
  uint32_t contentHash;  // XXH32 of the content (contentHash()), as in the replay header
  uint8_t modeIndex;     // index into the content's modes
  uint8_t modeKind;      // ObsModeKind
  uint8_t arenaIndex;    // index into the content's arenas
  uint8_t phase;         // ObsMatchPhase
  uint8_t round;         // current round number (1-based once the match started)
  uint8_t roundsToWin;   // rounds, survival: round wins that take the match
  uint8_t scoreToWin;    // deathmatch: eliminations that win
  uint8_t aliveCount;    // players alive in the arena
  uint16_t presentMask;  // bit i: slot i is in the match (the roster)
  uint16_t aliveMask;    // bit i: slot i has a monkey in the arena
  uint8_t inputDelayTicks;  // ticks between this observation and the tick the answer applies on
  uint8_t lastRoundWinner;  // slot, kObsNoSlot after a draw or before the first round end
  uint8_t pad0;
  uint8_t pad1;
  uint16_t roundStartTicks;               // ticks: countdown at a round start (players frozen)
  uint16_t roundEndTicks;                 // ticks: pause after a round end
  uint16_t respawnDelayTicks;             // deathmatch: ticks from an elimination to the respawn
  uint16_t respawnInvulnTicks;            // deathmatch: spawn protection after a respawn
  uint16_t reservedTurnTicks;             // v0.29 grid space: ticks per turn (0 in v1)
  uint16_t reservedTicksToLatch;          // v0.29: ticks to the next latch (0 in v1)
  uint8_t scores[kObsMaxPlayers];         // round wins (rounds, survival) or eliminations
  uint8_t deaths[kObsMaxPlayers];         // eliminations suffered this match
  uint16_t respawnTicks[kObsMaxPlayers];  // ticks until a dead slot respawns, 0 = none due
  uint16_t reservedTeamScores[kObsMaxPlayers];   // v0.29: per team index (0 in v1)
  uint32_t reservedModeData[kObsModeDataWords];  // v0.29: MatchRules::modeData (0 in v1)
};

// Content constants a bot needs to predict the sim (a flight helper needs
// every banana number).
struct ObsRules {
  int32_t playerRadius;       // Q16.16 units: every monkey's contact radius
  int32_t playerMoveSpeed;    // Q16.16 units/tick: base move speed (ObsSelf::moveSpeed is derived)
  int32_t bananaRadius;       // Q16.16 units: base contact radius (ObsBanana::radius is derived)
  int32_t bananaSpeed;        // Q16.16 units/tick: base flight speed
  int32_t bananaSpawnOffset;  // Q16.16 units: spawn distance ahead of the thrower
  int32_t bananaRestitution;  // Q16.16: wall bounce speed scale
  int32_t pickupRadius;       // Q16.16 units: pickup contact radius
  uint16_t bananaOutboundTicks;    // ticks outbound before returning
  uint16_t bananaMaxReturnTicks;   // ticks returning before it drops (also when its owner is gone)
  uint16_t bananaTurnRateBam;      // bam per tick: returning steering rate toward the owner
  uint16_t hitStunTicks;           // ticks of HitStun after a hit
  uint8_t bananaDamage;            // base damage per hit
  uint8_t dashAttackDamage;        // damage of a dash attack
  uint8_t dashAttackMinAmmo;       // bananas in hand that arm a dash attack
  uint8_t dashAttackConsumesAmmo;  // 0/1: a dash attack spends a banana
  uint8_t playerMaxHp;             // base max hp
  uint8_t playerAmmoMax;           // base ammo capacity
  uint8_t playerAmmoStart;         // bananas at a spawn
  uint8_t pad0;
};

// One ability slot: the live state and the numbers of the def it runs (a
// power-up override shows here as a different def).
struct ObsAbility {
  uint8_t defIndex;          // index into the content's abilities (0 = none)
  uint8_t kind;              // ObsAbilityKind
  uint8_t phase;             // ObsAbilityPhase
  uint8_t button;            // the PlayerInput button bit that fires this slot
  uint16_t phaseTicks;       // ticks spent in the current phase
  uint16_t cooldownTicks;    // ticks until it can fire again, 0 = ready (when Idle)
  uint32_t activatedTick;    // tick of the last activation (0 = never)
  uint8_t activatedSubTick;  // 1/256 tick: press stamp of the last activation
  uint8_t pad0;
  uint8_t pad1;
  uint8_t pad2;
  uint16_t defCooldownTicks;  // ticks from activation to the next possible one
  uint16_t defStartupTicks;   // ticks of each phase
  uint16_t defActiveTicks;
  uint16_t defRecoveryTicks;
  uint16_t defPerfectWindowTicks;  // Dash: the catch window from the press
  uint16_t pad3;
  int32_t defSpeed;   // Q16.16 units/tick: Dash speed, Blink distance per active tick
  int32_t defParam0;  // Q16.16: Slam reach
  int32_t defParam1;  // Q16.16: Slam push distance
  int32_t dirX;       // Q16.16 unit vector: direction captured at the last activation
  int32_t dirY;
};

struct ObsModifier {
  uint8_t powerUp;  // index into the content's power-ups
  uint8_t pad0;
  uint16_t ticksLeft;  // ticks the power-up stays live
};

// The bot's own monkey. All zero but slot, respawnTicks and the reserved
// fields while it has no monkey in the arena (alive == 0).
struct ObsSelf {
  uint8_t slot;   // own slot
  uint8_t alive;  // 1: a monkey in the arena
  uint8_t hp;
  uint8_t maxHp;          // derived (power-ups)
  uint8_t ammo;           // bananas in hand
  uint8_t ammoMax;        // derived (power-ups)
  uint8_t lastButtons;    // the buttons the sim applied last tick (a press needs a 0 -> 1 edge)
  uint8_t modifierCount;  // live entries in modifiers
  int32_t posX;           // Q16.16 units
  int32_t posY;
  int32_t prevPosX;  // Q16.16 units: position at the start of the last tick
  int32_t prevPosY;
  int32_t velX;  // Q16.16 units/tick: last tick's velocity
  int32_t velY;
  int32_t moveSpeed;   // Q16.16 units/tick: derived walking speed
  int32_t radius;      // Q16.16 units: contact radius
  int32_t knockbackX;  // Q16.16 units/tick: a slam's push, added for knockbackTicks more ticks
  int32_t knockbackY;
  uint32_t tags;                 // tags
  uint16_t hitStunTicks;         // ticks of HitStun left
  uint16_t knockbackTicks;       // ticks of knockback left
  uint16_t invulnTicks;          // ticks of spawn protection left
  uint16_t respawnTicks;         // ticks until the respawn while dead, 0 = none due
  uint8_t facing;                // angle8: aim direction (the body faces it)
  uint8_t reservedFacingOctant;  // v0.29 grid space: facing as 0..7 (0 in v1)
  uint8_t reservedRole;          // v0.29: loadout index (0 in v1)
  uint8_t reservedTeam;          // v0.29: own team index (0 in v1)
  int16_t reservedCellX;         // v0.29 grid space: own cell (0 in v1)
  int16_t reservedCellY;
  ObsAbility abilities[kObsAbilitySlots];   // slot 0 Dash button, 1 Throw, 2 Ability1
  ObsModifier modifiers[kObsMaxModifiers];  // live power-ups, [0, modifierCount) in pickup order
};

// Every player slot (the own slot included, relation Self).
struct ObsPlayer {
  uint8_t present;          // 1: the slot is in the roster
  uint8_t alive;            // 1: a monkey in the arena (a slot that left keeps it until it dies)
  uint8_t reservedVisible;  // v0.29 fog: 1 when seen (0 in v1: everything is seen)
  uint8_t relation;         // ObsRelation to self; None for an empty slot
  uint8_t reservedTeam;     // v0.29: team index (0 in v1)
  uint8_t hp;
  uint8_t maxHp;
  uint8_t ammo;
  uint8_t ammoMax;
  uint8_t facing;   // angle8
  uint8_t dashing;  // 1: Tag::Dashing (mid-dash: it moves at dash speed and strikes on contact)
  uint8_t pad0;
  uint32_t tags;  // tags
  int32_t posX;   // Q16.16 units
  int32_t posY;
  int32_t velX;  // Q16.16 units/tick
  int32_t velY;
  uint16_t hitStunTicks;  // ticks
  uint16_t invulnTicks;   // ticks of spawn protection
  uint16_t respawnTicks;  // ticks until a dead slot respawns, 0 = none due
  uint16_t pad1;
  uint8_t abilityKind[kObsAbilitySlots];  // ObsAbilityKind per slot
  uint8_t pad2;
  uint8_t abilityPhase[kObsAbilitySlots];  // ObsAbilityPhase per slot
  uint8_t pad3;
  uint16_t abilityCooldownTicks[kObsAbilitySlots];  // ticks per slot, 0 = ready
  uint16_t pad4;
  uint16_t abilityPhaseTicks[kObsAbilitySlots];  // ticks per slot in the current phase
  uint16_t pad5;
};

// One banana pool slot. approachTicks/approachDistance: the integer tick
// k in [0, kObsApproachHorizonTicks] that minimises |pos + k·vel − self.pos|
// with the banana on its current velocity and self standing still (exact
// int64 arithmetic; the smaller k on a tie), and that distance.
struct ObsBanana {
  uint8_t valid;            // 1: a banana in this pool slot
  uint8_t reservedVisible;  // v0.29 fog (0 in v1)
  uint8_t phase;            // ObsBananaPhase
  uint8_t owner;            // slot of the thrower (the parrier after a parry)
  uint8_t ownerRelation;    // ObsRelation of the owner to self
  uint8_t damage;           // hp per hit
  uint8_t pad0;
  uint8_t pad1;
  uint16_t headingBam;     // bam: flight heading
  uint16_t ticksAlive;     // ticks since the throw (saturates)
  uint16_t ticksInPhase;   // ticks in the current phase (saturates)
  uint16_t approachTicks;  // ticks until the closest approach, kObsNoApproach without self
  int32_t posX;            // Q16.16 units
  int32_t posY;
  int32_t velX;  // Q16.16 units/tick
  int32_t velY;
  int32_t speed;             // Q16.16 units/tick: flight speed (base × the thrower's speedMul)
  int32_t radius;            // Q16.16 units: contact radius (base × the thrower's radiusMul)
  int32_t approachDistance;  // Q16.16 units: centre distance at the closest approach
};

// One pickup pool slot.
struct ObsPickup {
  uint8_t valid;    // 1: a pickup in this pool slot
  uint8_t kind;     // ObsPickupKind
  uint8_t powerUp;  // PowerUp: index into the content's power-ups
  uint8_t spawner;  // PowerUp: index of its spawner in ObsArena::spawners
  int32_t posX;     // Q16.16 units
  int32_t posY;
  int32_t radius;  // Q16.16 units: contact radius
};

struct ObsPoint {
  int32_t x;  // Q16.16 units
  int32_t y;
};

// A closed axis-aligned box [min, max].
struct ObsBox {
  int32_t minX;  // Q16.16 units
  int32_t minY;
  int32_t maxX;
  int32_t maxY;
};

struct ObsHazard {
  ObsBox box;
  int32_t speedMul;  // Q16.16: Slow: move speed multiplier inside
  uint8_t kind;      // ObsHazardKind
  uint8_t pad0;
  uint8_t pad1;
  uint8_t pad2;
};

struct ObsSpawner {
  ObsPoint pos;
  uint16_t powerUpSet;     // bit i: power-up i may spawn (0 = any)
  uint16_t firstTicks;     // Playing tick of the first spawn
  uint16_t intervalTicks;  // ticks between spawns, 0 = once
  uint8_t maxAlive;        // its pickups lying at most at once
  uint8_t pad0;
};

// The arena's statics (immutable for the match).
struct ObsArena {
  ObsPoint halfExtents;     // Q16.16 units: the outer box is [-half, +half]
  uint8_t spawnPointCount;  // live entries of each array below
  uint8_t wallCount;
  uint8_t hazardCount;
  uint8_t spawnerCount;
  ObsPoint spawnPoints[kObsMaxSpawnPoints];
  ObsBox walls[kObsMaxWalls];  // solid: monkeys slide, bananas bounce
  ObsHazard hazards[kObsMaxHazards];
  ObsSpawner spawners[kObsMaxSpawners];
};

// v0.29 calls (reserved, all zero in v1): a call the monkey heard last turn.
struct ObsHeardCall {
  uint8_t valid;
  uint8_t slot;    // the caller
  uint8_t symbol;  // the caller's aim byte
  uint8_t pad0;
  uint32_t tick;  // when it was heard
  int16_t cellX;  // the caller's cell when visible
  int16_t cellY;
};

struct Observation {
  ObsHeader header;
  ObsMatch match;
  ObsRules rules;
  ObsSelf self;
  ObsPlayer players[kObsMaxPlayers];  // by slot
  ObsBanana bananas[kObsMaxBananas];  // by pool slot
  ObsPickup pickups[kObsMaxPickups];  // by pool slot
  ObsArena arena;
  ObsHeardCall reservedHeardCalls[kObsMaxHeardCalls];  // v0.29 (0 in v1)
};

// --- layout -------------------------------------------------------------------

static_assert(std::endian::native == std::endian::little, "the ABI is little-endian");
static_assert(std::is_trivially_copyable_v<Observation> && std::is_standard_layout_v<Observation>);
static_assert(alignof(Observation) <= 8);

static_assert(sizeof(ObsHeader) == 16);
static_assert(sizeof(ObsMatch) == 168);
static_assert(sizeof(ObsRules) == 44);
static_assert(sizeof(ObsAbility) == 48);
static_assert(sizeof(ObsModifier) == 4);
static_assert(sizeof(ObsSelf) == 244);
static_assert(sizeof(ObsPlayer) == 64);
static_assert(sizeof(ObsBanana) == 44);
static_assert(sizeof(ObsPickup) == 16);
static_assert(sizeof(ObsHazard) == 24);
static_assert(sizeof(ObsSpawner) == 16);
static_assert(sizeof(ObsArena) == 1116);
static_assert(sizeof(ObsHeardCall) == 12);
static_assert(sizeof(Observation) == 5748);

static_assert(offsetof(Observation, header) == 0);
static_assert(offsetof(Observation, match) == 16);
static_assert(offsetof(Observation, rules) == 184);
static_assert(offsetof(Observation, self) == 228);
static_assert(offsetof(Observation, players) == 472);
static_assert(offsetof(Observation, bananas) == 1112);
static_assert(offsetof(Observation, pickups) == 3928);
static_assert(offsetof(Observation, arena) == 4440);
static_assert(offsetof(Observation, reservedHeardCalls) == 5556);

static_assert(offsetof(ObsMatch, modeIndex) == 16);
static_assert(offsetof(ObsMatch, presentMask) == 24);
static_assert(offsetof(ObsMatch, inputDelayTicks) == 28);
static_assert(offsetof(ObsMatch, roundStartTicks) == 32);
static_assert(offsetof(ObsMatch, scores) == 44);
static_assert(offsetof(ObsMatch, deaths) == 54);
static_assert(offsetof(ObsMatch, respawnTicks) == 64);
static_assert(offsetof(ObsMatch, reservedTeamScores) == 84);
static_assert(offsetof(ObsMatch, reservedModeData) == 104);

static_assert(offsetof(ObsRules, bananaOutboundTicks) == 28);
static_assert(offsetof(ObsRules, bananaDamage) == 36);
static_assert(offsetof(ObsRules, playerMaxHp) == 40);

static_assert(offsetof(ObsAbility, phaseTicks) == 4);
static_assert(offsetof(ObsAbility, activatedTick) == 8);
static_assert(offsetof(ObsAbility, defCooldownTicks) == 16);
static_assert(offsetof(ObsAbility, defPerfectWindowTicks) == 24);
static_assert(offsetof(ObsAbility, defSpeed) == 28);
static_assert(offsetof(ObsAbility, dirX) == 40);

static_assert(offsetof(ObsSelf, posX) == 8);
static_assert(offsetof(ObsSelf, velX) == 24);
static_assert(offsetof(ObsSelf, moveSpeed) == 32);
static_assert(offsetof(ObsSelf, knockbackX) == 40);
static_assert(offsetof(ObsSelf, tags) == 48);
static_assert(offsetof(ObsSelf, hitStunTicks) == 52);
static_assert(offsetof(ObsSelf, invulnTicks) == 56);
static_assert(offsetof(ObsSelf, facing) == 60);
static_assert(offsetof(ObsSelf, reservedCellX) == 64);
static_assert(offsetof(ObsSelf, abilities) == 68);
static_assert(offsetof(ObsSelf, modifiers) == 212);

static_assert(offsetof(ObsPlayer, reservedTeam) == 4);
static_assert(offsetof(ObsPlayer, tags) == 12);
static_assert(offsetof(ObsPlayer, posX) == 16);
static_assert(offsetof(ObsPlayer, hitStunTicks) == 32);
static_assert(offsetof(ObsPlayer, abilityKind) == 40);
static_assert(offsetof(ObsPlayer, abilityPhase) == 44);
static_assert(offsetof(ObsPlayer, abilityCooldownTicks) == 48);
static_assert(offsetof(ObsPlayer, abilityPhaseTicks) == 56);

static_assert(offsetof(ObsBanana, headingBam) == 8);
static_assert(offsetof(ObsBanana, approachTicks) == 14);
static_assert(offsetof(ObsBanana, posX) == 16);
static_assert(offsetof(ObsBanana, speed) == 32);
static_assert(offsetof(ObsBanana, approachDistance) == 40);

static_assert(offsetof(ObsPickup, posX) == 4);
static_assert(offsetof(ObsHazard, speedMul) == 16);
static_assert(offsetof(ObsHazard, kind) == 20);
static_assert(offsetof(ObsSpawner, powerUpSet) == 8);
static_assert(offsetof(ObsSpawner, maxAlive) == 14);

static_assert(offsetof(ObsArena, spawnPointCount) == 8);
static_assert(offsetof(ObsArena, spawnPoints) == 12);
static_assert(offsetof(ObsArena, walls) == 92);
static_assert(offsetof(ObsArena, hazards) == 604);
static_assert(offsetof(ObsArena, spawners) == 988);

static_assert(offsetof(ObsHeardCall, tick) == 4);
static_assert(offsetof(ObsHeardCall, cellX) == 8);

}  // namespace kfm::ai
