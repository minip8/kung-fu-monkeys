// Arena themes (v0.52, architecture §7 "Look"): which ArenaLook an arena is
// drawn with. A theme is cosmetic and local: each client draws the arena with
// its own player's choice, so two players in one match may see different
// themes; walls, pits and mud stay where they are and keep what they mean.
// Nothing here reaches the sim, the content hash or the wire.
//
// The choice is Auto (each arena's suggested theme, suggestedTheme()) or one
// theme everywhere; the client saves it (`theme=` in the settings text) and
// takes --theme / ?theme=.
#pragma once

#include "kfm/presentation/palette.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace kfm::presentation {

enum class ArenaTheme : uint8_t { Jungle = 0, Temple, Beach };
inline constexpr std::size_t kArenaThemeCount = 3;

enum class ThemeChoice : uint8_t { Auto = 0, Jungle, Temple, Beach };
inline constexpr std::size_t kThemeChoiceCount = 4;

[[nodiscard]] const ArenaLook& arenaLook(ArenaTheme theme);

// "jungle", "temple", "beach"; choices add "auto".
[[nodiscard]] std::string_view themeName(ArenaTheme theme);
[[nodiscard]] std::string_view themeChoiceName(ThemeChoice choice);
[[nodiscard]] std::optional<ThemeChoice> parseThemeChoice(std::string_view name);

// The theme an arena suggests, by its content name; an arena this table does
// not know (a new TOML arena) is Jungle.
[[nodiscard]] ArenaTheme suggestedTheme(std::string_view arenaName);

// The theme to draw `arenaName` with under `choice`.
[[nodiscard]] ArenaTheme resolveTheme(ThemeChoice choice, std::string_view arenaName);

// The next or previous choice (direction > 0 or < 0), wrapping: menu rows.
[[nodiscard]] ThemeChoice cycleThemeChoice(ThemeChoice choice, int direction);

}  // namespace kfm::presentation
