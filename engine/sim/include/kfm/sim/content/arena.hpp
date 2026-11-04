// The arena (architecture §5.10): a box centred on the origin, spawn points,
// static interior walls and floor hazards. Immutable content like every other
// def; the sim reads it through MatchContent::arena. Statics are axis-aligned
// boxes (convex polygons are out of scope, §5.10) resolved by a plain loop in
// content order (systems/statics.hpp): at most kMaxArenaWalls walls, so no
// spatial structure is needed (§5.10 records the bench).
//
// Units: arena units; boxes are [min, max] on both axes with min < max.
#pragma once

#include "kfm/core/fixed.hpp"
#include "kfm/core/vec2.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/content/name.hpp"
#include "kfm/sim/content/powerup.hpp"

#include <array>
#include <cstdint>

namespace kfm::sim {

inline constexpr uint8_t kMaxArenaWalls = 32;
inline constexpr uint8_t kMaxArenaHazards = 16;
inline constexpr uint8_t kMaxArenaZones = 8;  // v0.33

// A solid axis-aligned block. Players slide along it (a dash stops at it);
// bananas bounce off it with BoomerangDef::restitution (§5.6, §5.7).
struct Wall {
  Vec2 min{};
  Vec2 max{};
};

enum class HazardKind : uint8_t {
  Pit = 0,  // a player whose centre is inside is eliminated (killer 0xFF); bananas fly over
  Slow,     // move speed x speedMul while the player's centre is inside (ModifierSystem)
  kCount,   // NOLINT(readability-identifier-naming): table-size sentinel, not a kind
};

// A floor region acting on the players whose centre lies inside it (closed
// box). Hazards never block movement or bananas.
struct Hazard {
  Vec2 min{};
  Vec2 max{};
  HazardKind kind{HazardKind::Pit};
  Fixed speedMul{Fixed::one()};  // Slow: move speed multiplier; Pit: unused
};

// An objective region (v0.33, architecture §5.3, §5.10): a closed box that
// blocks nothing and emits no contact. Modes read who stands in it through
// zoneOccupants() (kfm/sim/zones.hpp): hill holds zone 0, capture gives team
// t zone t.
struct Zone {
  Vec2 min{};
  Vec2 max{};

  [[nodiscard]] constexpr Vec2 centre() const {
    return Vec2{
        Fixed::fromRaw(static_cast<int32_t>((static_cast<int64_t>(min.x.raw) + max.x.raw) / 2)),
        Fixed::fromRaw(static_cast<int32_t>((static_cast<int64_t>(min.y.raw) + max.y.raw) / 2))};
  }
};

struct ArenaDef {
  ContentName name{};
  Vec2 halfExtents{};  // the outer box, centred at the origin
  std::array<Vec2, kMaxPlayers> spawnPoints{};
  uint8_t spawnPointCount{0};
  // Interior statics, in content order (the order collision resolves them).
  std::array<Wall, kMaxArenaWalls> walls{};
  uint8_t wallCount{0};
  std::array<Hazard, kMaxArenaHazards> hazards{};
  uint8_t hazardCount{0};
  // Power-up pickup spawners (§5.8); none in the default arena.
  std::array<PickupSpawner, kMaxPickupSpawners> pickupSpawners{};
  uint8_t pickupSpawnerCount{0};
  // v0.32: the spawn points split into this many consecutive groups (at
  // least one point each); team t spawns and respawns in group t mod
  // spawnGroups (spawnGroupRange). 1 = one shared list, the M1 behaviour.
  uint8_t spawnGroups{1};
  // v0.33: objective zones, in content order (TOML `zones`, or the a-h
  // cells of a tilemap), and the cell size of a tilemap arena (0 for a
  // hand-placed one; a grid mode needs one, §5.12).
  std::array<Zone, kMaxArenaZones> zones{};
  uint8_t zoneCount{0};
  Fixed cellSize{};
};

// Spawn points [begin, end) of group g (taken mod the group count) of an
// arena: group g holds points [g·n/G, (g+1)·n/G) of the n points.
struct SpawnRange {
  uint8_t begin{0};
  uint8_t end{0};
};
[[nodiscard]] constexpr SpawnRange spawnGroupRange(const ArenaDef& arena, uint8_t group) {
  const uint32_t groups = arena.spawnGroups > 0 ? arena.spawnGroups : 1u;
  const uint32_t n = arena.spawnPointCount;
  const uint32_t g = group % groups;
  return SpawnRange{static_cast<uint8_t>((g * n) / groups),
                    static_cast<uint8_t>(((g + 1) * n) / groups)};
}

// Whether p lies in the closed box [min, max] (hazards test the centre).
[[nodiscard]] constexpr bool boxContains(Vec2 min, Vec2 max, Vec2 p) {
  return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
}

// Squared distance from p to the closed box, in raw Q16.16 units squared
// (exact for coordinates within ±1024).
[[nodiscard]] constexpr int64_t boxDistanceSqRaw(Vec2 min, Vec2 max, Vec2 p) {
  const int64_t dx = static_cast<int64_t>(p.x.raw) - clamp(p.x, min.x, max.x).raw;
  const int64_t dy = static_cast<int64_t>(p.y.raw) - clamp(p.y, min.y, max.y).raw;
  return (dx * dx) + (dy * dy);
}

// Whether a circle overlaps the box; touching is not overlapping.
[[nodiscard]] constexpr bool circleOverlapsBox(Vec2 min, Vec2 max, Vec2 c, Fixed r) {
  return boxDistanceSqRaw(min, max, c) < static_cast<int64_t>(r.raw) * r.raw;
}

}  // namespace kfm::sim
