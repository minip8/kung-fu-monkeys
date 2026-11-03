// Dear ImGui debug overlay for kfm_client (architecture §7), toggled with F1:
// frame and tick statistics, renderer stats, checksum and match phase, the
// entity inspector, interpolation policy, pause/step/time-scale/restart
// controls, and feel-tuning sliders over the session's ContentDB copy.
#pragma once

#include "core/match_session.hpp"
#include "kfm/presentation/renderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace kfm::client {

class DebugOverlay {
 public:
  bool open{false};

  // Every frame, overlay open or not, so the frame-time graph is continuous.
  void recordFrame(double frameSeconds);
  // Builds the overlay window when open. nowNs is the frame's clock (restarts).
  void draw(MatchSession& session, const presentation::RendererStats& renderer, uint64_t nowNs);

 private:
  static constexpr std::size_t kFrameHistory = 240;
  std::array<float, kFrameHistory> frameMs_{};
  std::size_t frameHead_{0};
  std::string printedCpp_;
};

}  // namespace kfm::client
