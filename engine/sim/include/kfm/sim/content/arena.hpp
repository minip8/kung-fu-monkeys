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
  Pit = 0,  // a player wholly over the pits (their union, discOverPits), not dashing, falls
            // (v0.57), eliminated (killer 0xFF) PlayerDef::fallTicks later; bananas fly over
  Slow,     // move speed x speedMul while the player's centre is inside (ModifierSystem)
  kCount,   // NOLINT(readability-identifier-naming): table-size sentinel, not a kind
};

// A floor region acting on the players whose centre lies inside it (closed
// box; v0.57: a pit only on players wholly over the union of the pits,
// discOverPits). Hazards never block movement or bananas.
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

// v0.82: homeZoneOf() for a team whose home zone the arena does not have.
inline constexpr uint8_t kNoZone = 0xFF;

// v0.82: the home zone of team `team` (harvest, as capture: team t's home is
// zone t), or kNoZone when the arena has no zone of that index.
[[nodiscard]] constexpr uint8_t homeZoneOf(const ArenaDef& arena, uint8_t team) {
  return team < arena.zoneCount && team < kMaxArenaZones ? team : kNoZone;
}

// v0.82: whether the arena has a tree (a spawner that lays bananas,
// PickupSpawner::tree) among its spawners.
[[nodiscard]] constexpr bool hasTree(const ArenaDef& arena) {
  for (uint8_t i = 0; i < arena.pickupSpawnerCount && i < kMaxPickupSpawners; ++i) {
    if (arena.pickupSpawners[i].isTree()) return true;
  }
  return false;
}

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

// v0.57: whether the closed disc (c, r) lies wholly inside the union of the
// closed boxes boxAt selects among 0..count (at most kMaxArenaHazards;
// boxAt(i, min, max) fills box i and returns false to leave it out). The
// union matters: a tilemap pit is several boxes, and a disc straddling two
// adjoining ones is inside it. Exact in fixed point, with no sampling: the
// disc's bounding square is cut at every box edge that crosses it into
// sub-rectangles that no edge enters, so each one lies wholly inside or
// wholly outside every box, and its (doubled, so exact) midpoint says which.
// The disc is inside the union iff every sub-rectangle it overlaps (touching
// is not overlapping, circleOverlapsBox) is inside some box. A radius of 0
// is the centre point. Pure integer arithmetic over the arguments, so the
// sim, the bots and the tests all get the same answer on every host.
template <class BoxAt>
[[nodiscard]] constexpr bool discWithinBoxes(uint32_t count, BoxAt&& boxAt, Vec2 c, Fixed r) {
  constexpr uint32_t kMaxBoxes = kMaxArenaHazards;
  std::array<Vec2, kMaxBoxes> mins{};
  std::array<Vec2, kMaxBoxes> maxs{};
  uint32_t n = 0;
  for (uint32_t i = 0; i < count && i < kMaxBoxes; ++i) {
    if (boxAt(i, mins[n], maxs[n])) ++n;
  }
  const auto inside2 = [&](int64_t x2, int64_t y2) {  // a doubled point in some box
    for (uint32_t k = 0; k < n; ++k) {
      if (2 * int64_t{mins[k].x.raw} <= x2 && x2 <= 2 * int64_t{maxs[k].x.raw} &&
          2 * int64_t{mins[k].y.raw} <= y2 && y2 <= 2 * int64_t{maxs[k].y.raw}) {
        return true;
      }
    }
    return false;
  };
  if (r.raw <= 0) return inside2(2 * int64_t{c.x.raw}, 2 * int64_t{c.y.raw});

  // The breakpoints along each axis: the disc's extent and every box edge
  // strictly inside it, sorted (insertion sort: at most 2 + 2 * 16 values).
  constexpr uint32_t kMaxBreaks = 2 + (2 * kMaxBoxes);
  std::array<int32_t, kMaxBreaks> xs{};
  std::array<int32_t, kMaxBreaks> ys{};
  uint32_t nx = 0;
  uint32_t ny = 0;
  const int32_t x0 = c.x.raw - r.raw;
  const int32_t x1 = c.x.raw + r.raw;
  const int32_t y0 = c.y.raw - r.raw;
  const int32_t y1 = c.y.raw + r.raw;
  xs[nx++] = x0;
  xs[nx++] = x1;
  ys[ny++] = y0;
  ys[ny++] = y1;
  for (uint32_t k = 0; k < n; ++k) {
    for (const int32_t v : {mins[k].x.raw, maxs[k].x.raw}) {
      if (v > x0 && v < x1) xs[nx++] = v;
    }
    for (const int32_t v : {mins[k].y.raw, maxs[k].y.raw}) {
      if (v > y0 && v < y1) ys[ny++] = v;
    }
  }
  const auto sortPrefix = [](std::array<int32_t, kMaxBreaks>& a, uint32_t len) {
    for (uint32_t i = 1; i < len; ++i) {
      const int32_t v = a[i];
      uint32_t j = i;
      for (; j > 0 && a[j - 1] > v; --j)
        a[j] = a[j - 1];
      a[j] = v;
    }
  };
  sortPrefix(xs, nx);
  sortPrefix(ys, ny);
  for (uint32_t i = 0; i + 1 < nx; ++i) {
    if (xs[i] == xs[i + 1]) continue;
    for (uint32_t j = 0; j + 1 < ny; ++j) {
      if (ys[j] == ys[j + 1]) continue;
      const Vec2 lo{Fixed::fromRaw(xs[i]), Fixed::fromRaw(ys[j])};
      const Vec2 hi{Fixed::fromRaw(xs[i + 1]), Fixed::fromRaw(ys[j + 1])};
      if (!circleOverlapsBox(lo, hi, c, r)) continue;
      if (!inside2(int64_t{xs[i]} + xs[i + 1], int64_t{ys[j]} + ys[j + 1])) return false;
    }
  }
  return true;
}

// v0.57: whether a monkey of contact radius r at c is wholly over the
// arena's pits (their union), the test that starts a fall (§5.6 row 60).
[[nodiscard]] constexpr bool discOverPits(const ArenaDef& arena, Vec2 c, Fixed r) {
  return discWithinBoxes(
      arena.hazardCount,
      [&arena](uint32_t i, Vec2& min, Vec2& max) {
        const Hazard& h = arena.hazards[i];
        if (h.kind != HazardKind::Pit) return false;
        min = h.min;
        max = h.max;
        return true;
      },
      c, r);
}

}  // namespace kfm::sim
