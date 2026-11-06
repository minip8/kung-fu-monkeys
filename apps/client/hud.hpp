// In-game HUD and the play screens (architecture §7: UI is separate from
// gameplay rendering). Drawn with Dear ImGui between debugUiNewFrame() and
// debugUiFlush(): the HUD on the background draw list (over the 3D scene,
// under ImGui windows such as the debug overlay), the lobby, the network
// status, the online room's waiting screen and the match-over screen as
// windows. v0.43: in the game's font and palette (ui_style.hpp); the menus
// before play are screens.hpp.
#pragma once

#include "core/lobby_menu.hpp"
#include "core/match_session.hpp"
#include "core/menus.hpp"
#include "core/net_session.hpp"
#include "core/scoreboard.hpp"
#include "kfm/presentation/camera.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/presentation/theme.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
  sim::PlayerMask bots{0};     // v0.43: slots a bot drives (locally or on the server)
  bool paused{false};
  bool network{false};     // a networked match: no "back to the lobby" by Enter
  std::string statusLine;  // network: a small connection status line
  std::string roomLine;    // M6a: "Room ABCDE (eu)", drawn large above the status line
  std::string shareLine;   // M6a: the invite ("Invite: https://.../?room=ABCDE")
  // Network: the server's announced next match (arena, mode, power-ups),
  // shown under the match-over banner.
  std::string nextMatch;
  // v0.42, online: each slot's nickname (empty: the slot label).
  std::array<std::string, sim::kMaxPlayers> names{};
  // v0.43: the caller draws the match-over screen (drawMatchOver()), so the
  // HUD draws no match-over banner or scoreboard of its own.
  bool overScreen{false};
};
[[nodiscard]] HudView hudView(const MatchSession& session);
[[nodiscard]] HudView hudView(const NetSession& session);
// Each slot's label: its nickname, else P<n> / BOT<n> (scoreboard.hpp).
[[nodiscard]] std::array<std::string, sim::kMaxPlayers> slotLabels(const HudView& view);

// Scores, round, countdown, round/match banners, per-player labels and ammo
// pips (nicknames online, P<n> / BOT<n> locally), dropped-controller and
// pause notices, and the connection status line. v0.43: the scoreboard
// between rounds and, unless view.overScreen, at the match's end.
void drawMatchHud(const HudView& view, const HudInput& in);

// v0.52: the player's arena theme, which the lobby and room panels' arena
// previews are drawn in (main.cpp keeps it in step with the renderer's).
void setHudTheme(presentation::ThemeChoice choice);
inline void drawMatchHud(const MatchSession& session, const HudInput& in) {
  drawMatchHud(hudView(session), in);
}

// v0.43: a room's code with Copy link and, on touch devices, Share.
struct ShareView {
  std::string code;  // empty: no room (nothing drawn)
  std::string region;
  std::string link;      // what Copy link copies (roomInviteLink())
  bool canShare{false};  // IPlatformServices::canShare()
  bool copied{false};    // a copy happened in the last seconds: say so
};
struct ShareOutcome {
  bool copy{false};
  bool share{false};
};

// Network mode before the match is drawn, or after it ended: the connection
// status (connecting, syncing, rejected or disconnected with the reason),
// the server's match once its Welcome arrived (read-only: slot cards,
// arena with a preview, mode with its key numbers, power-up set) and how to
// get back to the menu. M6a: in a room, its code large with Copy link /
// Share; a lobby refusal shows in plain words (NetSession::statusText()).
// v0.44: a lost session that canReconnect() offers Reconnect and Leave
// (`buttons` holds the keyboard / pad focus); after a Reconnect the screen
// says it is reconnecting.
struct NetStatusOutcome {
  ShareOutcome share;
  bool reconnect{false};
  bool leave{false};
};
[[nodiscard]] NetStatusOutcome drawNetStatus(const NetSession& session, const ShareView& share,
                                             ButtonMenu& buttons);

// The local lobby (v0.24, restyled in v0.43): a card per slot (colour,
// label, device or bot), the settings panel (LobbyMenu rows: arena, mode,
// power-up toggles, bots, bot difficulty, volume, mute; mouse, keyboard or
// pad) beside an arena preview and the mode's key numbers, the join help and
// the start button.
struct LobbyOutcome {
  bool start{false};    // the start button was pressed
  bool changed{false};  // a setting changed through the panel's mouse controls
};
[[nodiscard]] LobbyOutcome drawLobby(MatchSession& session, LobbyMenu& menu);

// v0.42, restyled in v0.43: the online room's waiting screen (NetPhase
// Waiting): the room code large with Copy link / Share, the roster as cards
// (nicknames, the host's crown, you, the bots that join at the start, open
// slots), the match: the host edits it with the lobby panel's room rows
// (`menu`, LobbyMenu::setRoomRows()), everyone else sees it read-only, with
// the arena's preview; the host's Start match, everyone's Leave.
struct RoomOutcome {
  bool start{false};            // the host pressed Start match
  bool settingsChanged{false};  // the host changed a row of `menu`
  bool leave{false};
  ShareOutcome share;
};
[[nodiscard]] RoomOutcome drawRoomScreen(const NetSession& session, LobbyMenu& menu,
                                         const ShareView& share);

// v0.43: the match-over screen, locally and in an online room: the winner
// in their colour, the final scoreboard and a row of buttons (`menu` holds
// the focus); `waiting` is a line above them ("Waiting for Bonobo 42 to play
// again").
struct MatchOverView {
  std::string title;
  uint32_t color{0xFFFFFFFFu};  // ImU32
  Scoreboard board;
  std::vector<std::string> buttons;
  std::string waiting;
};
[[nodiscard]] MatchOverView matchOverView(const HudView& view,
                                          const presentation::RenderState& state);
// Returns the index of the button clicked.
[[nodiscard]] std::optional<std::size_t> drawMatchOver(const MatchOverView& view, ButtonMenu& menu);

// v0.46: the pause menu over a running match, the match dimmed behind it:
// its buttons in a column (`menu` holds the focus) and a line saying what
// the pause means (locally the match waits; online it goes on). Returns the
// item clicked.
[[nodiscard]] std::optional<PauseItem> drawPauseMenu(PauseMenu& menu);

}  // namespace kfm::client
