// Q16.16 fixed-point number: the only real-number type allowed inside the
// simulation (architecture §5.1). Range ±32767, resolution 1/65536.
//
// All intermediates are int64_t and the final cast to int32_t wraps modulo
// 2^32 (defined behaviour in C++20), so arithmetic never invokes UB. Range
// overflow is the caller's responsibility; the game keeps positions and
// velocities far inside the range.
//
// Floating-point conversions exist for presentation, tooling and tests.
// They must never be called from engine/sim (CI greps for float/double).
#pragma once

#include "kfm/core/assert.hpp"

#include <array>
#include <compare>
#include <cstdint>

namespace kfm {

struct Fixed {
  int32_t raw{0};

  static constexpr int kShift = 16;
  static constexpr int32_t kOneRaw = int32_t{1} << kShift;
  static constexpr int32_t kFracMask = kOneRaw - 1;

  // --- construction -------------------------------------------------------
  [[nodiscard]] static constexpr Fixed fromRaw(int32_t r) { return Fixed{r}; }
  [[nodiscard]] static constexpr Fixed fromInt(int32_t i) {
    return Fixed{static_cast<int32_t>(static_cast<uint32_t>(i) << kShift)};
  }
  // num / den, rounded toward zero. den must be non-zero.
  [[nodiscard]] static constexpr Fixed fromRatio(int32_t num, int32_t den) {
    KFM_ASSERT(den != 0);
    return Fixed{static_cast<int32_t>((static_cast<int64_t>(num) << kShift) / den)};
  }

  [[nodiscard]] static constexpr Fixed zero() { return Fixed{0}; }
  [[nodiscard]] static constexpr Fixed one() { return Fixed{kOneRaw}; }
  [[nodiscard]] static constexpr Fixed half() { return Fixed{kOneRaw / 2}; }
  [[nodiscard]] static constexpr Fixed max() { return Fixed{INT32_MAX}; }
  [[nodiscard]] static constexpr Fixed min() { return Fixed{INT32_MIN}; }
  [[nodiscard]] static constexpr Fixed epsilon() { return Fixed{1}; }
  // pi and 2*pi rounded to nearest representable value.
  [[nodiscard]] static constexpr Fixed pi() { return Fixed{205887}; }
  [[nodiscard]] static constexpr Fixed twoPi() { return Fixed{411775}; }

  // --- conversion ---------------------------------------------------------
  [[nodiscard]] constexpr int32_t floorToInt() const { return raw >> kShift; }
  [[nodiscard]] constexpr int32_t roundToInt() const {
    return static_cast<int32_t>((static_cast<int64_t>(raw) + (kOneRaw / 2)) >> kShift);
  }
  [[nodiscard]] constexpr int32_t ceilToInt() const {
    return static_cast<int32_t>((static_cast<int64_t>(raw) + kFracMask) >> kShift);
  }
  [[nodiscard]] constexpr Fixed frac() const { return Fixed{raw & kFracMask}; }

  // Presentation / tooling / tests only. Never inside engine/sim.
  [[nodiscard]] constexpr float toFloat() const {
    return static_cast<float>(raw) / static_cast<float>(kOneRaw);
  }
  [[nodiscard]] constexpr double toDouble() const {
    return static_cast<double>(raw) / static_cast<double>(kOneRaw);
  }
  [[nodiscard]] static constexpr Fixed fromDouble(double d) {
    const double scaled = d * static_cast<double>(kOneRaw);
    const double rounded = scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5;
    if (rounded >= 2147483647.0) return max();  // saturate instead of UB
    if (rounded <= -2147483648.0) return min();
    return Fixed{static_cast<int32_t>(rounded)};
  }

  // --- arithmetic ---------------------------------------------------------
  friend constexpr Fixed operator+(Fixed a, Fixed b) {
    return Fixed{static_cast<int32_t>(static_cast<int64_t>(a.raw) + b.raw)};
  }
  friend constexpr Fixed operator-(Fixed a, Fixed b) {
    return Fixed{static_cast<int32_t>(static_cast<int64_t>(a.raw) - b.raw)};
  }
  friend constexpr Fixed operator-(Fixed a) {
    return Fixed{static_cast<int32_t>(-static_cast<int64_t>(a.raw))};
  }
  friend constexpr Fixed operator*(Fixed a, Fixed b) {
    return Fixed{static_cast<int32_t>((static_cast<int64_t>(a.raw) * b.raw) >> kShift)};
  }
  friend constexpr Fixed operator*(Fixed a, int32_t b) {
    return Fixed{static_cast<int32_t>(static_cast<int64_t>(a.raw) * b)};
  }
  friend constexpr Fixed operator*(int32_t a, Fixed b) { return b * a; }
  // Division rounds toward zero. Division by zero asserts and saturates.
  friend constexpr Fixed operator/(Fixed a, Fixed b) {
    KFM_ASSERT(b.raw != 0);
    if (b.raw == 0) return a.raw < 0 ? min() : max();
    return Fixed{static_cast<int32_t>((static_cast<int64_t>(a.raw) << kShift) / b.raw)};
  }
  friend constexpr Fixed operator/(Fixed a, int32_t b) {
    KFM_ASSERT(b != 0);
    if (b == 0) return a.raw < 0 ? min() : max();
    return Fixed{static_cast<int32_t>(static_cast<int64_t>(a.raw) / b)};
  }

  constexpr Fixed& operator+=(Fixed o) { return *this = *this + o; }
  constexpr Fixed& operator-=(Fixed o) { return *this = *this - o; }
  constexpr Fixed& operator*=(Fixed o) { return *this = *this * o; }
  constexpr Fixed& operator/=(Fixed o) { return *this = *this / o; }
  constexpr Fixed& operator*=(int32_t o) { return *this = *this * o; }
  constexpr Fixed& operator/=(int32_t o) { return *this = *this / o; }

