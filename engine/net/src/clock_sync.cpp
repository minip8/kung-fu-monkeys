#include "kfm/net/clock_sync.hpp"

#include <algorithm>

namespace kfm::net {

// --- RttEstimator ------------------------------------------------------------------

void RttEstimator::addSample(uint64_t rttNs) {
  lastNs_ = rttNs;
  if (samples_ == 0) {
    // Unlike RFC 6298 (rttvar = R/2, an RTO safety margin) the jitter starts
    // at 0: it feeds the initial lead, which must not start at 2×RTT.
    srttNs_ = rttNs;
    rttvarNs_ = 0;
    minNs_ = maxNs_ = rttNs;
  } else {
    const uint64_t dev = rttNs > srttNs_ ? rttNs - srttNs_ : srttNs_ - rttNs;
    rttvarNs_ = rttvarNs_ - rttvarNs_ / 4 + dev / 4;
    srttNs_ = srttNs_ - srttNs_ / 8 + rttNs / 8;
    minNs_ = std::min(minNs_, rttNs);
    maxNs_ = std::max(maxNs_, rttNs);
  }
  ++samples_;
}

int32_t targetLeadTicks(const RttEstimator& rtt, uint32_t jitterMultiplier) {
  const uint64_t ahead = rtt.srttNs() / 2 + rtt.jitterNs() * jitterMultiplier;
  const uint64_t ticks = (ahead * sim::kTickHz + 999'999'999ull) / 1'000'000'000ull;
  return static_cast<int32_t>(std::min<uint64_t>(ticks, 1000)) + 1;
}

// --- LeadController ----------------------------------------------------------------

void LeadController::reset() {
  marginQ8_ = 0;
  last_ = 0;
  samples_ = 0;
}

void LeadController::onArrivalMargin(int8_t margin) {
  last_ = margin;
  const int32_t q8 = int32_t{margin} * 256;
  if (samples_ == 0) {
    marginQ8_ = q8;
  } else {
    // The EMA moves 1/2^k of the way (division truncates toward zero).
    marginQ8_ += (q8 - marginQ8_) / (int32_t{1} << config_.smoothingShift);
  }
  ++samples_;
}

uint32_t LeadController::ratePpm() const {
  if (samples_ == 0) return kRateOnePpm;
  int32_t errQ8 = 0;
  if (marginQ8_ < config_.marginLowQ8) errQ8 = marginQ8_ - config_.marginLowQ8;
  if (marginQ8_ > config_.marginHighQ8) errQ8 = marginQ8_ - config_.marginHighQ8;
  // Early (positive error) slows the clock down, late speeds it up.
  const int64_t delta = -int64_t{errQ8} * config_.gainPpmPerTick / 256;
  const int64_t maxDev = config_.maxDeviationPpm;
  return static_cast<uint32_t>(int64_t{kRateOnePpm} + std::clamp(delta, -maxDev, maxDev));
}

int32_t LeadController::takeJumpTicks() {
  if (samples_ == 0 || marginQ8_ >= config_.jumpBelowQ8) return 0;
  const int32_t mid = (config_.marginLowQ8 + config_.marginHighQ8) / 2;
  const int32_t ticks = (mid - marginQ8_ + 255) / 256;
  reset();
  return ticks;
}

// --- TickClock ---------------------------------------------------------------------

void TickClock::start(uint64_t nowNs) {
  lastNs_ = nowNs;
  acc_ = 0;
  started_ = true;
}

uint32_t TickClock::advance(uint64_t nowNs, uint32_t ratePpm, uint32_t maxTicks) {
  if (!started_) return 0;
  // A clock that went backwards adds nothing; a huge gap is capped at 10 s
  // so the accumulator cannot overflow (10 s × 60 × 1.02e6 ≈ 6e17 units).
  constexpr uint64_t kMaxGapNs = 10'000'000'000ull;
  const uint64_t dt = nowNs > lastNs_ ? std::min(nowNs - lastNs_, kMaxGapNs) : 0;
  lastNs_ = std::max(lastNs_, nowNs);
  acc_ += dt * sim::kTickHz * ratePpm;
  const uint64_t due = acc_ / kUnitsPerTick;
  acc_ -= due * kUnitsPerTick;
  if (due > maxTicks) {
    dropped_ += due - maxTicks;
    return maxTicks;
  }
  return static_cast<uint32_t>(due);
}

uint32_t TickClock::phaseQ16() const {
  return static_cast<uint32_t>((acc_ >> 20) * 65536 / (kUnitsPerTick >> 20));
}

}  // namespace kfm::net
