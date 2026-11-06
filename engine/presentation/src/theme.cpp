#include "kfm/presentation/theme.hpp"

#include <utility>

namespace kfm::presentation {

namespace {

constexpr std::array<std::string_view, kThemeChoiceCount> kChoiceNames = {"auto", "jungle",
                                                                          "temple", "beach"};

// Each built-in arena's suggested theme (content/arenas.toml and
// content/arenas/*.toml): dojos and courtyards are temples, island and the
// capture outposts beaches, the rest jungle.
constexpr std::array<std::pair<std::string_view, ArenaTheme>, 9> kSuggested = {{
    {"dojo", ArenaTheme::Temple},
    {"pillars", ArenaTheme::Jungle},
    {"corridors", ArenaTheme::Temple},
    {"island", ArenaTheme::Beach},
    {"dojo_powerups", ArenaTheme::Temple},
    {"standoff", ArenaTheme::Jungle},
    {"hilltop", ArenaTheme::Jungle},
    {"outposts", ArenaTheme::Beach},
    {"courtyard", ArenaTheme::Temple},
}};

}  // namespace

const ArenaLook& arenaLook(ArenaTheme theme) {
  switch (theme) {
    case ArenaTheme::Jungle:
      return kJungleLook;
    case ArenaTheme::Temple:
      return kTempleLook;
    case ArenaTheme::Beach:
      return kBeachLook;
  }
  return kJungleLook;
}

std::string_view themeName(ArenaTheme theme) {
  return kChoiceNames[static_cast<std::size_t>(theme) + 1];
}

std::string_view themeChoiceName(ThemeChoice choice) {
  const auto i = static_cast<std::size_t>(choice);
  return i < kChoiceNames.size() ? kChoiceNames[i] : kChoiceNames[0];
}

std::optional<ThemeChoice> parseThemeChoice(std::string_view name) {
  for (std::size_t i = 0; i < kChoiceNames.size(); ++i) {
    if (kChoiceNames[i] == name) return static_cast<ThemeChoice>(i);
  }
  return std::nullopt;
}

ArenaTheme suggestedTheme(std::string_view arenaName) {
  for (const auto& [name, theme] : kSuggested) {
    if (name == arenaName) return theme;
  }
  return ArenaTheme::Jungle;
}

ArenaTheme resolveTheme(ThemeChoice choice, std::string_view arenaName) {
  if (choice == ThemeChoice::Auto) return suggestedTheme(arenaName);
  return static_cast<ArenaTheme>(static_cast<uint8_t>(choice) - 1);
}

ThemeChoice cycleThemeChoice(ThemeChoice choice, int direction) {
  constexpr auto kCount = static_cast<int>(kThemeChoiceCount);
  if (direction == 0) return choice;
  const int step = direction > 0 ? 1 : kCount - 1;
  return static_cast<ThemeChoice>((static_cast<int>(choice) + step) % kCount);
}

}  // namespace kfm::presentation
