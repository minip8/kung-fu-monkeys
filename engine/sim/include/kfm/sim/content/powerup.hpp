// Power-up content (architecture §5.8, §5.10): PowerUpDef and its grants, and
// the arena's pickup spawners. Loaded from content/powerups.toml and the
// [[arena.spawner]] tables; ModifierSystem resolves the grants of every live
// ModifierList entry each tick.
#pragma once

#include "kfm/core/fixed.hpp"
#include "kfm/core/vec2.hpp"
#include "kfm/sim/content/name.hpp"
#include "kfm/sim/tags.hpp"

#include <array>
#include <cstdint>

namespace kfm::sim {

inline constexpr uint8_t kMaxPowerUps = 16;
inline constexpr uint8_t kMaxPowerUpGrants = 4;
inline constexpr uint8_t kMaxPickupSpawners = 8;
// ActiveModifier::ticksLeft of a mode's baseline power-up (v0.32): never
// counted down, never dropped. Power-up durations stay below it.
inline constexpr uint16_t kPermanentModifierTicks = 0xFFFF;

// Derived player stats a StatModifier can change (Stats in world_state.hpp).
// v0.34: Radius, the contact radius (base PlayerDef::radius).
enum class StatKind : uint8_t { MoveSpeed = 0, MaxHp, AmmoMax, Radius };

// stat = base × Π mul + Σ add over the live modifiers in slot order. Integer
// stats (MaxHp, AmmoMax) accept `add` only: the loader rejects a mul on them,
// and ModifierSystem ignores it. Fixed stats (MoveSpeed, Radius) take both.
struct StatModifier {
  StatKind stat{StatKind::MoveSpeed};
  Fixed mul{Fixed::one()};
  Fixed add{};
};

// Applied to every banana the holder throws while the power-up is live, and
// snapshotted onto the Boomerang at the throw (it outlives the power-up).
// Several combine in slot order: muls multiply, damage adds.
struct ProjectileModifier {
  Fixed speedMul{Fixed::one()};
  Fixed radiusMul{Fixed::one()};
  uint8_t damageAdd{0};
};

enum class GrantKind : uint8_t {
  None = 0,
  Stat,             // `stat`
  AbilityOverride,  // `slot` runs `ability` instead of the loadout's def
  Projectile,       // `projectile`
  Tag,              // `tag` is set while live; Shielded is consumed by the hit it absorbs
};

// One grant; only the fields of its kind are meaningful (the rest stay at
// their defaults, so the canonical form hashes the same for equal grants).
struct PowerUpGrant {
  GrantKind kind{GrantKind::None};
  StatModifier stat{};
  uint8_t slot{0};     // AbilityOverride: the ability slot
  uint8_t ability{0};  // AbilityOverride: index into ContentDB::abilities
  ProjectileModifier projectile{};
  Tag tag{Tag::Shielded};
};

// How presentation draws a power-up pickup: a palette colour (the player
// palette, index 0..9) and a placeholder shape.
enum class PowerUpShape : uint8_t { Orb = 0, Cube, Diamond, Coin };

struct PowerUpVisual {
  uint8_t color{0};
  PowerUpShape shape{PowerUpShape::Orb};
};

struct PowerUpDef {
  ContentName name{};
  uint16_t durationTicks{0};  // >= 1: live on exactly this many ticks after the pickup
  PowerUpVisual visual{};
  std::array<PowerUpGrant, kMaxPowerUpGrants> grants{};
  uint8_t grantCount{0};
};

// A point in the arena that spawns power-up pickups while the match phase is
// Playing: on Playing tick firstTicks, then every intervalTicks (0 = once),
// as long as fewer than maxAlive of its pickups are lying there. The def is
// drawn uniformly with the world RNG from powerUpSet (bit i = power-up i;
// 0 = every power-up).
//
// v0.81, a tree (tree = 1; the arena spawner key `tree = true`, exclusive
// with `power_ups`, so powerUpSet stays 0): on the same beat it lays one
// banana pickup at pos instead, while fewer than maxAlive bananas (the
// counts of the banana pickups, whoever dropped them) lie on pos's cell
// (grid::cellOf). No RNG draw; a tree bears in every mode and whatever the
// match's power-up set (MatchConfig::powerUps); a full pickup pool stacks
// the fruit on the nearest banana pickup (layBananaPickup, rules/
// bananas.hpp). A tree needs an arena with cells (the loader's check).
struct PickupSpawner {
  Vec2 pos{};
  uint16_t powerUpSet{0};
  uint16_t firstTicks{0};
  uint16_t intervalTicks{0};
  uint8_t maxAlive{1};
  uint8_t tree{0};  // v0.81: 0 = a power-up spawner, 1 = a tree

  [[nodiscard]] constexpr bool isTree() const { return tree != 0; }
};
static_assert(kMaxPowerUps <= 16, "PickupSpawner::powerUpSet is a uint16_t bit set");

// A per-match power-up set (MatchConfig::powerUps, v0.24): bit i enables
// power-up i. Spawners draw from their own set intersected with it; every
// bit set (the default) leaves them as the arena defines them, 0 turns every
// power-up spawner off.
inline constexpr uint16_t kAllPowerUps = 0xFFFF;

}  // namespace kfm::sim
