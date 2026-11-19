// The client's front-end screens as headless models (architecture §7,
// v0.43, M6b step 3): the main menu, the settings screen and a row of
// buttons (the match-over screens). SDL- and ImGui-free; hud.cpp draws them
// and main.cpp routes input to them and applies what they return.
//
// Navigation follows the lobby panel's conventions (lobby_menu.hpp):
// MenuInput turns arrows / WASD, a pad's d-pad and left stick into
// Up/Down/Left/Right; on these screens every pad navigates (nothing joins
// there), isMenuActivate() is Enter, keypad Enter, Space or any pad's South
// or Start, and isMenuBack() any pad's East or Back (the keyboard's Escape
// is the app shortcut). The mouse clicks the same items.
//
//   MainMenu      Play online, Play local, How to play (v0.94), Settings,
//                 Quit (native only); Play online is greyed and skipped
//                 without a way online (no lobby URL and no --connect
//                 server).
//   SettingsMenu  Name (the nickname, a text field), Volume, Sound, Arena
//                 theme, Mouse aim (on/off: the cursor aims for keyboard +
//                 mouse), Done: the player's preferences, never the match's
//                 (v0.97: Bot difficulty moved to the game-setup panel).
//                 v0.97: an overlay, opened over whatever screen asked for it
//                 (the main menu's Settings, the local lobby's and the
//                 waiting room's Settings button, Tab or a pad's Back/View
//                 there, the pause menu's Settings) and closed back onto it;
//                 a change applies at once and is saved.
//   ButtonMenu    a focus over N buttons (Play again / Back to lobby).
//   PauseMenu     v0.46: Escape (or a bound pad's Start) over a running
//                 match. v0.97: locally Resume, Settings, How to play,
//                 Restart match, Back to lobby, Main menu (the match is
//                 paused while it is open); online Resume, Settings, How to
//                 play, Leave room (the server's match runs on; the local
//                 slots send idle input). pauseItems() builds the list from a
//                 PauseContext. v0.94: How to play shows the controls card in
//                 the menu's place until Back (Esc, a pad's B, Enter).
#pragma once

#include "kfm/platform/input_event.hpp"
#include "lobby_menu.hpp"
#include "lobby_settings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace kfm::client {

class MatchSession;
class NetSession;

// Enter, keypad Enter or Space, or any pad's South or Start.
[[nodiscard]] bool isMenuActivate(const platform::InputEvent& event);
// Any pad's East or Back.
[[nodiscard]] bool isMenuBack(const platform::InputEvent& event);
// v0.97: the press that opens Settings over the local lobby or a waiting
// room: Tab, or the Back (View / Select) of a pad that navigates there
// (`deviceBound`, as MenuInput::translate()). Checked before isMenuBack().
[[nodiscard]] bool isSettingsEvent(const platform::InputEvent& event, bool deviceBound);

// --- the main menu --------------------------------------------------------------------

enum class MainMenuItem : uint8_t { PlayOnline = 0, PlayLocal, HowToPlay, Settings, Quit };

class MainMenu {
 public:
  // `quitItem`: natively; a browser tab is closed by the browser.
  explicit MainMenu(bool quitItem = true);

  // Whether Play online leads anywhere (a lobby URL or a --connect server).
  // A disabled item is drawn greyed and skipped by the focus.
  void setOnlineAvailable(bool available);
  [[nodiscard]] bool enabled(MainMenuItem item) const;

  [[nodiscard]] std::span<const MainMenuItem> items() const { return {items_.data(), count_}; }
  [[nodiscard]] std::size_t focus() const { return focus_; }
  [[nodiscard]] MainMenuItem focusedItem() const { return items_[focus_]; }
  // Focuses an enabled item (the mouse); others are ignored.
  void setFocus(std::size_t index);
  // Up/Down move the focus over the enabled items, wrapping; Left/Right do
  // nothing here.
  void apply(MenuAction action);
  // The focused item when enabled.
  [[nodiscard]] std::optional<MainMenuItem> activate() const;

  [[nodiscard]] static const char* label(MainMenuItem item);
  // One line under the focused item ("Make a room, or join friends with a code").
  [[nodiscard]] const char* hint(MainMenuItem item) const;

 private:
  std::array<MainMenuItem, 5> items_{};
  std::size_t count_{0};
  std::size_t focus_{0};
  bool online_{true};
};

// --- the settings overlay -------------------------------------------------------------

inline constexpr uint8_t kVolumeStep = 10;  // percent per Left/Right

enum class SettingsRow : uint8_t { Name = 0, Volume, Sound, Theme, MouseAim, Back };

class SettingsMenu {
 public:
  // v0.97: opens over the current screen, the focus on the first row.
  void open() {
    open_ = true;
    focus_ = 0;
  }
  void close() { open_ = false; }
  [[nodiscard]] bool isOpen() const { return open_; }

  [[nodiscard]] static std::span<const SettingsRow> rows();
  [[nodiscard]] std::size_t focus() const { return focus_; }
  [[nodiscard]] SettingsRow focusedRow() const { return rows()[focus_]; }
  void setFocus(std::size_t row);

  // Up/Down move the focus (wrapping); Left/Right change the focused value
  // in `settings` (volume by kVolumeStep within 0..100, sound and mouse aim
  // toggle, the theme cycles). True when a setting changed.
  bool apply(MenuAction action, LobbySettings& settings);
  static bool change(std::size_t row, int direction, LobbySettings& settings);

