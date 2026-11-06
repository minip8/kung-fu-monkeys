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
//   MainMenu      Play online, Play local, Settings, Quit (native only);
//                 Play online is greyed and skipped without a way online
//                 (no lobby URL and no --connect server).
//   SettingsMenu  Name (the nickname, a text field), Volume, Sound, Bot
//                 difficulty, Back: what a player (not a developer) sets
//                 once; the lobby keeps the match's own settings.
//   ButtonMenu    a focus over N buttons (Play again / Back to lobby).
//   PauseMenu     v0.46: Escape (or a bound pad's Start) over a running
//                 match: Resume, Back to lobby, Main menu locally (the match
//                 is paused while it is open), Resume, Leave room online (the
//                 server's match runs on; the local slots send idle input).
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

// --- the main menu --------------------------------------------------------------------

enum class MainMenuItem : uint8_t { PlayOnline = 0, PlayLocal, Settings, Quit };

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
  std::array<MainMenuItem, 4> items_{};
  std::size_t count_{0};
  std::size_t focus_{0};
  bool online_{true};
};

// --- the settings screen --------------------------------------------------------------

enum class SettingsRow : uint8_t { Name = 0, Volume, Sound, BotDifficulty, Theme, Back };

class SettingsMenu {
 public:
  [[nodiscard]] static std::span<const SettingsRow> rows();
  [[nodiscard]] std::size_t focus() const { return focus_; }
  [[nodiscard]] SettingsRow focusedRow() const { return rows()[focus_]; }
  void setFocus(std::size_t row);

  // Up/Down move the focus (wrapping); Left/Right change the focused value
  // in `settings` (volume by kVolumeStep within 0..100, sound toggles, the
  // bot difficulty cycles). True when a setting changed.
  bool apply(MenuAction action, LobbySettings& settings);
  static bool change(std::size_t row, int direction, LobbySettings& settings);

  // A typed name made valid (net::Nickname::from). An empty result keeps
  // the old name (a player always has one online). True when it changed.
  static bool setNick(std::string& nick, std::string_view text);

  [[nodiscard]] static const char* rowLabel(SettingsRow row);
  [[nodiscard]] static std::string rowValue(SettingsRow row, const LobbySettings& settings,
                                            const std::string& nick);

 private:
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

enum class PauseItem : uint8_t { Resume = 0, BackToLobby, MainMenu, LeaveRoom };

class PauseMenu {
 public:
  // Opens with the local buttons (Resume, Back to lobby, Main menu) or the
  // online ones (Resume, Leave room), the focus on Resume.
  void open(bool online);
  void close() { open_ = false; }
  [[nodiscard]] bool isOpen() const { return open_; }
  [[nodiscard]] bool online() const { return online_; }

  [[nodiscard]] std::span<const PauseItem> items() const;
  [[nodiscard]] std::size_t focus() const { return buttons_.focus(); }
  [[nodiscard]] PauseItem focusedItem() const { return items()[buttons_.focus()]; }
  [[nodiscard]] ButtonMenu& buttons() { return buttons_; }
  void setFocus(std::size_t index) { buttons_.setFocus(index); }
  // Any direction moves the focus, wrapping (ButtonMenu).
  void apply(MenuAction action) { buttons_.apply(action); }
  // What a press asks for while the menu is open: Escape or any pad's East,
  // Back or Start resume; Enter, keypad Enter, Space or a pad's South pick
  // the focused item; anything else nothing.
  [[nodiscard]] std::optional<PauseItem> press(const platform::InputEvent& event) const;

  [[nodiscard]] static const char* label(PauseItem item);

 private:
  bool open_{false};
  bool online_{false};
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
