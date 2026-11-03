// Dear ImGui debug overlay for kfm_client (architecture §7), toggled with F1:
// frame and tick statistics, renderer stats, checksum and match phase, the
// entity inspector, interpolation policy, pause/step/time-scale/restart
// controls, and feel-tuning sliders over the session's ContentDB copy.
// In network mode: the net panel (connection, RTT/jitter/loss, lead, rate,
// rollback depth histogram, mispredictions, checksums, traffic, net graph)
// instead of the local-only controls and tuning.
#pragma once

#include "core/match_session.hpp"
#include "core/net_session.hpp"
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
  // Network mode. Also feeds the net graph, so call it every frame.
  void draw(NetSession& session, const presentation::RendererStats& renderer);

 private:
  static constexpr std::size_t kFrameHistory = 240;
  std::array<float, kFrameHistory> frameMs_{};
  std::size_t frameHead_{0};
  std::string printedCpp_;
  // Net graph: RTT (ms) and the deepest rollback of each frame (ticks).
  std::array<float, kFrameHistory> rttMs_{};
  std::array<float, kFrameHistory> depth_{};
  std::size_t netHead_{0};
};

}  // namespace kfm::client
