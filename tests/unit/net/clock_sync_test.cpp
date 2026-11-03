#include "kfm/net/clock_sync.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <deque>

using namespace kfm;
using namespace kfm::net;

namespace {
constexpr uint64_t kMs = 1'000'000;
}  // namespace

TEST_CASE("RttEstimator smooths RTT and jitter", "[net][clock]") {
  RttEstimator r;
  CHECK(!r.hasSample());
  r.addSample(100 * kMs);
  CHECK(r.srttNs() == 100 * kMs);
  CHECK(r.jitterNs() == 0);  // no deviation seen yet: the first lead is not inflated
  for (int i = 0; i < 200; ++i)
    r.addSample((i % 2 == 0 ? 90 : 110) * kMs);
  CHECK(r.srttNs() > 95 * kMs);
  CHECK(r.srttNs() < 105 * kMs);
  CHECK(r.jitterNs() > 8 * kMs);
  CHECK(r.jitterNs() <= 12 * kMs);
  CHECK(r.minNs() == 90 * kMs);
  CHECK(r.maxNs() == 110 * kMs);
  CHECK(r.samples() == 201);
}

TEST_CASE("targetLeadTicks follows RTT/2 + jitter margin + 1 tick", "[net][clock]") {
  RttEstimator r;
  r.addSample(0);
  CHECK(targetLeadTicks(r) == 1);
  r.reset();
  r.addSample(100 * kMs);  // 50 ms one way = 3 ticks, rounded up
  CHECK(targetLeadTicks(r) == 4);
  r.reset();
  r.addSample(300 * kMs);  // 150 ms = 9 ticks exactly
  CHECK(targetLeadTicks(r) == 10);
}

TEST_CASE("LeadController holds the rate inside the band and clamps outside", "[net][clock]") {
  LeadController c;
  CHECK(c.ratePpm() == kRateOnePpm);  // no report yet
  c.onArrivalMargin(1);
  CHECK(c.ratePpm() == kRateOnePpm);
  c.reset();
  c.onArrivalMargin(2);
  CHECK(c.ratePpm() == kRateOnePpm);
  c.reset();
  c.onArrivalMargin(-1);  // late by 2 ticks below the band: +2 %
  CHECK(c.ratePpm() == 1'020'000);
  c.reset();
  c.onArrivalMargin(3);  // early by one tick above the band: −1 %
  CHECK(c.ratePpm() == 990'000);
  c.reset();
  c.onArrivalMargin(40);
  CHECK(c.ratePpm() == 980'000);
  CHECK(c.takeJumpTicks() == 0);
}

TEST_CASE("LeadController converges a delayed plant into the +1..+2 band", "[net][clock]") {
  // The plant: the client's lead over the server drifts at (rate − 1) × 60
  // ticks/s; the margin the server reports is the lead minus the one-way
  // delay, seen one RTT later, quantised to whole ticks.
  for (const double oneWayTicks : {0.0, 3.0, 9.0}) {
    for (const double startLead : {0.0, 6.0, 25.0}) {
      LeadController c;
      double lead = startLead;
      std::deque<int8_t> inFlight(static_cast<size_t>(2 * oneWayTicks) + 1, 0);
      bool primed = false;
      double maxRate = 0;
      double minRate = 2;
      for (int tick = 0; tick < 60 * 60; ++tick) {
        const auto margin = static_cast<int8_t>(std::floor(lead - oneWayTicks));
        inFlight.push_back(margin);
        const int8_t seen = inFlight.front();
        inFlight.pop_front();
        if (tick >= static_cast<int>(2 * oneWayTicks)) primed = true;
        if (primed) c.onArrivalMargin(seen);
        const double rate = static_cast<double>(c.ratePpm()) / 1e6;
        maxRate = std::max(maxRate, rate);
        minRate = std::min(minRate, rate);
        lead += rate - 1.0;  // one tick of wall time at this rate
      }
      INFO("one way " << oneWayTicks << " ticks, start lead " << startLead << ", end lead " << lead
                      << ", smoothed margin " << c.smoothedMarginQ8() / 256.0);
      CHECK(c.smoothedMarginQ8() >= 256 - 64);
      CHECK(c.smoothedMarginQ8() <= 512 + 64);
      CHECK(lead - oneWayTicks >= 0.5);
      CHECK(lead - oneWayTicks <= 3.0);
      CHECK(maxRate <= 1.02);
      CHECK(minRate >= 0.98);
    }
  }
}

TEST_CASE("LeadController asks for a forward jump when hopelessly late", "[net][clock]") {
  LeadController c;
  for (int i = 0; i < 100; ++i)
    c.onArrivalMargin(-20);
  const int32_t jump = c.takeJumpTicks();
  CHECK(jump == 22);  // to the band's middle (+1.5), rounded up
  CHECK(!c.hasMargin());
  CHECK(c.takeJumpTicks() == 0);
}

TEST_CASE("TickClock runs exactly kTickHz ticks per second and scales with the rate",
          "[net][clock]") {
  TickClock clock;
  CHECK(clock.advance(5 * kMs, kRateOnePpm, 8) == 0);  // not started
  clock.start(0);
  uint64_t ticks = 0;
  for (uint64_t ms = 1; ms <= 60'000; ++ms)
    ticks += clock.advance(ms * kMs, kRateOnePpm, 8);
  CHECK(ticks == 3600);  // no drift over a minute

  TickClock fast;
  fast.start(0);
  ticks = 0;
  for (uint64_t ms = 1; ms <= 10'000; ++ms)
    ticks += fast.advance(ms * kMs, 1'020'000, 8);
  CHECK(ticks == 612);

  TickClock stalled;
  stalled.start(0);
  CHECK(stalled.advance(1'000 * kMs, kRateOnePpm, 4) == 4);  // a 1 s stall
  CHECK(stalled.droppedTicks() == 56);
  CHECK(stalled.advance(900 * kMs, kRateOnePpm, 4) == 0);  // time going backwards adds nothing
}

TEST_CASE("TickClock::tickWallNs is one tick of wall time at the given rate", "[net][clock]") {
  CHECK(TickClock::tickWallNs(kRateOnePpm) == 16'666'667);
  CHECK(TickClock::tickWallNs(1'020'000) == 16'339'869);  // 102 %: shorter ticks
  CHECK(TickClock::tickWallNs(980'000) == 17'006'803);    // 98 %: longer ticks
  CHECK(TickClock::tickWallNs(0) > 0);
}
