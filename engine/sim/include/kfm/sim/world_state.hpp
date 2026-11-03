// The complete simulation state (architecture §5.2). Trivially copyable:
// snapshot save/restore for rollback is a memcpy. Everything the sim needs
// to advance one tick is here or in the immutable ContentDB. Every field is
// visited in visit.hpp; derived per-tick scratch (broadphase, contacts) lives
// in system-local arrays, never here.
#pragma once

#include "kfm/core/pool.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/core/types.hpp"
#include "kfm/core/vec2.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/tags.hpp"

#include <array>
#include <cstdint>
#include <type_traits>

namespace kfm::sim {

inline constexpr uint8_t kMaxModifiers = 8;

// --- components ---------------------------------------------------------------

struct Transform {
  Vec2 pos{};
  Vec2 prevPos{};  // pos at tick start, set by the integrating system (swept collision)
  Vec2 vel{};      // units per tick
  Angle facing{};
};

// Derived stats, recomputed each tick by ModifierSystem (§5.8).
struct Stats {
  Fixed moveSpeed{};
  uint8_t maxHp{0};
  uint8_t ammoMax{0};
};

enum class StatKind : uint8_t { MoveSpeed = 0, MaxHp, AmmoMax };

// M1: the type exists, lists stay empty. Integer stats accept `add` only.
struct StatModifier {
  StatKind stat{StatKind::MoveSpeed};
  Fixed mul{Fixed::one()};
  Fixed add{};
  uint16_t ticksLeft{0};
};

// Fixed capacity; items[0..count) are live, in slot order.
struct ModifierList {
  std::array<StatModifier, kMaxModifiers> items{};
  uint8_t count{0};
};

// Decoded by InputSystem each tick, consumed by Ability and Movement.
struct Intent {
  Vec2 move{};  // unit direction × magnitude in [0, 1]; zero when idle
  Angle aim{};
  uint8_t pressed{0};  // Button bits that went 0 -> 1 this tick
  uint8_t held{0};     // Button bits currently down
  uint8_t subTick{0};  // of this tick's input (§5.11)
};

// Per ability slot (§5.5). dir is the dash/throw direction captured at activation.
struct AbilityState {
  uint8_t defIndex{0};  // into ContentDB::abilities
  AbilityPhase phase{AbilityPhase::Idle};
  uint16_t phaseTicks{0};     // ticks spent in the current phase
  uint16_t cooldownTicks{0};  // remaining; 0 = ready
  Tick activatedTick{};
  uint8_t activatedSubTick{0};
  Vec2 dir{};
};

// --- entities -----------------------------------------------------------------

struct Player {
  PlayerId id{PlayerId::invalid()};
  Transform transform{};
  Intent intent{};
  uint8_t prevButtons{0};  // buttons of the previous tick, for edge detection
  Stats stats{};
  ModifierList modifiers{};
  uint8_t hp{0};
  uint8_t ammo{0};
  Tags tags{};
  uint16_t hitStunTicks{0};  // remaining; HitStun clears at 0
  std::array<AbilityState, kAbilitySlots> abilities{};
};

enum class BoomerangPhase : uint8_t { Outbound = 0, Returning, Grounded };

struct Boomerang {
  PlayerId owner{PlayerId::invalid()};
  BoomerangPhase phase{BoomerangPhase::Outbound};
  Transform transform{};  // facing = heading
  Angle spin{};           // cosmetic, still visited
  uint16_t ticksAlive{0};
  uint16_t ticksInPhase{0};
  uint8_t damage{1};
  uint8_t pierce{0};
};

enum class PickupKind : uint8_t { Banana = 0 };  // M1: a grounded banana; M4 adds power-ups

struct Pickup {
  PickupKind kind{PickupKind::Banana};
  Vec2 pos{};
  uint8_t powerUpIndex{0};
};

// --- match --------------------------------------------------------------------

enum class MatchPhase : uint8_t {
  Lobby = 0,
  RoundStarting,
  Playing,
  RoundEnded,
  MatchEnded,
};

// The match flow shared by every mode (architecture §5.3) plus the per-player
// mode state. ticksInPhase is also the match clock: a mode's time limit runs
// over the Playing phase (deathmatch: the whole match; survival: one round).
struct MatchRules {
  MatchPhase phase{MatchPhase::Lobby};
  PlayerMask present{0};
  uint32_t ticksInPhase{0};
  uint8_t round{0};
  // Round wins (rounds, survival) or eliminations scored (deathmatch).
  std::array<uint8_t, kMaxPlayers> scores{};
  PlayerId lastRoundWinner{PlayerId::invalid()};
  // v0.23 per-player mode counters: only deathmatch and survival write them,
  // so they stay zero in the rounds mode.
  std::array<uint8_t, kMaxPlayers> deaths{};         // eliminations suffered this match
  std::array<uint16_t, kMaxPlayers> respawnTicks{};  // dead: ticks until respawn; 0 = none due
  std::array<uint16_t, kMaxPlayers> invulnTicks{};   // alive: spawn protection left (Invulnerable)

  // Any of the mode counters is non-zero.
  [[nodiscard]] constexpr bool hasModeCounters() const {
    for (uint8_t i = 0; i < kMaxPlayers; ++i) {
      if (deaths[i] != 0 || respawnTicks[i] != 0 || invulnTicks[i] != 0) return true;
    }
    return false;
  }
};

struct WorldState {
  Tick tick{};
  Rng rng{};
  MatchRules rules{};
  Pool<Player, kMaxPlayers> players{};
  Pool<Boomerang, kMaxBoomerangs> boomerangs{};
  Pool<Pickup, kMaxPickups> pickups{};
  // Hazards arrive with arena content beyond M1.

  // Players are indexed by slot: the Pool index for a present player equals
  // its PlayerId, which keeps the mapping trivial and deterministic.
  [[nodiscard]] const Player* playerBySlot(PlayerId id) const {
    if (!id.isValid() || id.value >= kMaxPlayers) return nullptr;
    const auto index = static_cast<uint16_t>(id.value);
    return players.isAliveIndex(index) ? &players.items[index] : nullptr;
  }
  [[nodiscard]] Player* playerBySlot(PlayerId id) {
    if (!id.isValid() || id.value >= kMaxPlayers) return nullptr;
    const auto index = static_cast<uint16_t>(id.value);
    return players.isAliveIndex(index) ? &players.items[index] : nullptr;
  }
};
static_assert(std::is_trivially_copyable_v<WorldState>);

// Match configuration that seeds a fresh world. arena and mode are indices
// into ContentDB::arenas / ::modes (0 = the default); they are not part of
// WorldState but constant for the match like the content itself, so every
// host carries the config next to the world (replay header, join path).
struct MatchConfig {
  uint64_t seed{0};
  PlayerMask players{0};
  uint8_t arena{0};
  uint8_t mode{0};
  constexpr bool operator==(const MatchConfig&) const = default;
};

// The content a match with this config simulates with.
[[nodiscard]] constexpr MatchContent matchContent(const ContentDB& content,
                                                  const MatchConfig& config) {
  return MatchContent{content, config.arena, config.mode};
}

// Builds the initial state for a match: tick 0, seeded RNG, lobby phase.
[[nodiscard]] WorldState initialWorld(const MatchConfig& config);

}  // namespace kfm::sim
