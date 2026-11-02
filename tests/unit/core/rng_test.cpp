#include "kfm/core/rng.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <type_traits>

using kfm::Fixed;
using namespace kfm::literals;
using kfm::Rng;

TEST_CASE("Rng is deterministic and seed-sensitive", "[core][rng]") {
  STATIC_REQUIRE(std::is_trivially_copyable_v<Rng>);
  Rng a = Rng::seeded(12345);
  Rng b = Rng::seeded(12345);
  Rng c = Rng::seeded(12346);
  for (int i = 0; i < 1000; ++i) {
    CHECK(a.next() == b.next());
  }
  CHECK(a == b);
  CHECK(a != c);
}

TEST_CASE("Rng reference stream (xoshiro128** from state {1,2,3,4})", "[core][rng]") {
  // Reference values from the xoshiro128** algorithm with the raw state
  // {1, 2, 3, 4}; any divergence means the generator changed and every
  // replay checksum with it.
  Rng r;
  r.s = {1, 2, 3, 4};
  CHECK(r.next() == 11520u);
  CHECK(r.next() == 0u);
  CHECK(r.next() == 5927040u);
  CHECK(r.next() == 70819200u);
}

TEST_CASE("Rng golden values for seeded(0) and seeded(0xDEADBEEF)", "[core][rng]") {
  // Golden values recorded from this implementation; a cross-platform
  // determinism guard rather than a correctness proof.
  Rng z = Rng::seeded(0);
  CHECK(z.s == std::array<uint32_t, 4>{0x92CA2F0Eu, 0x3CD6E3F3u, 0x1B147DCCu, 0x4C081DBFu});
  CHECK(z.next() == 0xE308DC58u);
  Rng d = Rng::seeded(0xDEADBEEFull);
  CHECK(d.s == std::array<uint32_t, 4>{0xD3CE9097u, 0x62117CEDu, 0xF2C80841u, 0xA4884171u});
  CHECK(d.next() == 0x897AD89Du);
}

TEST_CASE("Rng ranges stay in bounds", "[core][rng]") {
  Rng r = Rng::seeded(7);
  for (int i = 0; i < 10000; ++i) {
    CHECK(r.below(10) < 10);
    const int32_t v = r.range(-5, 5);
    CHECK(v >= -5);
    CHECK(v <= 5);
    const Fixed u = r.unit();
    CHECK(u >= Fixed::zero());
    CHECK(u < Fixed::one());
    const Fixed b = r.between(2_fx, 3_fx);
    CHECK(b >= 2_fx);
    CHECK(b < 3_fx);
  }
  CHECK(r.below(0) == 0);
  CHECK(r.range(3, 3) == 3);
}

TEST_CASE("Rng::chance respects probability extremes", "[core][rng]") {
  Rng r = Rng::seeded(99);
  for (int i = 0; i < 100; ++i) {
    CHECK_FALSE(r.chance(Fixed::zero()));
    CHECK(r.chance(Fixed::one()));
  }
}

TEST_CASE("Rng::range covers a full 2^32 span", "[core][rng]") {
  Rng r = Rng::seeded(3);
  bool sawNonMin = false;
  for (int i = 0; i < 64; ++i) {
    if (r.range(INT32_MIN, INT32_MAX) != INT32_MIN) sawNonMin = true;
  }
  CHECK(sawNonMin);
}
