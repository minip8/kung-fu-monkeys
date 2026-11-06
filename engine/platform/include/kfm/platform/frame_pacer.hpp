// Frame pacing for a native window whose swap interval cannot be set (no
// vsync: some drivers refuse it, among them Mesa's llvmpipe under WSLg's X11).
// Without it the client loop runs as fast as it can, hundreds of frames a
// second, and the compositor shows an arbitrary, unevenly spaced subset of
// them (tearing, judder). Window::swap() asks the pacer how long to wait after
// presenting so frames start one display period apart. SDL-free and tested
// headlessly; Emscripten never paces (requestAnimationFrame does).
#pragma once

#include <cmath>
#include <cstdint>

namespace kfm::platform {

class FramePacer {
 public:
  // Display refresh rates outside [kMinHz, kMaxHz] (or unknown, 0) pace at
  // kFallbackHz.
  static constexpr double kFallbackHz = 60.0;
  static constexpr double kMinHz = 24.0;
  static constexpr double kMaxHz = 500.0;

  // Paces at `refreshHz` (the display's mode); 0 disables pacing.
  void setRate(double refreshHz) {
    if (refreshHz == 0.0) {
      periodNs_ = 0;
    } else {
      const double hz = refreshHz >= kMinHz && refreshHz <= kMaxHz ? refreshHz : kFallbackHz;
      periodNs_ = static_cast<uint64_t>(std::llround(1e9 / hz));
    }
    dueNs_ = 0;
  }
  [[nodiscard]] uint64_t periodNs() const { return periodNs_; }
  [[nodiscard]] bool active() const { return periodNs_ != 0; }

  // Called right after a frame was presented at `nowNs`: how long to wait
  // before the next frame starts. Frame starts are due one period apart; a
  // frame that overran its slot by less than a period starts the next one at
  // once and keeps the schedule (the rate holds), and one that overran by a
  // whole period or more restarts the schedule from now rather than rushing
  // through a backlog. The first frame is never delayed.
  [[nodiscard]] uint64_t waitAfterPresent(uint64_t nowNs) {
    if (periodNs_ == 0) return 0;
    const uint64_t next = dueNs_ + periodNs_;
    if (dueNs_ == 0 || nowNs >= next + periodNs_) {
      dueNs_ = nowNs;
      return 0;
    }
    dueNs_ = next;
    return nowNs < next ? next - nowNs : 0;
  }

 private:
  uint64_t periodNs_{0};
  uint64_t dueNs_{0};  // when the frame being presented was due to start; 0 = none yet
};

}  // namespace kfm::platform