  // v0.97: what a press asks for while the overlay is open: Close (Escape,
  // a pad's East or Back), Activate (isMenuActivate()) or nothing.
  enum class Press : uint8_t { None = 0, Activate, Close };
  [[nodiscard]] static Press press(const platform::InputEvent& event);
  // Activate on the focused row: Name puts the keyboard into the field,
  // Done closes, any other row changes as Right does.
  enum class Activation : uint8_t { None = 0, EditName, Changed, Closed };
  Activation activate(LobbySettings& settings);

  // A typed name made valid (net::Nickname::from). An empty result keeps
  // the old name (a player always has one online). True when it changed.
  static bool setNick(std::string& nick, std::string_view text);

  [[nodiscard]] static const char* rowLabel(SettingsRow row);
  [[nodiscard]] static std::string rowValue(SettingsRow row, const LobbySettings& settings,
                                            const std::string& nick);

 private:
  bool open_{false};
  std::size_t focus_{0};
};

// --- a row of buttons -----------------------------------------------------------------

class ButtonMenu {
 public:
  // The number of buttons (the focus is clamped to them).
  void setCount(std::size_t count);
  [[nodiscard]] std::size_t count() const { return count_; }
  [[nodiscard]] std::size_t focus() const { return focus_; }
  void setFocus(std::size_t index);
  // Any direction moves the focus: Up/Left back, Down/Right on, wrapping.
  void apply(MenuAction action);

 private:
  std::size_t count_{1};
  std::size_t focus_{0};
};

// --- the pause menu (v0.46) ------------------------------------------------------------

// v0.97: Settings (the overlay over the menu) and, locally, Restart match (a
// fresh match with the same setup and players) join the items.
enum class PauseItem : uint8_t {
  Resume = 0,
  Settings,
  HowToPlay,
  Restart,
  BackToLobby,
  MainMenu,
  LeaveRoom,
};

// v0.97: where the pause menu opens. `host`: the online room's host (no
// item of its own yet; End match and Restart match for the host go here,
// in pauseItems()).
struct PauseContext {
  bool online{false};
  bool host{false};
};

inline constexpr std::size_t kMaxPauseItems = 8;
struct PauseItems {
  std::array<PauseItem, kMaxPauseItems> items{};
  std::size_t count{0};
  [[nodiscard]] std::span<const PauseItem> view() const { return {items.data(), count}; }
};
// The buttons for a context, top to bottom: Resume first, the way out last.
[[nodiscard]] PauseItems pauseItems(PauseContext context);

class PauseMenu {
 public:
  // Opens with the context's buttons (pauseItems()), the focus on Resume.
  void open(PauseContext context);
  void close() {
    open_ = false;
    help_ = false;
  }
  [[nodiscard]] bool isOpen() const { return open_; }
  // v0.94: the controls card in the menu's place (How to play).
  [[nodiscard]] bool helpOpen() const { return open_ && help_; }
  void setHelp(bool open) { help_ = open && open_; }
  [[nodiscard]] bool online() const { return context_.online; }
  [[nodiscard]] PauseContext context() const { return context_; }

  [[nodiscard]] std::span<const PauseItem> items() const { return items_.view(); }
  [[nodiscard]] std::size_t focus() const { return buttons_.focus(); }
  [[nodiscard]] PauseItem focusedItem() const { return items()[buttons_.focus()]; }
  [[nodiscard]] ButtonMenu& buttons() { return buttons_; }
  void setFocus(std::size_t index) { buttons_.setFocus(index); }
  // Any direction moves the focus, wrapping (ButtonMenu).
  void apply(MenuAction action) { buttons_.apply(action); }
  // What a press asks for while the menu is open: Escape or any pad's East,
  // Back or Start resume; Enter, keypad Enter, Space or a pad's South pick
  // the focused item; anything else nothing. v0.94: with the controls card
  // open, Escape, Enter, Space and a pad's East, Back, Start or South give
  // HowToPlay, which closes the card (back to the buttons).
  [[nodiscard]] std::optional<PauseItem> press(const platform::InputEvent& event) const;

  [[nodiscard]] static const char* label(PauseItem item);

 private:
  bool open_{false};
  bool help_{false};
  PauseContext context_{};
  PauseItems items_{};
  ButtonMenu buttons_;
};

// Whether `event` opens the pause menu over a running match: Escape, or the
// Start of a pad bound to a slot (`deviceBound`, eventDeviceBound(); an
// unbound pad's press is a takeover).
[[nodiscard]] bool isPauseEvent(const platform::InputEvent& event, bool deviceBound);

// The menu with its session. Locally, opening pauses the match and closing
// resumes it (MatchSession::setPaused: presses made meanwhile are discarded,
// a held button does not fire on resume). Online, the session keeps
// advancing; opening suspends its input (NetSession::setInputSuspended:
// every local slot sends an idle input) and closing restores it.
void openPauseMenu(PauseMenu& menu, MatchSession& session);
void closePauseMenu(PauseMenu& menu, MatchSession& session);
void openPauseMenu(PauseMenu& menu, NetSession& session);
void closePauseMenu(PauseMenu& menu, NetSession& session);

}  // namespace kfm::client
