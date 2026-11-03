// Immutable content definitions (architecture §5.5, §5.7, §5.10). In M1 the
// defs are constexpr tables assembled into one ContentDB and hashed through
// the visitor; M4 swaps the source for the TOML/JSON loader. Systems read
// every tuning number from here, never from literals of their own.
//
// Units: lengths in arena units, speeds in units per tick, durations in ticks.
#pragma once

#include "kfm/core/fixed.hpp"
#include "kfm/core/vec2.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/tags.hpp"

#include <array>
#include <cstdint>

namespace kfm::sim {

inline constexpr uint8_t kAbilitySlots = 2;
inline constexpr uint8_t kAbilitySlotDash = 0;
inline constexpr uint8_t kAbilitySlotThrow = 1;
inline constexpr uint8_t kMaxAbilityDefs = 4;

enum class AbilityKind : uint8_t {
  None = 0,
  Dash,
  Throw,
  kCount,  // NOLINT(readability-identifier-naming): table-size sentinel, not a kind
};
enum class AbilityPhase : uint8_t { Idle = 0, Startup, Active, Recovery };

struct AbilityDef {
  AbilityKind kind{AbilityKind::None};
  uint16_t cooldownTicks{0};  // counted from activation
  uint16_t startupTicks{0};
  uint16_t activeTicks{0};
  uint16_t recoveryTicks{0};
  uint16_t perfectWindowTicks{0};  // Dash: first N active ticks grant Tag::PerfectDash
  Fixed speed{};                   // Dash: dash speed (units/tick). Throw: unused
  Fixed param0{};                  // kind-specific spare numeric params
  Fixed param1{};
  Tags tagsStartup{};  // tags granted during each phase
  Tags tagsActive{};
  Tags tagsRecovery{};
};

struct BoomerangDef {
  Fixed radius{};
  Fixed speed{};               // units/tick, constant magnitude in flight
  uint16_t outboundTicks{0};   // then Returning
  uint16_t maxReturnTicks{0};  // Returning longer than this -> Grounded (no infinite orbit)
  uint16_t turnRateBam{0};     // max heading change per tick while returning (BAM units)
  Fixed restitution{};         // wall bounce velocity scale (1 = elastic)
  Fixed spawnOffset{};         // distance ahead of the thrower where it spawns
  uint8_t damage{1};
  uint8_t pierce{0};  // 0/1: keeps flying after a hit
};

// Pickups lying in the arena (M1: grounded bananas; M4 adds power-ups).
struct PickupDef {
  Fixed radius{};  // contact radius against a player
};

struct PlayerDef {
  Fixed radius{};
  Fixed moveSpeed{};  // units/tick
  uint8_t maxHp{1};
  uint8_t ammoMax{1};
  uint8_t ammoStart{1};
  uint16_t hitStunTicks{0};
  // Base loadout: slot kAbilitySlotDash, slot kAbilitySlotThrow (indices into
  // ContentDB::abilities).
  std::array<uint8_t, kAbilitySlots> abilities{};
};

struct ArenaDef {
  Vec2 halfExtents{};  // box arena, centred at the origin
  std::array<Vec2, kMaxPlayers> spawnPoints{};
  uint8_t spawnPointCount{0};
};

struct ModeDef {  // FFA rules
  uint8_t roundsToWin{3};
  uint16_t roundEndTicks{120};   // pause after a round ends before the next starts
  uint16_t roundStartTicks{60};  // countdown at round start (players spawned, inputs ignored)
};

struct ContentDB {
  std::array<AbilityDef, kMaxAbilityDefs> abilities{};  // index 0 = None, then Dash, Throw
  uint8_t abilityCount{0};
  BoomerangDef boomerang{};
  PickupDef pickup{};
  PlayerDef player{};
  ArenaDef arena{};
  ModeDef mode{};

  // Def for an ability slot of the base loadout.
  [[nodiscard]] constexpr const AbilityDef& abilityForSlot(uint8_t slot) const {
    return abilities[player.abilities[slot]];
  }
};

// The M1 constexpr tables (default tuning numbers from the M1 contract).
[[nodiscard]] const ContentDB& defaultContent();

// XXH32 of the canonical serialized form (visit(V&, ContentDB&)). Peers and
// replays must match it.
[[nodiscard]] uint32_t contentHash(const ContentDB& content);

}  // namespace kfm::sim
