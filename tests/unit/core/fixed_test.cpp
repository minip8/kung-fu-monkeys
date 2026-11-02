#include "kfm/core/fixed.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>

using kfm::Angle;
using kfm::Fixed;
using namespace kfm::literals;
using Catch::Matchers::WithinAbs;

namespace {
constexpr double kUlp = 1.0 / 65536.0;
}

TEST_CASE("Fixed construction and conversion", "[core][fixed]") {
  STATIC_REQUIRE(Fixed::fromInt(1).raw == 65536);
  STATIC_REQUIRE(Fixed::fromInt(-1).raw == -65536);
  STATIC_REQUIRE(Fixed::fromRatio(1, 2).raw == 32768);
  STATIC_REQUIRE(Fixed::fromRatio(-1, 2).raw == -32768);
  STATIC_REQUIRE(Fixed::fromRatio(1, 3).raw == 21845);
  STATIC_REQUIRE((3_fx).raw == 3 * 65536);

  CHECK(Fixed::fromDouble(1.5).toDouble() == 1.5);
  CHECK(Fixed::fromDouble(-1.5).toDouble() == -1.5);
  CHECK(Fixed::fromDouble(0.1).raw == 6554);
  CHECK(Fixed::fromDouble(-0.1).raw == -6554);

  CHECK(Fixed::fromDouble(2.75).floorToInt() == 2);
  CHECK(Fixed::fromDouble(-2.25).floorToInt() == -3);
  CHECK(Fixed::fromDouble(2.5).roundToInt() == 3);
  CHECK(Fixed::fromDouble(2.49).roundToInt() == 2);
  CHECK(Fixed::fromDouble(-2.5).roundToInt() == -2);
  CHECK(Fixed::fromDouble(2.01).ceilToInt() == 3);
  CHECK(Fixed::fromDouble(2.0).ceilToInt() == 2);
  CHECK(Fixed::fromDouble(2.75).frac().toDouble() == 0.75);
}

TEST_CASE("Fixed arithmetic matches a double reference", "[core][fixed]") {
  const double values[] = {0.0, 1.0, -1.0, 0.5, -0.5, 3.25, -7.125, 100.0, -99.99, 0.001, 123.456};
  for (const double a : values) {
    for (const double b : values) {
      const Fixed fa = Fixed::fromDouble(a);
      const Fixed fb = Fixed::fromDouble(b);
      const double ra = fa.toDouble();
      const double rb = fb.toDouble();
      CHECK((fa + fb).toDouble() == ra + rb);
      CHECK((fa - fb).toDouble() == ra - rb);
      // Multiplication truncates toward negative infinity (arithmetic shift).
      CHECK_THAT((fa * fb).toDouble(), WithinAbs(ra * rb, kUlp));
      // Division only where the quotient fits the Q16.16 range.
      if (fb.raw != 0 && std::abs(ra / rb) < 32767.0) {
        CHECK_THAT((fa / fb).toDouble(), WithinAbs(ra / rb, kUlp));
      }
    }
  }
}

TEST_CASE("Fixed scalar operators and helpers", "[core][fixed]") {
  const Fixed x = Fixed::fromDouble(2.5);
  CHECK((x * 4).toDouble() == 10.0);
  CHECK((4 * x).toDouble() == 10.0);
  CHECK((x / 2).toDouble() == 1.25);
  CHECK(kfm::abs(-x) == x);
  CHECK(kfm::min(x, -x) == -x);
  CHECK(kfm::max(x, -x) == x);
  CHECK(kfm::clamp(x, Fixed::zero(), Fixed::one()) == Fixed::one());
  CHECK(kfm::clamp(-x, Fixed::zero(), Fixed::one()) == Fixed::zero());
  CHECK(kfm::floor(x).toDouble() == 2.0);
  CHECK(kfm::floor(-x).toDouble() == -3.0);
  CHECK(kfm::ceil(x).toDouble() == 3.0);
  CHECK(kfm::ceil(-x).toDouble() == -2.0);
  CHECK(kfm::sign(x) == 1);
  CHECK(kfm::sign(-x) == -1);
  CHECK(kfm::sign(Fixed::zero()) == 0);
  CHECK(kfm::lerp(Fixed::zero(), 10_fx, Fixed::half()).toDouble() == 5.0);
  CHECK(Fixed::one() > Fixed::half());
  CHECK(Fixed::zero() == Fixed{});
}

