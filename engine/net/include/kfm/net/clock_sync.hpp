// Clock sync and lead control (architecture §6.2). Integer-only: nothing in
// here that decides which tick an input belongs to uses floating point.
//
//   RttEstimator   smoothed RTT and jitter from Ping/Pong samples, the RFC
//                  6298 estimator (srtt gain 1/8, rttvar gain 1/4).
//   LeadController turns the server's arrivalMargin reports into a tick-rate
//                  multiplier: the smoothed margin is steered into the band
//                  [+1, +2] ticks by running the client clock at 98–102 %.
//                  A margin hopelessly late (below jumpBelowQ8) asks for a
//                  forward jump instead; the client never jumps backwards.
//   TickClock      the client's fixed-step clock, rate-controlled, with no
//                  drift: time accumulates in ns × kTickHz × ppm units so one
//                  tick at 100 % costs exactly 1e15 units.
//
// The lead target of §6.2, lead ≈ RTT/2 + jitterMargin + 1 tick, sets the
// lead on initial sync and reconnect (targetLeadTicks); from then on only the
// margin feedback moves it.
#pragma once

#include "kfm/sim/constants.hpp"

#include <cstdint>

namespace kfm::net {

// One tick in ns, truncated (16'666'666). Only for estimates; the clocks
// below accumulate exactly.
inline constexpr uint64_t kTickNs = 1'000'000'000ull / sim::kTickHz;
// Rates are in parts per million of real time.
inline constexpr uint32_t kRateOnePpm = 1'000'000;

class RttEstimator {
 public:
  void addSample(uint64_t rttNs);
  void reset() { *this = RttEstimator{}; }

  [[nodiscard]] bool hasSample() const { return samples_ > 0; }
  [[nodiscard]] uint64_t srttNs() const { return srttNs_; }
  // Mean deviation (RFC 6298 rttvar): the jitter estimate.
  [[nodiscard]] uint64_t jitterNs() const { return rttvarNs_; }
  [[nodiscard]] uint64_t lastNs() const { return lastNs_; }
  [[nodiscard]] uint64_t minNs() const { return minNs_; }
  [[nodiscard]] uint64_t maxNs() const { return maxNs_; }
  [[nodiscard]] uint64_t samples() const { return samples_; }

 private:
  uint64_t srttNs_{0};
  uint64_t rttvarNs_{0};
  uint64_t lastNs_{0};
  uint64_t minNs_{0};
  uint64_t maxNs_{0};
  uint64_t samples_{0};
};

// §6.2 lead target in whole ticks, rounded up:
// ceil((srtt/2 + jitterMultiplier·jitter) / tick) + 1.
[[nodiscard]] int32_t targetLeadTicks(const RttEstimator& rtt, uint32_t jitterMultiplier = 2);

struct LeadControllerConfig {
  // Target band of the smoothed arrival margin, Q8 ticks (§6.2: +1..+2).
  int32_t marginLowQ8{1 * 256};
  int32_t marginHighQ8{2 * 256};
  // Rate change per tick of error outside the band, and its clamp (98–102 %).
  uint32_t gainPpmPerTick{10'000};
  uint32_t maxDeviationPpm{20'000};
  // EMA gain of the margin, 1 / 2^smoothingShift per report.
  uint8_t smoothingShift{3};
  // A smoothed margin below this (Q8) jumps the clock forward to the band.
  int32_t jumpBelowQ8{-8 * 256};
};

class LeadController {
 public:
  explicit LeadController(const LeadControllerConfig& config = {}) : config_(config) {}

  void reset();
  // One arrivalMargin report (ticks, positive = early).
  void onArrivalMargin(int8_t margin);

  [[nodiscard]] bool hasMargin() const { return samples_ > 0; }
  [[nodiscard]] int32_t smoothedMarginQ8() const { return marginQ8_; }
  [[nodiscard]] int8_t lastMargin() const { return last_; }
  // Clock rate for the client's TickClock: 1e6 inside the band, faster when
  // late, slower when early, clamped to 1e6 ± maxDeviationPpm.
  [[nodiscard]] uint32_t ratePpm() const;
  // Ticks to jump forward now when the margin is hopelessly late (0
  // otherwise). The smoothed margin is reset; the caller ignores reports for
  // inputs sent before the jump.
  [[nodiscard]] int32_t takeJumpTicks();
  [[nodiscard]] const LeadControllerConfig& config() const { return config_; }

 private:
  LeadControllerConfig config_;
  int32_t marginQ8_{0};
  int8_t last_{0};
  uint64_t samples_{0};
};

class TickClock {
 public:
  // Starts counting at nowNs with an empty accumulator.
  void start(uint64_t nowNs);
  [[nodiscard]] bool started() const { return started_; }
  // Ticks due between the previous call and nowNs at the given rate. At most
  // maxTicks are returned; a larger backlog is dropped and counted (a stalled
  // host must not simulate seconds at once; the lead controller recovers).
  [[nodiscard]] uint32_t advance(uint64_t nowNs, uint32_t ratePpm, uint32_t maxTicks);
  [[nodiscard]] uint64_t droppedTicks() const { return dropped_; }
  // Fraction of the next tick already elapsed, in 1/65536 (render alpha).
  [[nodiscard]] uint32_t phaseQ16() const;

  // Accumulator units per tick: 1e9 ns × kTickHz × 1e6 ppm / kTickHz.
  static constexpr uint64_t kUnitsPerTick = 1'000'000'000ull * kRateOnePpm;

 private:
  uint64_t lastNs_{0};
  uint64_t acc_{0};
  uint64_t dropped_{0};
  bool started_{false};
};

}  // namespace kfm::net
