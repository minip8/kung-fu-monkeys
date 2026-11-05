// The client's look (architecture §7, v0.43, M6b step 3): one palette for
// the HUD, the menus and the ImGui theme, the game font and the widgets the
// screens share. Dear ImGui stays the drawing library.
//
// Palette: banana yellow for titles, the room code and ammo; cream text on
// a dark jungle green, leaf green for buttons and focus, bark brown for
// borders; the player and team colours stay presentation::playerColor().
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
inline constexpr ImU32 kBananaDeep = IM_COL32(236, 164, 26, 255);
inline constexpr ImU32 kCream = IM_COL32(255, 244, 214, 255);
inline constexpr ImU32 kCreamDim = IM_COL32(196, 184, 150, 255);
inline constexpr ImU32 kJungle = IM_COL32(20, 36, 24, 255);
inline constexpr ImU32 kJungleLight = IM_COL32(38, 64, 42, 255);
inline constexpr ImU32 kLeaf = IM_COL32(74, 146, 62, 255);
inline constexpr ImU32 kLeafLight = IM_COL32(104, 184, 82, 255);
inline constexpr ImU32 kBark = IM_COL32(110, 74, 40, 255);
inline constexpr ImU32 kSky = IM_COL32(200, 220, 255, 230);  // status and info lines
inline constexpr ImU32 kAlarm = IM_COL32(255, 96, 70, 255);  // the last seconds, errors
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
void heading(const char* text, ImU32 color = kBanana, float size = kFontHeading);
// A large button: banana while focused (the keyboard / pad focus) or
// hovered, greyed when disabled. True when clicked.
bool bigButton(const char* label, bool focused, float width, bool enabled = true,
               float size = kFontButton);
// A row marker: the focused row of a menu, as a rounded banana bar behind it.
void focusBar(ImVec2 min, ImVec2 max);

}  // namespace kfm::client::ui
