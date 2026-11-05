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
  std::string roomLine;    // M6a: "Room ABCDE (eu)", drawn large above the status line
  std::string shareLine;   // M6a: the invite ("Invite: https://.../?room=ABCDE")
  // Network: the server's announced next match (arena, mode, power-ups),
  // shown under the match-over banner.
  std::string nextMatch;
  // v0.42, online: each human slot's nickname (empty: the slot label).
  std::array<std::string, sim::kMaxPlayers> names{};
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
// the local lobby. M6a: in a room, the room's code and region in large type
// and `invite` (the link or flags that join it); a lobby refusal shows in
// plain words (NetSession::statusText()).
void drawNetStatus(const NetSession& session, const std::string& invite = {});

// The lobby (v0.24): a card per slot (colour, label, device or bot), then
// locally the settings panel (LobbyMenu rows: arena, mode, power-up toggles,
// bots, volume, mute; mouse, keyboard or pad) beside an arena preview and
// the mode's key numbers, the join help and the start button. With an
// onlineLabel (network mode: "server ws://...", "room ABCDE (lobby ...)")
// the settings are hidden: the server owns them and drawNetStatus() shows
// them once connected. M6a: with a lobby URL (LobbyMenu::lobbyAvailable())
// the Online section follows the rows: Create room (a button beside the
// region) and a room code field (upper case, the room alphabet only) with
// Join, both handed to the menu (activate() / requestJoin()).
struct LobbyOutcome {
  bool start{false};    // the start (or connect) button was pressed
  bool changed{false};  // a setting changed through the panel's mouse controls
};
// v0.42: the online room while it waits or shows its match over (the
// NetSession's Waiting and Over phases). Functional, not final (M6b step 3
// restyles it): the room code and invite, the roster (nicknames, the host,
// you, bots); waiting, the match the room will play: the host edits it with
// the lobby panel's rows (`menu`: arena, mode, teams, loadout, power-ups,
// bots), everyone else sees it read-only; over, the winner and the scores.
// The host gets Start match / Play again, the others a waiting line.
struct RoomOutcome {
  bool start{false};            // the host pressed Start match / Play again
  bool settingsChanged{false};  // the host changed a row of `menu`
};
[[nodiscard]] RoomOutcome drawRoomScreen(const NetSession& session, LobbyMenu& menu,
                                         const std::string& invite = {});

[[nodiscard]] LobbyOutcome drawLobby(MatchSession& session, LobbyMenu& menu,
                                     const std::string& onlineLabel = {});

}  // namespace kfm::client
