// CollisionSystem internals (architecture §5.3, §5.11), header-only so unit
// tests can check the grid layout and the swept narrowphase directly. Not part
// of the public sim interface.
//
// Precondition for every function here: coordinates stay within ±1024 units
// (|raw| < 2^26) so every int64 product below is exact (squares of relative
// positions and velocities stay below 2^57). The arena is ±16 × ±9.
#pragma once

#include "kfm/core/fixed.hpp"
#include "kfm/core/vec2.hpp"

#include <algorithm>
#include <compare>
#include <cstdint>
#include <utility>

namespace kfm::sim::collision {

// --- swept AABB ---------------------------------------------------------------

struct Aabb {
  Fixed minX{};
  Fixed minY{};
  Fixed maxX{};
  Fixed maxY{};
};

// Box covering a circle of radius r moving from `from` to `to`.
[[nodiscard]] constexpr Aabb sweptAabb(Vec2 from, Vec2 to, Fixed r) {
  return Aabb{min(from.x, to.x) - r, min(from.y, to.y) - r, max(from.x, to.x) + r,
              max(from.y, to.y) + r};
}

// Closed-interval overlap (touching boxes overlap).
[[nodiscard]] constexpr bool overlaps(const Aabb& a, const Aabb& b) {
  return a.minX <= b.maxX && b.minX <= a.maxX && a.minY <= b.maxY && b.minY <= a.maxY;
}

// --- uniform grid -------------------------------------------------------------

inline constexpr int32_t kBaseCellRaw = 2 * Fixed::kOneRaw;  // 2 units
inline constexpr uint16_t kMaxGridAxisCells = 64;
inline constexpr uint32_t kMaxGridCells = uint32_t{kMaxGridAxisCells} * kMaxGridAxisCells;

// Square cells over [-half, +half]. The cell size starts at 2 units and grows
// when the arena would need more than kMaxGridAxisCells along an axis.
struct GridLayout {
  int64_t originXRaw{0};
  int64_t originYRaw{0};
  int64_t cellRaw{kBaseCellRaw};
  uint16_t nx{1};
  uint16_t ny{1};

  // Cell coordinate of a position along one axis, clamped into the grid. The
  // mapping is monotonic, so two overlapping boxes always share a cell, even
  // when they lie partly or wholly outside the arena.
  [[nodiscard]] constexpr uint16_t cellX(Fixed x) const { return axisCell(x.raw, originXRaw, nx); }
  [[nodiscard]] constexpr uint16_t cellY(Fixed y) const { return axisCell(y.raw, originYRaw, ny); }

 private:
  [[nodiscard]] constexpr uint16_t axisCell(int32_t v, int64_t origin, uint16_t n) const {
    const int64_t rel = static_cast<int64_t>(v) - origin;
    if (rel <= 0) return 0;
    const int64_t c = rel / cellRaw;
    return std::cmp_greater_equal(c, n) ? static_cast<uint16_t>(n - 1) : static_cast<uint16_t>(c);
  }
};

[[nodiscard]] constexpr GridLayout gridLayout(Vec2 halfExtents) {
  const int64_t extX = std::max<int64_t>(int64_t{2} * halfExtents.x.raw, 0);
  const int64_t extY = std::max<int64_t>(int64_t{2} * halfExtents.y.raw, 0);
  const auto cellsFor = [](int64_t extent, int64_t cell) { return (extent + cell - 1) / cell; };

  GridLayout g;
  g.originXRaw = -static_cast<int64_t>(halfExtents.x.raw);
  g.originYRaw = -static_cast<int64_t>(halfExtents.y.raw);
  g.cellRaw = kBaseCellRaw;
  const int64_t ext = std::max(extX, extY);
  if (cellsFor(ext, g.cellRaw) > kMaxGridAxisCells) g.cellRaw = cellsFor(ext, kMaxGridAxisCells);
  g.nx = static_cast<uint16_t>(std::max<int64_t>(cellsFor(extX, g.cellRaw), 1));
  g.ny = static_cast<uint16_t>(std::max<int64_t>(cellsFor(extY, g.cellRaw), 1));
  return g;
}

// --- 128-bit helpers ----------------------------------------------------------
// Portable (no __int128: MSVC has none) unsigned 128-bit arithmetic, just
// enough for the exact discriminant below.

struct U128 {
  uint64_t hi{0};
  uint64_t lo{0};

  constexpr auto operator<=>(const U128&) const = default;
};

[[nodiscard]] constexpr U128 mulU64(uint64_t a, uint64_t b) {
  constexpr uint64_t kMask = 0xFFFF'FFFFu;
  const uint64_t a0 = a & kMask;
  const uint64_t a1 = a >> 32;
  const uint64_t b0 = b & kMask;
  const uint64_t b1 = b >> 32;
  const uint64_t p00 = a0 * b0;
  const uint64_t p01 = a0 * b1;
  const uint64_t p10 = a1 * b0;
  const uint64_t p11 = a1 * b1;
  const uint64_t mid = (p00 >> 32) + (p01 & kMask) + (p10 & kMask);
  return U128{p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32), (mid << 32) | (p00 & kMask)};
}

