#include "core/fixed_step_clock.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using kfm::client::FixedStepClock;

namespace {

constexpr uint64_t kT0 = 5'000'000'000ULL;  // arbitrary clock origin
// 1e9 / 60 is not an integer: 3 ticks are exactly 50 ms.
constexpr uint64_t kThreeTicksNs = 50'000'000ULL;

}  // namespace

TEST_CASE("clock: the first advance only starts it", "[client][clock]") {
  FixedStepClock c;
  CHECK(c.advance(kT0) == 0);
  CHECK(c.started());
}

TEST_CASE("clock: exact tick counts for elapsed wall time", "[client][clock]") {
  FixedStepClock c;
  c.reset(kT0);
  CHECK(c.advance(kT0 + kThreeTicksNs) == 3);
  CHECK(c.accumulator() == 0);
  CHECK(c.alpha() == 0.0f);

  // 1 s in 60 frames of 1e9/60 ns rounded: the total is still exactly 60 ticks.
  FixedStepClock d;
  d.reset(0);
  uint64_t total = 0;
  for (uint64_t f = 1; f <= 60; ++f)
    total += d.advance((f * 1'000'000'000ULL) / 60);
  CHECK(total == 60);

  // Just under one tick yields nothing and a fractional alpha.
  FixedStepClock e;
  e.reset(0);
  CHECK(e.advance(16'666'666) == 0);
  CHECK(e.alpha() == Catch::Approx(0.99999996).epsilon(1e-6));
  CHECK(e.advance(16'666'667) == 1);
  CHECK(e.alpha() < 1e-6f);
}

TEST_CASE("clock: the per-frame cap drops the backlog and counts it", "[client][clock]") {
  FixedStepClock c;
  c.reset(kT0);
  c.setMaxTicksPerFrame(4);
  // 10 ticks and a half due at once.
  const uint64_t elapsed = (10 * 1'000'000'000ULL + 500'000'000ULL) / 60;
  CHECK(c.advance(kT0 + elapsed) == 4);
  CHECK(c.stats().droppedTicks == 6);
  CHECK(c.stats().framesWithDrops == 1);
  CHECK(c.alpha() == Catch::Approx(0.5).margin(1e-6));  // the fraction survives
  CHECK(c.stats().ticksPerFrame[4] == 1);
}

TEST_CASE("clock: pause stops ticks and single step hands out one", "[client][clock]") {
  FixedStepClock c;
  c.reset(kT0);
  c.setPaused(true);
  CHECK(c.advance(kT0 + kThreeTicksNs) == 0);
  c.requestStep();
  CHECK(c.advance(kT0 + (2 * kThreeTicksNs)) == 1);
  CHECK(c.advance(kT0 + (3 * kThreeTicksNs)) == 0);
  c.setPaused(false);
  // Time spent paused is not owed afterwards.
  CHECK(c.advance(kT0 + (4 * kThreeTicksNs)) == 3);
}

TEST_CASE("clock: time scale", "[client][clock]") {
  FixedStepClock half;
  half.reset(kT0);
  half.setTimeScale(0.5);
  CHECK(half.advance(kT0 + (2 * kThreeTicksNs)) == 3);
  CHECK(half.tickWallNs() == 33'333'333);

  FixedStepClock twice;
  twice.reset(kT0);
  twice.setTimeScale(2.0);
  CHECK(twice.advance(kT0 + kThreeTicksNs) == 4);  // 6 due, capped at 4
  CHECK(twice.stats().droppedTicks == 2);

  FixedStepClock clamped;
  clamped.setTimeScale(10.0);
  CHECK(clamped.timeScale() == 2.0);
  clamped.setTimeScale(0.0);
  CHECK(clamped.timeScale() == 0.25);
}

TEST_CASE("clock: tick start times tile the frame interval", "[client][clock]") {
  FixedStepClock c;
  c.reset(kT0);
  REQUIRE(c.advance(kT0 + kThreeTicksNs) == 3);
  // The batch ends exactly at now (no leftover): ticks start 50, 33.3, 16.7 ms before.
  CHECK(c.tickStartNs(0) == kT0);
  CHECK(c.tickStartNs(1) == kT0 + 16'666'667);
  CHECK(c.tickStartNs(2) == kT0 + 33'333'333);
  CHECK(c.tickWallNs() == 16'666'667);

  // A leftover accumulator does not move the windows: the batch still ends at
  // now (the frame samples every event up to now), counting back whole ticks.
  FixedStepClock d;
  d.reset(kT0);
  const uint64_t now1 = kT0 + 41'666'667;  // 2.5 ticks
  REQUIRE(d.advance(now1) == 2);
  CHECK(d.alpha() == Catch::Approx(0.5).margin(1e-6));
  CHECK(d.tickStartNs(1) == now1 - 16'666'667);
  CHECK(d.tickStartNs(0) == now1 - 33'333'333);
  // The next 60 Hz frame's single tick starts where the last batch ended.
  const uint64_t now2 = now1 + 16'666'667;
  REQUIRE(d.advance(now2) == 1);
  CHECK(d.alpha() == Catch::Approx(0.5).margin(1e-6));
  CHECK(d.tickStartNs(0) == now1);
  // A paused single step also ends at now.
  d.setPaused(true);
  d.requestStep();
  const uint64_t now3 = now2 + 5'000'000;
  REQUIRE(d.advance(now3) == 1);
  CHECK(d.tickStartNs(0) == now3 - 16'666'667);
}

TEST_CASE("clock: display rate measurement", "[client][clock]") {
  FixedStepClock c;
  c.reset(0);
  CHECK(c.displayHz() == 0.0);
  for (uint64_t f = 1; f <= 120; ++f)
    (void)c.advance(f * 8'333'333ULL);  // 120 Hz
  CHECK(c.displayHz() == Catch::Approx(120.0).epsilon(0.001));
}
