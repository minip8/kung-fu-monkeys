#include "fixed_step_clock.hpp"

#include "kfm/sim/constants.hpp"

#include <algorithm>
#include <cmath>

namespace kfm::client {

namespace {

// Weight of the newest frame interval in the display-rate average.
constexpr double kFrameEmaWeight = 0.1;

}  // namespace

void FixedStepClock::reset(uint64_t nowNs) {
  started_ = true;
  stepRequested_ = false;
  lastNowNs_ = nowNs;
  accumulator_ = 0;
  lastTicks_ = 0;
}

void FixedStepClock::setPaused(bool paused) {
  paused_ = paused;
  if (!paused_) stepRequested_ = false;
}

void FixedStepClock::setTimeScale(double scale) {
  if (std::isnan(scale)) scale = 1.0;
  timeScale_ = std::clamp(scale, kMinTimeScale, kMaxTimeScale);
}

uint32_t FixedStepClock::advance(uint64_t nowNs) {
  if (!started_) {
    reset(nowNs);
    return 0;
  }
  const uint64_t elapsed = nowNs > lastNowNs_ ? nowNs - lastNowNs_ : 0;
  lastNowNs_ = nowNs;
  ++stats_.frames;
  if (elapsed > 0) {
    const auto e = static_cast<double>(elapsed);
    frameNsEma_ = frameNsEma_ <= 0.0 ? e : frameNsEma_ + (kFrameEmaWeight * (e - frameNsEma_));
  }

  uint32_t ticks = 0;
  if (paused_) {
    if (stepRequested_) {
      stepRequested_ = false;
      ticks = 1;
    }
  } else {
    const uint64_t scaled = timeScale_ == 1.0
                                ? elapsed * sim::kTickHz
                                : static_cast<uint64_t>(std::llround(static_cast<double>(elapsed) *
                                                                     sim::kTickHz * timeScale_));
    accumulator_ += scaled;
    uint64_t due = accumulator_ / kTickCost;
    if (due > maxTicksPerFrame_) {
      const uint64_t dropped = due - maxTicksPerFrame_;
      stats_.droppedTicks += dropped;
      ++stats_.framesWithDrops;
      accumulator_ -= dropped * kTickCost;
      due = maxTicksPerFrame_;
    }
    ticks = static_cast<uint32_t>(due);
    accumulator_ -= due * kTickCost;
  }

  lastTicks_ = ticks;
  stats_.ticks += ticks;
  ++stats_.ticksPerFrame[std::min(ticks, kTickHistogramBins - 1)];
  return ticks;
}

uint64_t FixedStepClock::tickWallNs() const {
  return static_cast<uint64_t>(
      std::llround(1e9 / (static_cast<double>(sim::kTickHz) * timeScale_)));
}

uint64_t FixedStepClock::tickStartNs(uint32_t i) const {
  // Units of ns × kTickHz × timeScale before lastNowNs_ at which tick i
  // starts: the batch's last tick ends exactly at lastNowNs_.
  const uint32_t n = std::max(lastTicks_, i + 1);
  const uint64_t unitsBefore = static_cast<uint64_t>(n - i) * kTickCost;
  const double nsBefore =
      static_cast<double>(unitsBefore) / (static_cast<double>(sim::kTickHz) * timeScale_);
  const auto before = static_cast<uint64_t>(std::llround(nsBefore));
  return before >= lastNowNs_ ? 0 : lastNowNs_ - before;
}

float FixedStepClock::alpha() const {
  return static_cast<float>(static_cast<double>(accumulator_) / static_cast<double>(kTickCost));
}

double FixedStepClock::displayHz() const {
  return frameNsEma_ > 0.0 ? 1e9 / frameNsEma_ : 0.0;
}

}  // namespace kfm::client
