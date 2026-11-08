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

// The region with its availability, Create room, the room code field
// (`editCode` puts the keyboard into it) with Join, and Back. Commands go
// to menu.takeCommand(). `lobby`: the lobby's URL, shown small.
struct OnlineOutcome {
  bool regionChanged{false};
  bool back{false};
};
[[nodiscard]] OnlineOutcome drawOnlineScreen(OnlineMenu& menu, bool& editCode,
                                             const std::string& lobby);

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
