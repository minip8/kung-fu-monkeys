// The client's look (architecture §7, v0.43, M6b step 3): one palette for
// the HUD, the menus and the ImGui theme, the game font and the widgets the
// screens share. Dear ImGui stays the drawing library.
//
// Palette (Mango Sunset, architecture §7 "Look"): every screen is a panel
// (bark brown, a 4 px ink border, 24 px corners, a 6 px solid ink drop
// shadow) with cream text and gold headings; buttons carry their own
// colour by role (Tone): the lead action mango, the second gold, the rest
// sand, each with a 4 px ink border, 16 px corners and a 5 px solid ink
// drop shadow. Focus (keyboard, pad or mouse) is the same everywhere: the
// focused button stands 2 px higher on a deeper shadow inside a cream ring,
// and a focused settings row sits on an inset bar inside the same ring. A
// disabled button is sunk flat onto its slab in bark-light with dim text.
// Large text wears an ink outline. Banana yellow only for bananas (ammo
// pips, the host's crown); the dark jungle green behind every screen. The
// player and team colours stay presentation::playerColor(). Text pairs are
// 4.5:1 or better (ink on mango 6.7, bark on gold 8.2, bark on sand 9.5,
// cream on bark 11.5, dim cream on bark 7.0, dim cream on bark-light 4.7).
//
// Font: Lilita One (assets/fonts/, SIL Open Font License 1.1), compiled into
// the binary (cmake/EmbedFile.cmake) and loaded once by applyUiStyle() as
// ImGui's default font. Dear ImGui 1.92+ bakes glyphs per size on demand,
// so one ImFont serves the sizes the screens use (kFontUi, kFontHeading,
// kFontTitle, ...). ImGui's own font stays loaded for the F1 debug overlay
// (debugFont()).
#pragma once

#include <imgui.h>

#include <cstdint>

namespace kfm::client::ui {

// --- palette ---------------------------------------------------------------------------
inline constexpr ImU32 kBanana = IM_COL32(250, 214, 40, 255);
inline constexpr ImU32 kGold = IM_COL32(255, 201, 60, 255);  // #ffc93c
inline constexpr ImU32 kGoldLight = IM_COL32(255, 224, 138, 255);
inline constexpr ImU32 kMango = IM_COL32(242, 140, 40, 255);  // #f28c28
inline constexpr ImU32 kMangoLight = IM_COL32(255, 170, 76, 255);
inline constexpr ImU32 kMangoDeep = IM_COL32(214, 108, 22, 255);
inline constexpr ImU32 kSand = IM_COL32(245, 220, 176, 255);  // #f5dcb0
inline constexpr ImU32 kSandLight = IM_COL32(255, 240, 210, 255);
inline constexpr ImU32 kCream = IM_COL32(255, 244, 214, 255);
inline constexpr ImU32 kCreamDim = IM_COL32(214, 190, 150, 255);
inline constexpr ImU32 kBark = IM_COL32(74, 44, 23, 255);        // #4a2c17, panels
inline constexpr ImU32 kBarkInset = IM_COL32(90, 53, 25, 255);   // rows and cards on a panel
inline constexpr ImU32 kBarkLight = IM_COL32(107, 68, 35, 255);  // #6b4423, frames
inline constexpr ImU32 kInk = IM_COL32(43, 29, 14, 255);         // #2b1d0e, borders
inline constexpr ImU32 kJungle = IM_COL32(20, 36, 24, 255);      // behind the screens
inline constexpr ImU32 kSky = IM_COL32(200, 220, 255, 230);      // status and info lines
inline constexpr ImU32 kAlarm = IM_COL32(255, 122, 94, 255);     // the last seconds, errors
inline constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
inline constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 200);
inline constexpr ImU32 kEmptyPip = IM_COL32(40, 40, 40, 200);

// Roles.
inline constexpr ImU32 kPanelBg = kBark;
inline constexpr ImU32 kOutline = kInk;  // borders, drop shadows, text outlines
inline constexpr ImU32 kFocusRing = kCream;
inline constexpr ImU32 kTitle = kGold;
inline constexpr ImU32 kScrim = IM_COL32(0, 0, 0, 150);  // over a paused match
// The arena preview's marks: objective zones, power-up spawners.
inline constexpr ImU32 kPreviewZone = IM_COL32(255, 255, 255, 50);
inline constexpr ImU32 kPreviewZoneRim = IM_COL32(255, 255, 255, 190);
inline constexpr ImU32 kPreviewZoneLetter = IM_COL32(255, 255, 255, 220);
inline constexpr ImU32 kPreviewSpawnerOn = IM_COL32(255, 255, 255, 230);
inline constexpr ImU32 kPreviewSpawnerOff = IM_COL32(170, 170, 170, 200);

