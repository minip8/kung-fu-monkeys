// Fixed-step clock for the client (architecture §6.8): turns wall time into a
// whole number of 60 Hz sim ticks per frame, with a per-frame cap so a
// stalled frame never spirals, plus pause, single step and time scale.
//
// Time is accumulated exactly in units of ns × kTickHz, so one tick costs
// exactly 1e9 units, i.e. 1e9 / kTickHz ns of wall time (16 666 666.67 ns at
// 60 Hz), and the tick rate never drifts. With a time scale other than 1 the
// scaled elapsed time is rounded to the nearest unit per frame.
//
// Per frame:
//   const uint32_t n = clock.advance(nowNs);
//   for (uint32_t i = 0; i < n; ++i) {
//     sample inputs for [tickStartNs(i), tickStartNs(i) + tickWallNs()); step();
//   }
//   draw with alpha();
#pragma once

#include <array>
#include <cstdint>

namespace kfm::client {

inline constexpr uint32_t kDefaultMaxTicksPerFrame = 4;
// Histogram bins for ticks stepped per frame: 0, 1, ..., kTickHistogramBins - 1
// (the last bin also counts anything larger).
inline constexpr uint32_t kTickHistogramBins = 9;

struct ClockStats {
  uint64_t frames{0};           // advance() calls after the first
  uint64_t ticks{0};            // ticks handed out
  uint64_t droppedTicks{0};     // backlog ticks discarded by the per-frame cap
  uint64_t framesWithDrops{0};  // frames that discarded backlog
  std::array<uint64_t, kTickHistogramBins> ticksPerFrame{};
};

class FixedStepClock {
 public:
  // One tick in accumulator units (ns × kTickHz).
  static constexpr uint64_t kTickCost = 1'000'000'000;
  static constexpr double kMinTimeScale = 0.25;
  static constexpr double kMaxTimeScale = 2.0;

  // Starts (or restarts) at nowNs with an empty accumulator. Stats, pause,
  // time scale and the display-rate measurement are kept.
  void reset(uint64_t nowNs);
  [[nodiscard]] bool started() const { return started_; }

  // Feeds the frame's wall time and returns how many ticks to step now
  // (0..maxTicksPerFrame()). The first call on an unstarted clock only starts
  // it and returns 0. While paused it returns 0, or 1 once after
  // requestStep(); the accumulator does not move while paused.
  uint32_t advance(uint64_t nowNs);

  // Wall-clock start of the i-th tick handed out by the last advance(), on the
  // nowNs clock; each tick spans tickWallNs(). The last tick of the batch ends
  // at that advance()'s nowNs minus the leftover accumulator.
  [[nodiscard]] uint64_t tickStartNs(uint32_t i) const;
  // Wall duration of one tick at the current time scale.
  [[nodiscard]] uint64_t tickWallNs() const;

  // Fraction of the next tick already accumulated, in [0, 1).
  [[nodiscard]] float alpha() const;
  [[nodiscard]] uint64_t accumulator() const { return accumulator_; }

  void setPaused(bool paused);
  [[nodiscard]] bool paused() const { return paused_; }
  // While paused, the next advance() hands out exactly one tick.
  void requestStep() { stepRequested_ = true; }

  // Clamped to [kMinTimeScale, kMaxTimeScale].
  void setTimeScale(double scale);
  [[nodiscard]] double timeScale() const { return timeScale_; }

  // At least 1.
  void setMaxTicksPerFrame(uint32_t cap) { maxTicksPerFrame_ = cap == 0 ? 1 : cap; }
  [[nodiscard]] uint32_t maxTicksPerFrame() const { return maxTicksPerFrame_; }

  // Smoothed wall time between advance() calls (exponential moving average),
  // 0 before two calls; and the display rate it implies (0 if unknown).
  [[nodiscard]] double frameNs() const { return frameNsEma_; }
  [[nodiscard]] double displayHz() const;

  [[nodiscard]] uint32_t lastTicks() const { return lastTicks_; }
  [[nodiscard]] const ClockStats& stats() const { return stats_; }
  void resetStats() { stats_ = {}; }

 private:
  bool started_{false};
  bool paused_{false};
  bool stepRequested_{false};
  double timeScale_{1.0};
  uint32_t maxTicksPerFrame_{kDefaultMaxTicksPerFrame};
  uint64_t lastNowNs_{0};
  uint64_t accumulator_{0};     // units of ns × kTickHz; < kTickCost between frames
  uint64_t batchTailUnits_{0};  // the last batch's final tick ended this long before lastNowNs_
  double frameNsEma_{0.0};
  uint32_t lastTicks_{0};
  ClockStats stats_{};
};

}  // namespace kfm::client
