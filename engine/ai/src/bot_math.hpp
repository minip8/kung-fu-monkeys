// Fixed-point helpers for bots that read only the Observation (internal to
// engine/ai). The reference bot is the worked example of every SDK, so it
// uses nothing an SDK author could not write: these are integer re-statements
// of what the observation's units mean, not calls into the sim.
#pragma once

#include "kfm/ai/observation.hpp"
#include "kfm/core/fixed.hpp"
#include "kfm/core/vec2.hpp"

#include <cstdint>

namespace kfm::ai::botmath {

[[nodiscard]] constexpr Vec2 vec(int32_t xRaw, int32_t yRaw) {
  return Vec2{Fixed::fromRaw(xRaw), Fixed::fromRaw(yRaw)};
}
[[nodiscard]] constexpr Vec2 vec(const ObsPoint& p) {
  return vec(p.x, p.y);
}

// Deterministic integer atan2: the heading of v in BAM (0 = +x, a quarter
// turn = +y), the zero vector mapping to 0. Reduce into the first quadrant by
// exact quarter-turn rotations, then binary-search the largest angle whose
// LUT direction is not counter-clockwise past v (an exact int64 cross
// product). The same method as the sim's projectile steering, so a bot that
// predicts a returning banana steers it the way the sim does.
[[nodiscard]] constexpr Angle angleOf(Vec2 v) {
  int64_t x = v.x.raw;
  int64_t y = v.y.raw;
  if (x == 0 && y == 0) return Angle{};
  uint16_t quadrant = 0;
  if (x <= 0 && y > 0) {
    const int64_t t = x;
    x = y;
    y = -t;
    quadrant = 1;
  } else if (x < 0 && y <= 0) {
    x = -x;
    y = -y;
    quadrant = 2;
  } else if (x >= 0 && y < 0) {
    const int64_t t = x;
    x = -y;
    y = t;
    quadrant = 3;
  }
  uint32_t lo = 0;
  uint32_t hi = Angle::kQuarter;
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

// Nearest 1/256 turn (the PlayerInput direction byte).
[[nodiscard]] constexpr uint8_t toInput8(Angle a) {
  return static_cast<uint8_t>(((uint32_t{a.bam} + 128u) >> 8) & 0xFFu);
}

[[nodiscard]] constexpr uint8_t directionTo(Vec2 from, Vec2 to) {
  return toInput8(angleOf(to - from));
}

[[nodiscard]] constexpr Vec2 unitOf(uint8_t dir) {
  return Vec2::fromAngle(Angle::fromInput8(dir));
}

// r·r for a radius, in raw Q32 (the unit of lengthSqRaw).
[[nodiscard]] constexpr uint64_t squareRaw(Fixed r) {
  const int64_t v = r.raw < 0 ? -int64_t{r.raw} : int64_t{r.raw};
  return static_cast<uint64_t>(v * v);
}

// min over t in [0, 1] of |r + t·d|², raw Q32: the closest two points come
// while both move linearly through one tick (r = relative start, d =
// relative displacement over the tick). Exact up to the Q16.16 rounding of t,
// for |r| up to 64 units and |d| up to 8 units.
[[nodiscard]] constexpr uint64_t segmentMinSq(Vec2 r, Vec2 d) {
  const int64_t rd = (int64_t{r.x.raw} * d.x.raw) + (int64_t{r.y.raw} * d.y.raw);
  if (rd >= 0) return lengthSqRaw(r);
  const int64_t dd = (int64_t{d.x.raw} * d.x.raw) + (int64_t{d.y.raw} * d.y.raw);
  if (-rd >= dd) return lengthSqRaw(r + d);
  const Fixed t = Fixed::fromRaw(static_cast<int32_t>(((-rd) << Fixed::kShift) / dd));
  return lengthSqRaw(r + (d * t));
}

// Whether the circle at c with radius r overlaps the box (touching does not).
[[nodiscard]] constexpr bool circleOverlaps(const ObsBox& b, Vec2 c, Fixed r) {
  const int64_t dx =
      int64_t{c.x.raw} - clamp(c.x, Fixed::fromRaw(b.minX), Fixed::fromRaw(b.maxX)).raw;
  const int64_t dy =
      int64_t{c.y.raw} - clamp(c.y, Fixed::fromRaw(b.minY), Fixed::fromRaw(b.maxY)).raw;
  return static_cast<uint64_t>((dx * dx) + (dy * dy)) < squareRaw(r);
}

}  // namespace kfm::ai::botmath
