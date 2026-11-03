#include "kfm/sim/tags.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace kfm::sim;

TEST_CASE("Tags set, clear and query single tags", "[sim][tags]") {
  Tags t;
  CHECK(t.bits == 0);
  CHECK_FALSE(t.has(Tag::Dashing));
  t.set(Tag::Dashing);
  t.set(Tag::HitStun);
  CHECK(t.has(Tag::Dashing));
  CHECK(t.has(Tag::HitStun));
  CHECK_FALSE(t.has(Tag::PerfectDash));
  t.set(Tag::Dashing);  // idempotent
  CHECK(t.bits == (tagBit(Tag::Dashing) | tagBit(Tag::HitStun)));
  t.clear(Tag::Dashing);
  CHECK_FALSE(t.has(Tag::Dashing));
  CHECK(t.has(Tag::HitStun));
  t.clear(Tag::Dashing);  // clearing an unset tag is a no-op
  CHECK(t == Tags::of(Tag::HitStun));
}

TEST_CASE("Tags hasAll and hasNone follow set semantics", "[sim][tags]") {
  const Tags t = Tags::of(Tag::Dashing) | Tag::PerfectDash;
  CHECK(t.hasAll(Tags{}));
  CHECK(t.hasNone(Tags{}));
  CHECK(t.hasAll(Tags::of(Tag::Dashing)));
  CHECK(t.hasAll(Tags::of(Tag::Dashing) | Tag::PerfectDash));
  CHECK_FALSE(t.hasAll(Tags::of(Tag::Dashing) | Tag::HitStun));
  CHECK(t.hasNone(Tags::of(Tag::HitStun) | Tag::Invulnerable));
  CHECK_FALSE(t.hasNone(Tags::of(Tag::HitStun) | Tag::PerfectDash));
}

TEST_CASE("Tags are usable at compile time", "[sim][tags]") {
  constexpr Tags kT = [] {
    Tags t;
    t.set(Tag::Eliminated);
    return t;
  }();
  STATIC_REQUIRE(kT.has(Tag::Eliminated));
  STATIC_REQUIRE(kT.hasNone(Tags::of(Tag::Shielded)));
  STATIC_REQUIRE(static_cast<int>(Tag::kCount) <= 32);
}
