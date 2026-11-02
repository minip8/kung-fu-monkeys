#include "kfm/core/vec2.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using kfm::Angle;
using kfm::Fixed;
using namespace kfm::literals;
using Catch::Matchers::WithinAbs;
using kfm::Vec2;

TEST_CASE("Vec2 arithmetic", "[core][vec2]") {
  const Vec2 a{1_fx, 2_fx};
  const Vec2 b{3_fx, -4_fx};
  CHECK((a + b) == Vec2{4_fx, -2_fx});
  CHECK((a - b) == Vec2{-2_fx, 6_fx});
  CHECK((-a) == Vec2{-1_fx, -2_fx});
  CHECK((a * 2) == Vec2{2_fx, 4_fx});
  CHECK((a * Fixed::half()) == Vec2{Fixed::half(), 1_fx});
  CHECK((b / 2) == Vec2{Fixed::fromDouble(1.5), -2_fx});
  CHECK(kfm::dot(a, b).toDouble() == -5.0);
  CHECK(kfm::cross(a, b).toDouble() == -10.0);
  CHECK(kfm::perp(a) == Vec2{-2_fx, 1_fx});
  CHECK(kfm::lengthSq(b).toDouble() == 25.0);
  CHECK(kfm::length(b) == 5_fx);
  CHECK(kfm::distance(a, b) == kfm::length(b - a));
}

TEST_CASE("Vec2 length does not overflow across the full range", "[core][vec2]") {
  // lengthSq(big) would overflow Fixed; length() must still be exact.
  const Vec2 big{Fixed::fromInt(20000), Fixed::fromInt(-20000)};
  CHECK_THAT(kfm::length(big).toDouble(), WithinAbs(20000.0 * std::sqrt(2.0), 1.0 / 65536.0));
  CHECK(kfm::lengthSqRaw(Vec2{Fixed::max(), Fixed::max()}) ==
        2ULL * static_cast<uint64_t>(INT32_MAX) * static_cast<uint64_t>(INT32_MAX));
}

TEST_CASE("Vec2 normalisation and clamping", "[core][vec2]") {
  CHECK(kfm::normalized(Vec2::zero()) == Vec2::zero());
  const Vec2 n = kfm::normalized(Vec2{3_fx, 4_fx});
  CHECK_THAT(n.x.toDouble(), WithinAbs(0.6, 1.0 / 65536.0));
  CHECK_THAT(n.y.toDouble(), WithinAbs(0.8, 1.0 / 65536.0));
  CHECK(kfm::clampLength(Vec2{3_fx, 4_fx}, 10_fx) == Vec2{3_fx, 4_fx});
  const Vec2 c = kfm::clampLength(Vec2{3_fx, 4_fx}, 1_fx);
  CHECK_THAT(kfm::length(c).toDouble(), WithinAbs(1.0, 2.0 / 65536.0));
  CHECK(kfm::clampLength(Vec2::zero(), 1_fx) == Vec2::zero());
}

TEST_CASE("Vec2::fromAngle", "[core][vec2]") {
  CHECK(Vec2::fromAngle(Angle{0}) == Vec2{Fixed::one(), Fixed::zero()});
  CHECK(Vec2::fromAngle(Angle{Angle::kQuarter}, 2_fx) == Vec2{Fixed::zero(), 2_fx});
  const Vec2 d = Vec2::fromAngle(Angle::fromDegrees(45));
  CHECK_THAT(d.x.toDouble(), WithinAbs(std::sqrt(0.5), 2.0 / 65536.0));
  CHECK_THAT(d.y.toDouble(), WithinAbs(std::sqrt(0.5), 2.0 / 65536.0));
}
