// The front-end screens (architecture §7, v0.43, M6b step 3), drawn with
// Dear ImGui over the lobby's arena between debugUiNewFrame() and
// debugUiFlush(): the main menu, Settings and Play online. Their state is the
// headless models in core/menus.hpp and core/online_menu.hpp; the mouse
// changes the same models the keyboard and pads drive through main.cpp.
#pragma once

#include "core/how_to_play.hpp"
#include "core/lobby_settings.hpp"
#include "core/menus.hpp"
#include "core/online_menu.hpp"

#include <optional>
#include <string>

namespace kfm::client {

// The menu panel: the title, the items as large buttons in their tones (Play
// online the lead, Play local the second, the focused one in the cream ring)
// with the focused item's hint under them, and `corner` (the version line)
// small in the bottom right. Returns the item clicked.
[[nodiscard]] std::optional<MainMenuItem> drawMainMenu(MainMenu& menu, const std::string& corner);

// Name (a text field: `editNick` puts the keyboard into it), Volume, Sound,
// Bot difficulty and Back. `settings` and `nick` are edited in place.
struct SettingsOutcome {
  bool changed{false};      // volume, sound or bot difficulty
  bool nickChanged{false};  // a new valid name was committed
  bool back{false};
};
[[nodiscard]] SettingsOutcome drawSettingsScreen(SettingsMenu& menu, LobbySettings& settings,
                                                 std::string& nick, bool& editNick);

// v0.94: the player's name (a text field, edited in place: `editName` puts
// the keyboard into it), then the region with its availability and Create
// room, the room code field (`editCode`) with Join, and Back. Commands go to
// menu.takeCommand(). `lobby`: the lobby's URL, shown small.
struct OnlineFields {
  std::string& nick;
  bool nickChosen{false};
  bool& editName;
  bool& editCode;
  const std::string& lobby;
};
struct OnlineOutcome {
  bool regionChanged{false};
  bool nickChanged{false};    // a new valid name was committed
  bool nickCommitted{false};  // the field was committed (changed or not): the name is chosen
  bool back{false};
};
[[nodiscard]] OnlineOutcome drawOnlineScreen(OnlineMenu& menu, OnlineFields& fields);

// v0.94: the How to play card (core/how_to_play.hpp) inside the current
// window, with its Back button (`backFocused` draws it focused; a click sets
// `back`): the pause menu shows it in its own place.
void drawHowToPlay(const HowToPlay& card, bool backFocused, bool& back);
// The same as a screen of its own (the main menu's How to play). True when
// Back was clicked.
[[nodiscard]] bool drawHowToPlayScreen(const HowToPlay& card);

// M7 step 6: the replay viewer before it plays, on the jungle background
// (no arena is drawn): "Loading replay…" with `source` small under it while
// the bytes are on their way (the browser's fetch), or "Could not open the
// replay" with `error` and a Main menu button (`button` holds the keyboard /
// pad focus; Escape, Enter or a pad's South / East also leave). True when
// Main menu was clicked.
void drawReplayLoading(const std::string& source);
[[nodiscard]] bool drawReplayError(const std::string& error, const std::string& source,
                                   ButtonMenu& button);

}  // namespace kfm::client
