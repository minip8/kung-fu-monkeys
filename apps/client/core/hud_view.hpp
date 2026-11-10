// The HUD's and the match-over screen's view models (architecture §7, the
// v0.43 rule: screen state lives in headless models here, hud.cpp and
// screens.cpp only draw them). Pure functions of the sessions and the
// HUD's view of the match (presentation::RenderHud), tested without a
// window (tests/unit/client/hud_view_test.cpp):
//
//   HudView / hudView()    what the HUD knows about the slots, from a local
//                          match, an online session or a replay.
//   winnerTitle()          the banner title of a round or match winner
//                          ("Team 2" or the slot's label) and the palette
//                          index it is drawn in.
//   clockUrgent()          the mode clock's last seconds (drawn red).
//   MatchOverView          the match-over screen: title, colour, the final
//                          scoreboard and typed buttons (MatchOverAction),
//                          locally (Play again, Back to lobby) and in an
//                          online room (the host's Play again, Leave).
//   roomBotPreview()       the slots the room's bots will take at Start.
#pragma once

#include "kfm/net/messages.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/input.hpp"
#include "match_session.hpp"
#include "scoreboard.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kfm::client {

class NetSession;
class ReplaySession;

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
  // M7 step 6: a replay (ReplaySession): every slot is a spectator's view
  // (P<n>, no "you"), and the match-over banner says the replay ended.
  bool replay{false};
  // M7 step 6: the pixels at the bottom a control bar takes (the replay's,
  // replayBarInset()); the between-rounds scoreboard stays above them.
  float bottomInset{0.0f};
};
[[nodiscard]] HudView hudView(const MatchSession& session);
[[nodiscard]] HudView hudView(const NetSession& session);
// The recorded slots as Remote (labelled P<n>: a replay carries no names).
[[nodiscard]] HudView hudView(const ReplaySession& session);
// Each slot's label: its nickname, else P<n> / BOT<n> (scoreboard.hpp).
[[nodiscard]] std::array<std::string, sim::kMaxPlayers> slotLabels(const HudView& view);
// The rows the scoreboard marks "you": this client's slots online; none in
// local play, where several players share the screen.
[[nodiscard]] sim::PlayerMask onlineLocalMask(const HudView& view);

// The banner title of a round or match winner: "Team 2" in a team mode, the
// slot's label otherwise, and the palette index to draw it in (a team is
// drawn in the colour of its index, team 1 red, team 2 blue, ...; a slot in
// its own).
struct WinnerTitle {
  std::string text;
  uint8_t colorIndex{0};
  bool operator==(const WinnerTitle&) const = default;
};
[[nodiscard]] WinnerTitle winnerTitle(const HudView& view, const presentation::RenderHud& hud,
                                      uint8_t winner);

// The mode clock turns red (and stays so) in its last ten seconds of play.
inline constexpr float kClockUrgentSeconds = 10.0f;
[[nodiscard]] bool clockUrgent(const presentation::RenderHud& hud);

// --- the match-over screen (v0.43) ------------------------------------------------

enum class MatchOverAction : uint8_t { PlayAgain = 0, BackToLobby, Leave };
[[nodiscard]] const char* matchOverLabel(MatchOverAction action);

// The local match-over screen's buttons, in order.
[[nodiscard]] std::span<const MatchOverAction> localMatchOverActions();
// The action at `index` of `actions` (a button's index, the menu's focus).
[[nodiscard]] std::optional<MatchOverAction> matchOverActionAt(
    std::span<const MatchOverAction> actions, std::size_t index);

// The winner in their colour (none: white), the final scoreboard and a row
// of buttons; `waiting` is a line above them ("Waiting for Bonobo 42 to
// play again").
struct MatchOverView {
  std::string title;
  std::optional<uint8_t> colorIndex;  // palette index (winnerTitle()); none: white
  Scoreboard board;
  std::vector<MatchOverAction> actions;
  std::string waiting;
  [[nodiscard]] std::optional<MatchOverAction> actionAt(std::size_t index) const {
    return matchOverActionAt(actions, index);
  }
};
// The title and the scoreboard, no buttons.
[[nodiscard]] MatchOverView matchOverView(const HudView& view, const presentation::RenderHud& hud);
// Locally: Play again (a new seed) and Back to lobby.
[[nodiscard]] MatchOverView localMatchOverView(const HudView& view,
                                               const presentation::RenderHud& hud);
// In an online room: the host's Play again and Leave; everyone else Leave,
// waiting for `hostName` (empty: "the host") to play again.
[[nodiscard]] MatchOverView roomMatchOverView(const HudView& view,
                                              const presentation::RenderHud& hud, bool host,
                                              std::string_view hostName);

// --- the online room's waiting screen (v0.42) -------------------------------------

// The slots the room's bots take when the host starts: the server places
// settings.bots bots in the lowest free slots (apps/server ServerHost::
// placeRoomBots); while none are placed yet (Waiting), the lowest slots no
// human holds. Once the server placed them (room.bots non-zero) there is
// nothing left to preview.
[[nodiscard]] sim::PlayerMask roomBotPreview(const net::RoomStateMsg& room);

}  // namespace kfm::client
