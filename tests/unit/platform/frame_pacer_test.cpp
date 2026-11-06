// FramePacer (platform/frame_pacer.hpp): the wait after each present that
// keeps an unsynchronised native loop at the display rate.
#include "kfm/platform/frame_pacer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using kfm::platform::FramePacer;

namespace {

constexpr uint64_t kMs = 1'000'000;
constexpr uint64_t kT0 = 5'000 * kMs;

}  // namespace

TEST_CASE("pacer: off until a rate is set, and never waits then", "[platform][pacer]") {
  FramePacer p;
  CHECK_FALSE(p.active());
  CHECK(p.waitAfterPresent(kT0) == 0);
  p.setRate(60.0);
  CHECK(p.active());
  p.setRate(0.0);
  CHECK_FALSE(p.active());
  CHECK(p.waitAfterPresent(kT0) == 0);
}

TEST_CASE("pacer: the period follows the refresh rate, unusable rates fall back",
          "[platform][pacer]") {
  FramePacer p;
  p.setRate(50.0);
  CHECK(p.periodNs() == 20 * kMs);
  p.setRate(59.96);
  CHECK(p.periodNs() == 16'677'785);
  p.setRate(-1.0);
  CHECK(p.periodNs() == 16'666'667);
  p.setRate(10'000.0);
  CHECK(p.periodNs() == 16'666'667);
}

TEST_CASE("pacer: fast frames start one period apart", "[platform][pacer]") {
  FramePacer p;
  p.setRate(50.0);  // 20 ms
  uint64_t now = kT0;
  CHECK(p.waitAfterPresent(now) == 0);  // the first frame is not delayed
  for (int i = 0; i < 10; ++i) {
    now += 3 * kMs;  // the frame took 3 ms
    const uint64_t wait = p.waitAfterPresent(now);
    CHECK(wait == 17 * kMs);
    now += wait;
    CHECK(now == kT0 + (static_cast<uint64_t>(i + 1) * 20 * kMs));
  }
}

TEST_CASE("pacer: a slow frame keeps the schedule, a stall restarts it", "[platform][pacer]") {
  FramePacer p;
  p.setRate(50.0);
  CHECK(p.waitAfterPresent(kT0) == 0);  // schedule anchored at kT0
  // 25 ms: 5 ms past the next frame's slot, so it starts at once...
  CHECK(p.waitAfterPresent(kT0 + (25 * kMs)) == 0);
  // ...and a quick one after it waits only until its slot at 40 ms.
  CHECK(p.waitAfterPresent(kT0 + (28 * kMs)) == 12 * kMs);
  // A stall of over a period past the slot (80 ms > 60 + 20) restarts from now.
  CHECK(p.waitAfterPresent(kT0 + (120 * kMs)) == 0);
  CHECK(p.waitAfterPresent(kT0 + (123 * kMs)) == 17 * kMs);
}
