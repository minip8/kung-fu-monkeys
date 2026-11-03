// In-game HUD and lobby screen (architecture §7: UI is separate from gameplay
// rendering). Drawn with Dear ImGui between debugUiNewFrame() and
// debugUiFlush(): the HUD on the background draw list (over the 3D scene,
// under ImGui windows such as the debug overlay), the lobby as a window.
#pragma once

#include "core/lobby_menu.hpp"
#include "core/match_session.hpp"
#include "core/net_session.hpp"
#include "kfm/presentation/camera.hpp"
#include "kfm/presentation/render_state.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace kfm::client {

struct HudInput {
  const presentation::RenderState* state{nullptr};
  const presentation::Camera* camera{nullptr};  // framebuffer pixels
  float uiScale{1.0f};                          // ImGui units per framebuffer pixel
};

// What the HUD needs to know about the slots, from either session.
struct HudView {
  std::array<SlotOwner, sim::kMaxPlayers> owners{};
  sim::PlayerMask dropped{0};  // local slots whose controller is gone
  bool paused{false};
  bool network{false};     // a networked match: no "back to the lobby" by Enter
  std::string statusLine;  // network: a small connection status line
  // Network: the server's announced next match (arena, mode, power-ups),
  // shown under the match-over banner.
  std::string nextMatch;
};
[[nodiscard]] HudView hudView(const MatchSession& session);
[[nodiscard]] HudView hudView(const NetSession& session);

// Scores, round, countdown, round/match banners, per-player labels and ammo
// pips (P<n> for humans, local or remote, BOT<n> for local bots),
// dropped-controller and pause notices, and the connection status line.
void drawMatchHud(const HudView& view, const HudInput& in);
inline void drawMatchHud(const MatchSession& session, const HudInput& in) {
  drawMatchHud(hudView(session), in);
}

// Network mode before the match is drawn, or after it ended: the connection
// status (connecting, syncing, rejected or disconnected with the reason), the
// server's match once its Welcome arrived (read-only: slot cards, arena with
// a preview, mode with its key numbers, power-up set) and how to get back to
// the local lobby.
void drawNetStatus(const NetSession& session);

// The lobby (v0.24): a card per slot (colour, label, device or bot), then
// locally the settings panel (LobbyMenu rows: arena, mode, power-up toggles,
// bots, volume, mute; mouse, keyboard or pad) beside an arena preview and
// the mode's key numbers, the join help and the start button. With a
// connectUrl (network mode) the settings are hidden: the server owns them
// and drawNetStatus() shows them once connected.
struct LobbyOutcome {
  bool start{false};    // the start (or connect) button was pressed
  bool changed{false};  // a setting changed through the panel's mouse controls
};
[[nodiscard]] LobbyOutcome drawLobby(MatchSession& session, LobbyMenu& menu,
                                     const std::string& connectUrl = {});

}  // namespace kfm::client
