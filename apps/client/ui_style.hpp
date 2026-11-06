// The client's look (architecture §7, v0.43, M6b step 3): one palette for
// the HUD, the menus and the ImGui theme, the game font and the widgets the
// screens share. Dear ImGui stays the drawing library.
//
// Palette (v0.51, Mango Sunset, architecture §7 "Look"): bark-brown panels
// with dark ink borders and cream text; buttons sand at rest and mango when
// focused or hovered, each with a chunky ink drop shadow; gold for titles,
// headings, the room code and status lines; banana yellow only for bananas
// (ammo pips, the host's crown); the dark jungle green behind every screen.
// The player and team colours stay presentation::playerColor().
//
// Font: Lilita One (assets/fonts/, SIL Open Font License 1.1), compiled into
// the binary (cmake/EmbedFile.cmake) and loaded once by applyUiStyle() as
// ImGui's default font. Dear ImGui 1.92+ bakes glyphs per size on demand,
// so one ImFont serves the three sizes the screens use (kFontUi,
// kFontHeading, kFontTitle). ImGui's own font stays loaded for the F1
// debug overlay (debugFont()).
#pragma once

#include <imgui.h>

#include <cstdint>

namespace kfm::client::ui {

// --- palette ---------------------------------------------------------------------------
inline constexpr ImU32 kBanana = IM_COL32(250, 214, 40, 255);
inline constexpr ImU32 kGold = IM_COL32(255, 201, 60, 255);   // #ffc93c
inline constexpr ImU32 kMango = IM_COL32(242, 140, 40, 255);  // #f28c28
inline constexpr ImU32 kMangoLight = IM_COL32(255, 170, 76, 255);
inline constexpr ImU32 kMangoDeep = IM_COL32(214, 108, 22, 255);
inline constexpr ImU32 kSand = IM_COL32(245, 220, 176, 255);  // #f5dcb0
inline constexpr ImU32 kCream = IM_COL32(255, 244, 214, 255);
inline constexpr ImU32 kCreamDim = IM_COL32(214, 190, 150, 255);
inline constexpr ImU32 kBark = IM_COL32(74, 44, 23, 255);        // #4a2c17, panels
inline constexpr ImU32 kBarkLight = IM_COL32(107, 68, 35, 255);  // #6b4423, frames
inline constexpr ImU32 kInk = IM_COL32(43, 29, 14, 255);         // #2b1d0e, borders
inline constexpr ImU32 kJungle = IM_COL32(20, 36, 24, 255);      // behind the screens
inline constexpr ImU32 kSky = IM_COL32(200, 220, 255, 230);      // status and info lines
inline constexpr ImU32 kAlarm = IM_COL32(255, 96, 70, 255);      // the last seconds, errors
inline constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
inline constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 200);
inline constexpr ImU32 kEmptyPip = IM_COL32(40, 40, 40, 200);

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
// Text with a drop shadow at `pos` (top left) on `dl`.
void shadowText(ImDrawList* dl, float size, ImVec2 pos, ImU32 color, const char* text);
// The same centred on `anchor`.
void centredText(ImDrawList* dl, float size, ImVec2 anchor, ImU32 color, const char* text);
[[nodiscard]] float textWidth(float size, const char* text);

// A screen: a centred panel without a title bar, `width` wide, its height
// fitted to the content. Always call endScreen().
bool beginScreen(const char* id, float width, float bgAlpha = 0.92f);
void endScreen();
// A heading line inside a screen.
void heading(const char* text, ImU32 color = kGold, float size = kFontHeading);
// A large button with an ink border and drop shadow: mango while focused
// (the keyboard / pad focus) or hovered, sand otherwise, greyed when
// disabled. True when clicked.
bool bigButton(const char* label, bool focused, float width, bool enabled = true,
               float size = kFontButton);
// A row marker: the focused row of a menu, as a rounded mango bar behind it.
void focusBar(ImVec2 min, ImVec2 max);

}  // namespace kfm::client::ui