[[nodiscard]] constexpr U128 operator+(U128 a, U128 b) {
  const uint64_t lo = a.lo + b.lo;
  return U128{a.hi + b.hi + (lo < a.lo ? 1u : 0u), lo};
}

// Requires a >= b.
[[nodiscard]] constexpr U128 operator-(U128 a, U128 b) {
  return U128{a.hi - b.hi - (a.lo < b.lo ? 1u : 0u), a.lo - b.lo};
}

[[nodiscard]] constexpr U128 shiftRight(U128 a, int n) {  // 0 < n < 64
  return U128{a.hi >> n, (a.lo >> n) | (a.hi << (64 - n))};
}

// floor(sqrt(n)) for n < 2^126 (the result fits 64 bits).
[[nodiscard]] constexpr uint64_t isqrt128(U128 n) {
  U128 res{};
  U128 bit{uint64_t{1} << 60, 0};  // 2^124
  while (bit > n)
    bit = shiftRight(bit, 2);
  while (bit != U128{}) {
    const U128 trial = res + bit;
    if (n >= trial) {
      n = n - trial;
      res = shiftRight(res, 1) + bit;
    } else {
      res = shiftRight(res, 1);
    }
    bit = shiftRight(bit, 2);
  }
  return res.lo;
}

// floor(num / den * 2^16) for 0 <= num <= den < 2^62, by restoring division.
[[nodiscard]] constexpr int32_t fractionQ16(uint64_t num, uint64_t den) {
  if (num >= den) return Fixed::kOneRaw;
  uint64_t rem = num;
  int32_t q = 0;
  for (int i = 0; i < Fixed::kShift; ++i) {
    rem <<= 1;
    q <<= 1;
    if (rem >= den) {
      rem -= den;
      q |= 1;
    }
  }
  return q;
}

// --- swept circle narrowphase -------------------------------------------------

struct SweptHit {
  bool hit{false};
  Fixed tHit{};  // fraction of the tick in [0, 1]
};

// Circles A (prevA -> posA) and B (prevB -> posB), both moving linearly over
// the tick, with radii summing to radiusSum. Returns the earliest t in [0, 1]
// at which their distance equals radiusSum while approaching, or t = 0 when
// they already overlap at the start of the tick. A path that only grazes
// (closest approach exactly radiusSum) is not a hit.
//
// Relative motion d(t) = d0 + dv t with d0 = prevB - prevA. |d(t)|^2 = R^2 is
// a t^2 + 2 b t + c = 0 with a = dv.dv, b = d0.dv, c = d0.d0 - R^2, all exact
// in int64 (raw Q16.16 units squared). The discriminant b^2 - a c is formed
// exactly in 128 bits, and the first root is taken in the cancellation-free
// form t = c / (-b + sqrt(b^2 - a c)), so tHit is exact to within one ulp.
[[nodiscard]] constexpr SweptHit sweptCircleHit(Vec2 prevA, Vec2 posA, Vec2 prevB, Vec2 posB,
                                                Fixed radiusSum) {
  const int64_t d0x = static_cast<int64_t>(prevB.x.raw) - prevA.x.raw;
  const int64_t d0y = static_cast<int64_t>(prevB.y.raw) - prevA.y.raw;
  const int64_t dvx = (static_cast<int64_t>(posB.x.raw) - prevB.x.raw) -
                      (static_cast<int64_t>(posA.x.raw) - prevA.x.raw);
  const int64_t dvy = (static_cast<int64_t>(posB.y.raw) - prevB.y.raw) -
                      (static_cast<int64_t>(posA.y.raw) - prevA.y.raw);
  const int64_t r = radiusSum.raw;

  const int64_t c = (d0x * d0x) + (d0y * d0y) - (r * r);
  if (c < 0) return SweptHit{true, Fixed::zero()};  // overlapping at tick start

  const int64_t b = (d0x * dvx) + (d0y * dvy);
  if (b >= 0) return SweptHit{};  // not approaching (also covers dv == 0)

  const auto a = static_cast<uint64_t>((dvx * dvx) + (dvy * dvy));
  const auto negB = static_cast<uint64_t>(-b);
  const U128 bSq = mulU64(negB, negB);
  const U128 ac = mulU64(a, static_cast<uint64_t>(c));
  if (bSq <= ac) return SweptHit{};  // closest approach >= R: miss or graze

  const uint64_t den = negB + isqrt128(bSq - ac);
  const auto num = static_cast<uint64_t>(c);
  if (num > den) return SweptHit{};  // first touch lies beyond this tick
  return SweptHit{true, Fixed::fromRaw(fractionQ16(num, den))};
}

}  // namespace kfm::sim::collision