TEST_CASE("Fixed wraps instead of invoking UB on overflow", "[core][fixed]") {
  const Fixed big = Fixed::max();
  const Fixed wrapped = big + Fixed::epsilon();
  CHECK(wrapped == Fixed::min());
  CHECK((Fixed::min() - Fixed::epsilon()) == Fixed::max());
  CHECK((-Fixed::min()) == Fixed::min());
}

TEST_CASE("isqrt is exact", "[core][fixed]") {
  STATIC_REQUIRE(kfm::isqrt(0) == 0);
  STATIC_REQUIRE(kfm::isqrt(1) == 1);
  STATIC_REQUIRE(kfm::isqrt(3) == 1);
  STATIC_REQUIRE(kfm::isqrt(4) == 2);
  STATIC_REQUIRE(kfm::isqrt(15) == 3);
  STATIC_REQUIRE(kfm::isqrt(16) == 4);
  STATIC_REQUIRE(kfm::isqrt(UINT64_MAX) == 0xFFFFFFFFu);
  for (uint64_t n = 0; n < 5000; ++n) {
    const uint64_t r = kfm::isqrt(n);
    CHECK(r * r <= n);
    CHECK((r + 1) * (r + 1) > n);
  }
}

TEST_CASE("Fixed sqrt matches a double reference", "[core][fixed]") {
  const double values[] = {0.0, 0.25, 1.0, 2.0, 4.0, 9.0, 10.0, 100.0, 1000.0, 32000.0, 0.0001};
  for (const double v : values) {
    const Fixed f = Fixed::fromDouble(v);
    CHECK_THAT(kfm::sqrt(f).toDouble(), WithinAbs(std::sqrt(f.toDouble()), kUlp));
  }
  CHECK(kfm::sqrt(4_fx) == 2_fx);
  CHECK(kfm::sqrt(Fixed::fromDouble(2.25)) == Fixed::fromDouble(1.5));
}

TEST_CASE("Angle arithmetic wraps", "[core][fixed]") {
  const Angle a{0xF000};
  const Angle b{0x2000};
  CHECK((a + b).bam == 0x1000);
  CHECK((b - a).bam == 0x3000);
  CHECK(Angle{0x0100}.signedDeltaFrom(Angle{0xFF00}) == 0x0200);
  CHECK(Angle{0xFF00}.signedDeltaFrom(Angle{0x0100}) == -0x0200);
  CHECK(Angle::fromDegrees(0).bam == 0);
  CHECK(Angle::fromDegrees(90).bam == Angle::kQuarter);
  CHECK(Angle::fromDegrees(180).bam == Angle::kHalf);
  CHECK(Angle::fromDegrees(-90).bam == 0xC000);
  CHECK(Angle::fromDegrees(450).bam == Angle::kQuarter);
  CHECK(Angle::fromInput8(64).bam == Angle::kQuarter);
}

TEST_CASE("sin/cos LUT matches a double reference", "[core][fixed]") {
  STATIC_REQUIRE(kfm::sin(Angle{0}) == Fixed::zero());
  STATIC_REQUIRE(kfm::sin(Angle{Angle::kQuarter}) == Fixed::one());
  STATIC_REQUIRE(kfm::sin(Angle{Angle::kHalf}) == Fixed::zero());
  STATIC_REQUIRE(kfm::sin(Angle{0xC000}) == -Fixed::one());
  STATIC_REQUIRE(kfm::cos(Angle{0}) == Fixed::one());
  STATIC_REQUIRE(kfm::cos(Angle{Angle::kHalf}) == -Fixed::one());

  double maxErr = 0.0;
  for (uint32_t bam = 0; bam < 65536; bam += 7) {
    const Angle a{static_cast<uint16_t>(bam)};
    const double rad = static_cast<double>(bam) * 2.0 * std::numbers::pi / 65536.0;
    maxErr = std::max(maxErr, std::abs(kfm::sin(a).toDouble() - std::sin(rad)));
    maxErr = std::max(maxErr, std::abs(kfm::cos(a).toDouble() - std::cos(rad)));
  }
  CHECK(maxErr <= 2.0 * kUlp);
}

TEST_CASE("Fixed::fromDouble saturates and signedDeltaFrom covers the half turn", "[core][fixed]") {
  CHECK(Fixed::fromDouble(1e9) == Fixed::max());
  CHECK(Fixed::fromDouble(-1e9) == Fixed::min());
  CHECK(Angle{0x8000}.signedDeltaFrom(Angle{0}) == -32768);
}
