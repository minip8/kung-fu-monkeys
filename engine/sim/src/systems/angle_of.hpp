// Deterministic integer atan2 over Fixed vectors (internal to engine/sim).
// Core has sin/cos but no inverse; ProjectileSystem needs the heading of a
// velocity and of the direction to a boomerang's owner.
//
// Method: reduce the vector into the first quadrant by exact quarter-turn
// rotations, then binary-search the largest quadrant angle `a` whose LUT
// direction (cos a, sin a) is not counter-clockwise past the vector
// (cross(dir(a), v) >= 0, an exact int64 test). Fourteen iterations of two LUT
// lookups each; every step is integer arithmetic, so the result is identical
// on every platform. Accuracy is bounded by the sin table (< 2 ulps of
// Q16.16), i.e. within a couple of BAM of the true angle, rounded down.
#pragma once

#include "kfm/core/fixed.hpp"
#include "kfm/core/vec2.hpp"

#include <cstdint>

namespace kfm::sim {

// Heading of v in BAM (0 = +x, quarter = +y). The zero vector maps to 0.
[[nodiscard]] constexpr Angle angleOf(Vec2 v) {
  int64_t x = v.x.raw;
  int64_t y = v.y.raw;
  if (x == 0 && y == 0) return Angle{};

  // Rotate by -quadrant * 90 degrees so that x > 0, y >= 0.
  uint16_t quadrant = 0;
  if (x <= 0 && y > 0) {  // second quadrant: (x, y) -> (y, -x)
    const int64_t t = x;
    x = y;
    y = -t;
    quadrant = 1;
  } else if (x < 0 && y <= 0) {  // third: (x, y) -> (-x, -y)
    x = -x;
    y = -y;
    quadrant = 2;
  } else if (x >= 0 && y < 0) {  // fourth: (x, y) -> (-y, x)
    const int64_t t = x;
    x = -y;
    y = t;
    quadrant = 3;
  }

  // Largest a in [0, quarter) with cross(dir(a), (x, y)) >= 0. At a = 0 the
  // cross product is y * 65536 >= 0, so lo always satisfies the predicate.
  uint32_t lo = 0;
  uint32_t hi = Angle::kQuarter;  // predicate false at a quarter turn (x > 0)
  while (hi - lo > 1) {
    const uint32_t mid = (lo + hi) / 2;
    const Angle a = Angle::fromBam(static_cast<uint16_t>(mid));
    const int64_t c =
        (static_cast<int64_t>(cos(a).raw) * y) - (static_cast<int64_t>(sin(a).raw) * x);
    if (c >= 0) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return Angle::fromBam(
      static_cast<uint16_t>(lo + (static_cast<uint32_t>(quadrant) * Angle::kQuarter)));
}

}  // namespace kfm::sim