  constexpr auto operator<=>(const Fixed&) const = default;
};

// Literal helper: 3_fx == Fixed::fromInt(3). `using namespace kfm::literals;`
inline namespace literals {
constexpr Fixed operator""_fx(unsigned long long v) {
  return Fixed::fromInt(static_cast<int32_t>(v));
}
}  // namespace literals

// --- free functions ---------------------------------------------------------

[[nodiscard]] constexpr Fixed abs(Fixed a) {
  return a.raw < 0 ? -a : a;
}
[[nodiscard]] constexpr Fixed min(Fixed a, Fixed b) {
  return a.raw < b.raw ? a : b;
}
[[nodiscard]] constexpr Fixed max(Fixed a, Fixed b) {
  return a.raw > b.raw ? a : b;
}
[[nodiscard]] constexpr Fixed clamp(Fixed v, Fixed lo, Fixed hi) {
  return min(max(v, lo), hi);
}
[[nodiscard]] constexpr Fixed floor(Fixed a) {
  return Fixed{a.raw & ~Fixed::kFracMask};
}
[[nodiscard]] constexpr Fixed ceil(Fixed a) {
  return Fixed::fromInt(a.ceilToInt());
}
[[nodiscard]] constexpr int32_t sign(Fixed a) {
  if (a.raw > 0) return 1;
  if (a.raw < 0) return -1;
  return 0;
}

// a + (b - a) * t, with t in [0, 1].
[[nodiscard]] constexpr Fixed lerp(Fixed a, Fixed b, Fixed t) {
  return a + (b - a) * t;
}

// Integer square root, floor(sqrt(n)), exact for all uint64_t.
[[nodiscard]] constexpr uint32_t isqrt(uint64_t n) {
  uint64_t res = 0;
  uint64_t bit = uint64_t{1} << 62;
  while (bit > n)
    bit >>= 2;
  while (bit != 0) {
    if (n >= res + bit) {
      n -= res + bit;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }
  return static_cast<uint32_t>(res);
}

// floor(sqrt(a)) in Q16.16. Negative input asserts and returns zero.
[[nodiscard]] constexpr Fixed sqrt(Fixed a) {
  KFM_ASSERT(a.raw >= 0);
  if (a.raw <= 0) return Fixed::zero();
  return Fixed{static_cast<int32_t>(isqrt(static_cast<uint64_t>(a.raw) << Fixed::kShift))};
}

// --- angles -----------------------------------------------------------------

// Binary angle: 65536 units per full turn, so arithmetic wraps for free.
// Zero points along +x; increasing angles rotate toward +y.
struct Angle {
  uint16_t bam{0};

  static constexpr uint16_t kQuarter = 0x4000;
  static constexpr uint16_t kHalf = 0x8000;

  [[nodiscard]] static constexpr Angle fromBam(uint16_t b) { return Angle{b}; }
  // Input encodes direction as 1/256 of a turn (architecture §5.4).
  [[nodiscard]] static constexpr Angle fromInput8(uint8_t v) {
    return Angle{static_cast<uint16_t>(static_cast<uint16_t>(v) << 8)};
  }
  [[nodiscard]] static constexpr Angle fromDegrees(int32_t deg) {
    return Angle{
        static_cast<uint16_t>(((static_cast<int64_t>(deg) % 360 + 360) % 360) * 65536 / 360)};
  }

  friend constexpr Angle operator+(Angle a, Angle b) {
    return Angle{static_cast<uint16_t>(a.bam + b.bam)};
  }
  friend constexpr Angle operator-(Angle a, Angle b) {
    return Angle{static_cast<uint16_t>(a.bam - b.bam)};
  }
  constexpr Angle& operator+=(Angle o) { return *this = *this + o; }
  constexpr Angle& operator-=(Angle o) { return *this = *this - o; }

  // Signed shortest difference from `other` to this, in [-32768, 32767]
  // (exactly half a turn maps to -32768).
  [[nodiscard]] constexpr int16_t signedDeltaFrom(Angle other) const {
    return static_cast<int16_t>(static_cast<uint16_t>(bam - other.bam));
  }

  constexpr auto operator<=>(const Angle&) const = default;
};

namespace detail {
#include "kfm/core/sin_table.inc"
}  // namespace detail

// sin/cos via the generated table with linear interpolation between samples.
// Max error against the real function is below 2 ulps of Q16.16.
[[nodiscard]] constexpr Fixed sin(Angle a) {
  constexpr int kIndexBits = 12;  // log2(kSinTableSize)
  constexpr int kFracBits = 16 - kIndexBits;
  constexpr uint32_t kFracMask = (1u << kFracBits) - 1;
  static_assert((1 << kIndexBits) == detail::kSinTableSize);
  const uint32_t i0 = static_cast<uint32_t>(a.bam) >> kFracBits;
  const uint32_t i1 = (i0 + 1) & static_cast<uint32_t>(detail::kSinTableSize - 1);
  const int32_t f = static_cast<int32_t>(static_cast<uint32_t>(a.bam) & kFracMask);
  const int32_t s0 = detail::kSinTable[i0];
  const int32_t s1 = detail::kSinTable[i1];
  return Fixed{s0 + (((s1 - s0) * f) >> kFracBits)};
}
[[nodiscard]] constexpr Fixed cos(Angle a) {
  return sin(a + Angle{Angle::kQuarter});
}

}  // namespace kfm
