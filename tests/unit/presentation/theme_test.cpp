// Arena themes (architecture v0.52, §7 "Look"): the names, the per-arena
// suggestions, the user's choice over them, and the looks' colours.
#include "kfm/presentation/theme.hpp"

#include "kfm/presentation/palette.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string_view>

using namespace kfm::presentation;

namespace {

constexpr std::array<ThemeChoice, kThemeChoiceCount> kChoices = {
    ThemeChoice::Auto, ThemeChoice::Jungle, ThemeChoice::Temple, ThemeChoice::Beach};

[[nodiscard]] bool inUnitRange(Color c) {
  return c.r >= 0.0f && c.r <= 1.0f && c.g >= 0.0f && c.g <= 1.0f && c.b >= 0.0f && c.b <= 1.0f;
}

[[nodiscard]] float luma(Color c) {
  return (0.2126f * c.r) + (0.7152f * c.g) + (0.0722f * c.b);
}

}  // namespace

TEST_CASE("theme choices round-trip through their names", "[presentation][theme]") {
  for (const ThemeChoice c : kChoices) {
    const auto parsed = parseThemeChoice(themeChoiceName(c));
    CHECK(parsed.value_or(ThemeChoice::Auto) == c);
    CHECK(parsed.has_value());
  }
  CHECK(themeChoiceName(ThemeChoice::Auto) == "auto");
  CHECK(themeName(ArenaTheme::Temple) == "temple");
  CHECK_FALSE(parseThemeChoice("Jungle").has_value());  // names are lower case
  CHECK_FALSE(parseThemeChoice("").has_value());
}

TEST_CASE("auto follows the arena; a fixed choice overrides it", "[presentation][theme]") {
  CHECK(suggestedTheme("dojo") == ArenaTheme::Temple);
  CHECK(suggestedTheme("island") == ArenaTheme::Beach);
  CHECK(suggestedTheme("pillars") == ArenaTheme::Jungle);
  CHECK(suggestedTheme("courtyard") == ArenaTheme::Temple);
  CHECK(suggestedTheme("a_new_toml_arena") == ArenaTheme::Jungle);  // the default
  CHECK(resolveTheme(ThemeChoice::Auto, "island") == ArenaTheme::Beach);
  CHECK(resolveTheme(ThemeChoice::Jungle, "island") == ArenaTheme::Jungle);
  CHECK(resolveTheme(ThemeChoice::Temple, "pillars") == ArenaTheme::Temple);
  CHECK(resolveTheme(ThemeChoice::Beach, "dojo") == ArenaTheme::Beach);
}

TEST_CASE("theme choices cycle and wrap both ways", "[presentation][theme]") {
  ThemeChoice c = ThemeChoice::Auto;
  for (std::size_t i = 0; i < kThemeChoiceCount; ++i)
    c = cycleThemeChoice(c, +1);
  CHECK(c == ThemeChoice::Auto);
  CHECK(cycleThemeChoice(ThemeChoice::Auto, -1) == ThemeChoice::Beach);
  CHECK(cycleThemeChoice(ThemeChoice::Beach, +1) == ThemeChoice::Auto);
  CHECK(cycleThemeChoice(ThemeChoice::Temple, 0) == ThemeChoice::Temple);
}

TEST_CASE("every look is in range and keeps the floor mid-value", "[presentation][theme]") {
  for (const ArenaTheme t : {ArenaTheme::Jungle, ArenaTheme::Temple, ArenaTheme::Beach}) {
    const ArenaLook& l = arenaLook(t);
    INFO(themeName(t));
    for (const Color c : {l.background, l.floor, l.floorAlt, l.rim, l.wall, l.wallAlt, l.pitBank,
                          l.pit, l.pitAlt, l.mud, l.mudAlt, l.shadow, l.ink, l.shadeTint}) {
      CHECK(inUnitRange(c));
    }
    // "Players pop first": the floor is neither dark nor glaring, and the ink
    // is darker than anything it outlines.
    CHECK(luma(l.floor) > 0.3f);
    CHECK(luma(l.floor) < 0.8f);
    CHECK(luma(l.ink) < 0.2f);
    CHECK(luma(l.ink) < luma(l.wall));
    CHECK(luma(l.ink) < luma(l.rim));
    // A pit reads as a drop: darker than the floor around it.
    CHECK(luma(l.pit) < luma(l.floor));
    // Floors patterned with their own scale.
    CHECK(l.floorScale > 0.0f);
  }
  CHECK(&arenaLook(ArenaTheme::Jungle) == &kJungleLook);
  CHECK(arenaLook(ArenaTheme::Temple).floorSurface == SurfaceKind::Paving);
  CHECK(arenaLook(ArenaTheme::Beach).wallSurface == SurfaceKind::Planks);
}
