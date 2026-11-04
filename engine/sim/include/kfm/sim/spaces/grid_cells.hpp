// The grid space's cells (architecture §5.12, v0.34): the one mapping
// between arena positions and cells, shared by the grid's SpaceBehavior
// (src/spaces/grid.cpp) and the observation builder (engine/ai, step 5).
//
// A cell is a square of ArenaDef::cellSize. Column 0 is the arena's west
// edge (-x) and row 0 its north edge (+y), so (col, row) reads like the
// tilemap that drew the arena (content/arenas/*.toml: the top map row is
// row 0). A cell holds the points of [left, right) × (bottom, top]: a point
// on a vertical cell edge belongs to the cell east of it, a point on a
// horizontal edge to the cell south of it. The centre of a cell is exactly
// where the tilemap loader puts a spawn point on it, so a monkey spawned on
// a tilemap rests on a cell centre. An arena without cells (cellSize 0, a
// hand-placed arena) maps every point to cell (0, 0) and has no columns.
//
// Directions: octant 0 is +x (east), counter-clockwise in 45-degree steps,
// so 2 is +y (north, row - 1), 4 west, 6 south. A moveDir (0..255, 0 = +x,
// §5.4) goes to the nearest octant, a half-way direction to the
// counter-clockwise one: octant = ((moveDir + 16) / 32) mod 8.
#pragma once

#include "kfm/core/fixed.hpp"
#include "kfm/core/vec2.hpp"
#include "kfm/sim/content/arena.hpp"

#include <array>
#include <cstdint>

namespace kfm::sim::grid {

struct Cell {
  int32_t col{0};
  int32_t row{0};
  constexpr bool operator==(const Cell&) const = default;
};

inline constexpr uint8_t kOctants = 8;
// Column and row offsets of the eight neighbours, indexed by octant.
inline constexpr std::array<int8_t, kOctants> kOctantCol = {1, 1, 0, -1, -1, -1, 0, 1};
inline constexpr std::array<int8_t, kOctants> kOctantRow = {0, -1, -1, -1, 0, 1, 1, 1};

// Division rounding toward negative infinity (b > 0).
[[nodiscard]] constexpr int64_t floorDiv(int64_t a, int64_t b) {
  const int64_t q = a / b;
  return (a % b != 0 && a < 0) ? q - 1 : q;
}

[[nodiscard]] constexpr bool hasCells(const ArenaDef& a) {
  return a.cellSize.raw > 0;
}
// Cells across (columns) and down (rows) the arena box; 0 without cells.
[[nodiscard]] constexpr int32_t columns(const ArenaDef& a) {
  return hasCells(a) ? static_cast<int32_t>((int64_t{a.halfExtents.x.raw} * 2) / a.cellSize.raw)
                     : 0;
}
[[nodiscard]] constexpr int32_t rows(const ArenaDef& a) {
  return hasCells(a) ? static_cast<int32_t>((int64_t{a.halfExtents.y.raw} * 2) / a.cellSize.raw)
                     : 0;
}

// The cell holding a point (it may lie outside the arena's columns and rows).
[[nodiscard]] constexpr Cell cellOf(const ArenaDef& a, Vec2 p) {
  if (!hasCells(a)) return Cell{};
  const int64_t c = a.cellSize.raw;
  return Cell{static_cast<int32_t>(floorDiv(int64_t{p.x.raw} + a.halfExtents.x.raw, c)),
              static_cast<int32_t>(floorDiv(int64_t{a.halfExtents.y.raw} - p.y.raw, c))};
}

// The centre of a cell (the origin without cells).
[[nodiscard]] constexpr Vec2 cellCentre(const ArenaDef& a, Cell k) {
  if (!hasCells(a)) return Vec2{};
  const int64_t c = a.cellSize.raw;
  return Vec2{Fixed::fromRaw(static_cast<int32_t>((k.col * c) - a.halfExtents.x.raw + (c / 2))),
              Fixed::fromRaw(static_cast<int32_t>(a.halfExtents.y.raw - (k.row * c) - (c / 2)))};
}

// Whether a cell is one of the arena's columns and rows.
[[nodiscard]] constexpr bool inside(const ArenaDef& a, Cell k) {
  return k.col >= 0 && k.row >= 0 && k.col < columns(a) && k.row < rows(a);
}

// The octant nearest a moveDir (§5.4's 256 directions).
[[nodiscard]] constexpr uint8_t octantOf(uint8_t moveDir) {
  return static_cast<uint8_t>(((uint32_t{moveDir} + 16u) >> 5) & 7u);
}
// The octant nearest an angle. Eight BAM of slack round a direction that
// angleOf() recovered a couple of BAM short of a half-way point the way
// octantOf() rounds the moveDir it came from.
[[nodiscard]] constexpr uint8_t octantOfAngle(Angle a) {
  return static_cast<uint8_t>(((uint32_t{a.bam} + 4096u + 8u) >> 13) & 7u);
}
[[nodiscard]] constexpr bool diagonal(uint8_t octant) {
  return (octant & 1u) != 0;
}

// The neighbour of a cell in an octant.
[[nodiscard]] constexpr Cell neighbour(Cell k, uint8_t octant) {
  return Cell{k.col + kOctantCol[octant & 7u], k.row + kOctantRow[octant & 7u]};
}

}  // namespace kfm::sim::grid
