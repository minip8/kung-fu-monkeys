// 2D vector over Fixed. The sim is strictly 2D (architecture §7).
#pragma once

#include "kfm/core/fixed.hpp"

#include <compare>
#include <cstdint>

namespace kfm {

struct Vec2 {
  Fixed x{};
  Fixed y{};

  [[nodiscard]] static constexpr Vec2 zero() { return Vec2{}; }
  [[nodiscard]] static constexpr Vec2 fromAngle(Angle a, Fixed magnitude = Fixed::one()) {
    return Vec2{cos(a) * magnitude, sin(a) * magnitude};
  }

  friend constexpr Vec2 operator+(Vec2 a, Vec2 b) { return Vec2{a.x + b.x, a.y + b.y}; }
  friend constexpr Vec2 operator-(Vec2 a, Vec2 b) { return Vec2{a.x - b.x, a.y - b.y}; }
  friend constexpr Vec2 operator-(Vec2 a) { return Vec2{-a.x, -a.y}; }
  friend constexpr Vec2 operator*(Vec2 a, Fixed s) { return Vec2{a.x * s, a.y * s}; }
  friend constexpr Vec2 operator*(Fixed s, Vec2 a) { return a * s; }
  friend constexpr Vec2 operator*(Vec2 a, int32_t s) { return Vec2{a.x * s, a.y * s}; }
  friend constexpr Vec2 operator/(Vec2 a, Fixed s) { return Vec2{a.x / s, a.y / s}; }
  friend constexpr Vec2 operator/(Vec2 a, int32_t s) { return Vec2{a.x / s, a.y / s}; }

  constexpr Vec2& operator+=(Vec2 o) { return *this = *this + o; }
  constexpr Vec2& operator-=(Vec2 o) { return *this = *this - o; }
  constexpr Vec2& operator*=(Fixed s) { return *this = *this * s; }
  constexpr Vec2& operator/=(Fixed s) { return *this = *this / s; }

  constexpr auto operator<=>(const Vec2&) const = default;
};

[[nodiscard]] constexpr Fixed dot(Vec2 a, Vec2 b) {
  return a.x * b.x + a.y * b.y;
}
// 2D cross product (z component), positive when b is counter-clockwise of a.
[[nodiscard]] constexpr Fixed cross(Vec2 a, Vec2 b) {
  return a.x * b.y - a.y * b.x;
}
[[nodiscard]] constexpr Vec2 perp(Vec2 a) {
  return Vec2{-a.y, a.x};
}

// Squared length as Fixed. Overflows for lengths above ~181; use length()
// or lengthSqRaw() when the magnitude is not known to be small.
[[nodiscard]] constexpr Fixed lengthSq(Vec2 a) {
  return dot(a, a);
}

// Squared length in Q32.32 as a 64-bit integer. Exact for the full range.
[[nodiscard]] constexpr uint64_t lengthSqRaw(Vec2 a) {
  const int64_t x = a.x.raw;
  const int64_t y = a.y.raw;
  return static_cast<uint64_t>(x * x + y * y);
}

// Exact floor of the length for the full Fixed range (no intermediate overflow).
[[nodiscard]] constexpr Fixed length(Vec2 a) {
  return Fixed::fromRaw(static_cast<int32_t>(isqrt(lengthSqRaw(a))));
}

[[nodiscard]] constexpr Fixed distance(Vec2 a, Vec2 b) {
  return length(b - a);
}

// Unit vector in the direction of a; zero vector when a is zero.
[[nodiscard]] constexpr Vec2 normalized(Vec2 a) {
  const Fixed len = length(a);
  if (len.raw == 0) return Vec2::zero();
  return a / len;
}

// a scaled to at most maxLen.
[[nodiscard]] constexpr Vec2 clampLength(Vec2 a, Fixed maxLen) {
  const Fixed len = length(a);
  if (len <= maxLen || len.raw == 0) return a;
  return a * (maxLen / len);
}

[[nodiscard]] constexpr Vec2 lerp(Vec2 a, Vec2 b, Fixed t) {
  return a + (b - a) * t;
}

}  // namespace kfm
