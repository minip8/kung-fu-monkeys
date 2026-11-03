// In-game HUD and lobby screen (architecture §7: UI is separate from gameplay
// rendering). Drawn with Dear ImGui between debugUiNewFrame() and
// debugUiFlush(): the HUD on the background draw list (over the 3D scene,
// under ImGui windows such as the debug overlay), the lobby as a window.
#pragma once

#include "core/match_session.hpp"
#include "kfm/presentation/camera.hpp"
#include "kfm/presentation/render_state.hpp"

#include <cstdint>

namespace kfm::client {

struct HudInput {
  const presentation::RenderState* state{nullptr};
  const presentation::Camera* camera{nullptr};  // framebuffer pixels
  float uiScale{1.0f};                          // ImGui units per framebuffer pixel
};

// Scores, round, countdown, round/match banners, per-player labels and ammo
// pips, dropped-controller and pause notices.
void drawMatchHud(const MatchSession& session, const HudInput& in);

// The lobby: bound devices, join prompt with the default bindings, bot count
// and the start button. Returns true when the start button was pressed.
[[nodiscard]] bool drawLobby(MatchSession& session);

}  // namespace kfm::client