// A button's colour by role: Lead is the screen's loudest action (Play
// online, Create room, Start match, Play again, Resume, Reconnect), Second
// the next one (Play local, Join, Copy link), Plain the rest (Settings,
// Back, Leave, Quit).
enum class Tone : uint8_t { Lead, Second, Plain };
struct ToneColors {
  ImU32 fill;
  ImU32 hover;
  ImU32 pressed;
  ImU32 text;
};
[[nodiscard]] constexpr ToneColors toneColors(Tone tone) {
  switch (tone) {
    case Tone::Lead:
      return {kMango, kMangoLight, kMangoDeep, kInk};
    case Tone::Second:
      return {kGold, kGoldLight, kMango, kBark};
    case Tone::Plain:
      break;
  }
  return {kSand, kSandLight, kGold, kBark};
}
inline constexpr ToneColors kToneOff{kBarkLight, kBarkLight, kBarkLight, kCreamDim};

// --- shapes (pixels) ---------------------------------------------------------------------
inline constexpr float kPanelBorder = 4.0f;
inline constexpr float kPanelRounding = 24.0f;
inline constexpr float kPanelDrop = 6.0f;
inline constexpr float kButtonBorder = 4.0f;
inline constexpr float kButtonRounding = 16.0f;
inline constexpr float kButtonDrop = 5.0f;
inline constexpr float kFocusLift = 2.0f;   // the focused button stands this much higher
inline constexpr float kFocusRingW = 3.0f;  // outside the ink border

[[nodiscard]] ImVec4 vec4(ImU32 c);
[[nodiscard]] ImU32 withAlpha(ImU32 c, int alpha);
// A slot's (or a team's) palette colour, presentation::playerColor().
[[nodiscard]] ImU32 slotColor(uint32_t slot, int alpha = 255);
[[nodiscard]] ImVec4 slotColorVec(uint32_t slot);

// --- font sizes (pixels) -----------------------------------------------------------------
inline constexpr float kFontUi = 20.0f;
inline constexpr float kFontSmall = 16.0f;
inline constexpr float kFontButton = 28.0f;
inline constexpr float kFontHeading = 36.0f;
inline constexpr float kFontTitle = 76.0f;

// The game font and the theme, once after platform::debugUiInit().
void applyUiStyle();
// ImGui's own font, for the debug overlay.
[[nodiscard]] ImFont* debugFont();

// --- shared widgets ----------------------------------------------------------------------
// Toon text at `pos` (top left) on `dl`: an ink outline and an ink drop
// under it, so it reads over a panel and over any arena alike.
void shadowText(ImDrawList* dl, float size, ImVec2 pos, ImU32 color, const char* text);
// The same centred on `anchor`.
void centredText(ImDrawList* dl, float size, ImVec2 anchor, ImU32 color, const char* text);
[[nodiscard]] float textWidth(float size, const char* text);

// The chunky card every panel, button and slot card is: a solid `outline`
// slab `drop` lower, the face in `fill` and a `border` ink line inside its
// edge. `alpha` scales all three (dimmed slot cards).
void card(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 fill, float rounding, float border,
          float drop, int alpha = 255);

// A screen: a centred panel without a title bar, `width` wide, its height
// fitted to the content, in the panel look. `centreY` is the panel centre
// as a fraction of the window height. Always call endScreen().
bool beginScreen(const char* id, float width, float centreY = 0.5f);
void endScreen();
// A heading line inside a screen.
void heading(const char* text, ImU32 color = kTitle, float size = kFontHeading);
// A large button in its tone's colour (Tone): focused (the keyboard / pad
// focus) it stands higher in the cream ring, hovered it lightens, pressed it
// sinks onto its slab, disabled it lies flat in bark-light. `width` > 0 is
// the width, < 0 the rest of the line, 0 fits the label. True when clicked
// while enabled.
bool bigButton(const char* label, bool focused, float width, bool enabled = true,
               float size = kFontButton, Tone tone = Tone::Plain);
// The height of a bigButton's face at `size` (without its slab).
[[nodiscard]] float bigButtonHeight(float size = kFontButton);
// The focused row of a menu: an inset bar in the cream focus ring.
void focusBar(ImVec2 min, ImVec2 max);
// The cream focus ring alone, round an item from `min` to `max` (a text
// field), drawn after it.
void focusRing(ImVec2 min, ImVec2 max, float rounding);
// A text field in the card look: cream with bark text inside its ink
// border. Wrap the InputText calls.
void pushFieldStyle();
void popFieldStyle();
// A small sand arrow key (the "<" / ">" of a settings value), square with
// the frame height for a side. True when clicked.
bool arrowButton(const char* id, ImGuiDir dir);

}  // namespace kfm::client::ui
