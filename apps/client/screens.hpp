// The front-end screens (architecture §7, v0.43, M6b step 3), drawn with
// Dear ImGui over the lobby's arena between debugUiNewFrame() and
// debugUiFlush(): the main menu, Settings and Play online. Their state is the
// headless models in core/menus.hpp and core/online_menu.hpp; the mouse
// changes the same models the keyboard and pads drive through main.cpp.
#pragma once

#include "core/lobby_settings.hpp"
#include "core/menus.hpp"
#include "core/online_menu.hpp"

#include <optional>
#include <string>

namespace kfm::client {

// The title, the menu's items as large buttons (the focused one in banana)
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

// The region with its availability, Create room, the room code field
// (`editCode` puts the keyboard into it) with Join, and Back. Commands go
// to menu.takeCommand(). `lobby`: the lobby's URL, shown small.
struct OnlineOutcome {
  bool regionChanged{false};
  bool back{false};
};
[[nodiscard]] OnlineOutcome drawOnlineScreen(OnlineMenu& menu, bool& editCode,
                                             const std::string& lobby);

}  // namespace kfm::client
