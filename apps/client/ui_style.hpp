// The client's look (architecture §7, v0.43, M6b step 3): one palette for
// the HUD, the menus and the ImGui theme, the game fonts and the widgets the
// screens share. Dear ImGui stays the drawing library.
//
// Palette (v0.93, Banana Beach, architecture §7 "Look"; Mango Sunset before
// it): soft, toy-like and sunny, no ink outlines. Every screen is a white
// card (32 px corners, a soft cocoa shadow, no border) over a cream wash
// (kGround) laid on the arena behind it; text is cocoa, headings deep teal
// in Lilita One, body text Nunito. Buttons are pills with a solid darker lip
// under them instead of a border, coloured by role (Tone): the lead action
// banana with cocoa text, the second coral with cocoa text, the rest pale
// foam with deep-teal text. Focus (keyboard, pad or mouse, v0.55's rule in
// core/menu_highlight.hpp) is the same everywhere: the focused button
// stands 2 px higher inside a cocoa ring with a white gap, a focused
// settings row sits on a sand bar inside the same ring. A disabled button
// lies flat (no lip) in kDisabled with cocoa-soft text. Banana yellow is for
// bananas and the lead button (ammo pips, the host's crown); the player and
// team colours stay presentation::playerColor(). The main menu's block is
// an ocean card (kOcean) as on the style board.
//
// Text pairs (WCAG contrast, 4.5:1 or better for body text, 3:1 for large
// text, Lilita One at 24 px and up):
//   cocoa on white 13.8, on cream (kGround) 12.5, on banana 10.0, on banana
//   light 10.8, on coral 6.0, on coral light 7.1, on sand light 11.3, on foam
//   9.2; cocoa-soft on white 7.1, on cream 6.4, on sand light 5.8, on pale
//   foam 6.3, on kDisabled 5.5; teal on white 5.7, on cream 5.2, on pale
//   foam 5.1; deep teal on white 9.0, on pale foam 8.0; alarm on white 5.2,
//   on cream 4.7; white on deep teal 9.0, on alarm 5.2; white on ocean 3.6
//   (large text only: the main menu's title). The style board's white text
//   on coral (2.3) is not used: coral buttons take cocoa text.
// Over the playfield (bright sand and sea) HUD text sits on white pills
// (kHudPill) or, for the large banners, wears a cocoa outline (shadowText):
// readability beats purity.
//
// Fonts: Lilita One (titles, headings, buttons, banners, numbers) and
// Nunito Bold (body text, lists, settings rows) and ExtraBold (small HUD
// labels over the game), assets/fonts/ under the SIL Open Font License 1.1,
// compiled into the binary (cmake/EmbedFile.cmake) and loaded once by
// applyUiStyle(); Nunito Bold is ImGui's default font. Dear ImGui 1.92+
// bakes glyphs per size on demand, so one ImFont each serves every size.
// ImGui's own font stays loaded for the F1 debug overlay (debugFont()).
#pragma once

#include <imgui.h>

#include <cstdint>

namespace kfm::client {
class MenuHighlight;
}  // namespace kfm::client

namespace kfm::client::ui {

// --- palette (Banana Beach, the style board's tokens) ----------------------------------
inline constexpr ImU32 kGround = IM_COL32(255, 243, 214, 255);    // #fff3d6 cream
inline constexpr ImU32 kCocoa = IM_COL32(58, 42, 26, 255);        // #3a2a1a text
inline constexpr ImU32 kCocoaSoft = IM_COL32(107, 84, 64, 255);   // #6b5440 secondary text
inline constexpr ImU32 kTeal = IM_COL32(15, 111, 143, 255);       // #0f6f8f headings
inline constexpr ImU32 kTealDeep = IM_COL32(15, 79, 106, 255);    // #0f4f6a plain buttons
inline constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);     // cards
inline constexpr ImU32 kOcean = IM_COL32(31, 143, 196, 255);      // #1f8fc4
inline constexpr ImU32 kOceanFar = IM_COL32(76, 201, 224, 255);   // #4cc9e0
inline constexpr ImU32 kFoam = IM_COL32(143, 224, 236, 255);      // #8fe0ec shallows
inline constexpr ImU32 kFoamLip = IM_COL32(169, 217, 232, 255);   // #a9d9e8
inline constexpr ImU32 kFoamPale = IM_COL32(227, 245, 249, 255);  // #e3f5f9 plain buttons
inline constexpr ImU32 kBanana = IM_COL32(255, 217, 58, 255);     // #ffd93a
inline constexpr ImU32 kBananaLight = IM_COL32(255, 227, 110, 255);
inline constexpr ImU32 kBananaLip = IM_COL32(217, 164, 14, 255);  // #d9a40e
inline constexpr ImU32 kBananaInk = IM_COL32(201, 138, 16, 255);  // #c98a10 a banana's edge
inline constexpr ImU32 kCoral = IM_COL32(255, 138, 107, 255);     // #ff8a6b
inline constexpr ImU32 kCoralLight = IM_COL32(255, 162, 135, 255);
inline constexpr ImU32 kCoralLip = IM_COL32(216, 102, 74, 255);    // #d8664a
inline constexpr ImU32 kSand = IM_COL32(245, 208, 138, 255);       // #f5d08a
inline constexpr ImU32 kSandLight = IM_COL32(255, 231, 173, 255);  // #ffe7ad
inline constexpr ImU32 kSandShade = IM_COL32(217, 173, 102, 255);  // #d9ad66
inline constexpr ImU32 kPalm = IM_COL32(79, 191, 106, 255);        // #4fbf6a
inline constexpr ImU32 kAlarm = IM_COL32(196, 60, 44, 255);        // errors, the last seconds
inline constexpr ImU32 kDisabled = IM_COL32(239, 231, 214, 255);   // a disabled button's face
inline constexpr ImU32 kLine = IM_COL32(236, 222, 196, 255);       // separators on a card

// Roles.
inline constexpr ImU32 kPanelBg = kWhite;
inline constexpr ImU32 kText = kCocoa;
inline constexpr ImU32 kTextDim = kCocoaSoft;
inline constexpr ImU32 kTitle = kTeal;
inline constexpr ImU32 kFocusRing = kCocoa;      // outside a white gap
inline constexpr ImU32 kFocusFill = kSandLight;  // a focused settings row
inline constexpr ImU32 kInset = kGround;         // cards on a card: slots, the preview, rows
inline constexpr ImU32 kShadow = IM_COL32(58, 42, 26, 255);  // soft shadows (alpha by layer)
inline constexpr ImU32 kOutline = kCocoa;  // large text over the game, small marks
inline constexpr ImU32 kHudPill = IM_COL32(255, 255, 255, 230);  // HUD text over the game
inline constexpr ImU32 kEmptyPip = IM_COL32(58, 42, 26, 70);     // a spent ammo pip, empty pies
inline constexpr ImU32 kScrim = IM_COL32(58, 42, 26, 110);       // over a paused match
inline constexpr ImU32 kWash = IM_COL32(255, 243, 214, 200);     // over the arena behind menus
inline constexpr ImU32 kTransparent = IM_COL32(0, 0, 0, 0);      // a widget's own fill switched off
inline constexpr ImU32 kWarning = kBananaLip;  // the F1 overlay's notices (a tuned value)
inline constexpr ImU32 kError = kAlarm;        // ... and its failures (a bot module fell back)
inline constexpr ImU32 kSuccess = IM_COL32(31, 122, 61, 255);  // "Copied" (5.3 on white)
// The arena preview's marks: objective zones, power-up spawners.
inline constexpr ImU32 kPreviewZone = IM_COL32(255, 255, 255, 70);
inline constexpr ImU32 kPreviewZoneRim = IM_COL32(255, 255, 255, 210);
inline constexpr ImU32 kPreviewZoneLetter = kCocoa;
inline constexpr ImU32 kPreviewSpawnerOn = kWhite;
inline constexpr ImU32 kPreviewSpawnerOff = IM_COL32(255, 255, 255, 150);
// v0.82: a tree, a palm-green disc with a banana dot.
inline constexpr ImU32 kPreviewTree = kPalm;

// A button's colour by role: Lead is the screen's loudest action (Play
// online, Create room, Start match, Play again, Resume, Reconnect), Second
// the next one (Play local, Join, Copy link), Plain the rest (Settings,
// Back, Leave, Quit).
enum class Tone : uint8_t { Lead, Second, Plain };
struct ToneColors {
  ImU32 fill;
  ImU32 hover;
  ImU32 lip;  // the solid band under the pill (and its pressed face)
  ImU32 text;
};
[[nodiscard]] constexpr ToneColors toneColors(Tone tone) {
  switch (tone) {
    case Tone::Lead:
      return {kBanana, kBananaLight, kBananaLip, kCocoa};
    case Tone::Second:
      return {kCoral, kCoralLight, kCoralLip, kCocoa};
    case Tone::Plain:
      break;
  }
  return {kFoamPale, kWhite, kFoamLip, kTealDeep};
}
inline constexpr ToneColors kToneOff{kDisabled, kDisabled, kDisabled, kCocoaSoft};

// --- shapes (pixels) ---------------------------------------------------------------------
inline constexpr float kPanelRounding = 32.0f;
inline constexpr float kPanelShadowY = 12.0f;     // the soft shadow's offset ...
inline constexpr float kPanelShadowBlur = 30.0f;  // ... and its spread
inline constexpr int kPanelShadowAlpha = 38;      // 0.15 of the cocoa
inline constexpr float kPanelDrop = 12.0f;        // room kept under a panel for its shadow
inline constexpr float kButtonDrop = 6.0f;        // the lip under a pill
inline constexpr float kFocusLift = 2.0f;         // the focused button stands this much higher
inline constexpr float kFocusGap = 2.0f;          // white between an item and its ring
inline constexpr float kFocusRingW = 3.0f;
inline constexpr float kRowRounding = 14.0f;   // settings rows, toggles, fields
inline constexpr float kChipRounding = 12.0f;  // slot cards, HUD cards

[[nodiscard]] ImVec4 vec4(ImU32 c);
[[nodiscard]] ImU32 withAlpha(ImU32 c, int alpha);
// A slot's (or a team's) palette colour, presentation::playerColor().
[[nodiscard]] ImU32 slotColor(uint32_t slot, int alpha = 255);
[[nodiscard]] ImVec4 slotColorVec(uint32_t slot);

// --- fonts -------------------------------------------------------------------------------
inline constexpr float kFontUi = 19.0f;
inline constexpr float kFontSmall = 16.0f;
inline constexpr float kFontButton = 28.0f;
inline constexpr float kFontHeading = 36.0f;
inline constexpr float kFontTitle = 76.0f;

// Which typeface: Display is Lilita One (titles, buttons, banners, big
// numbers), Body Nunito Bold (ImGui's default), Strong Nunito ExtraBold
// (small labels over the game).
enum class Face : uint8_t { Display, Body, Strong };
[[nodiscard]] ImFont* font(Face face);

// The game fonts and the theme, once after platform::debugUiInit().
void applyUiStyle();
// ImGui's own font, for the debug overlay.
[[nodiscard]] ImFont* debugFont();

// --- shared widgets ----------------------------------------------------------------------
// Sticker text at `pos` (top left) on `dl`: a cocoa outline and a soft
// cocoa drop under it, so it reads over a card and over any arena alike.
void shadowText(ImDrawList* dl, float size, ImVec2 pos, ImU32 color, const char* text,
                Face face = Face::Display);
// The same centred on `anchor`.
void centredText(ImDrawList* dl, float size, ImVec2 anchor, ImU32 color, const char* text,
                 Face face = Face::Display);
// Text on a white pill centred on `anchor` (HUD lines over the game).
void pillText(ImDrawList* dl, float size, ImVec2 anchor, ImU32 color, const char* text,
              Face face = Face::Strong);
[[nodiscard]] float textWidth(float size, const char* text, Face face = Face::Display);

// A soft shadow under a rounded rectangle (the style board's 0 12px 30px
// cocoa at 0.15, as layered rectangles), scaled by `alpha` / 255.
void softShadow(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float offsetY, float blur,
                int alpha = kPanelShadowAlpha);
// A card: its soft shadow (`shadow`, scaled down for small cards) and the
// face in `fill`. `alpha` scales both (dimmed slot cards lie flat).
void card(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 fill, float rounding, float shadow = 1.0f,
          int alpha = 255);

// A screen: a centred white card without a title bar, `width` wide, its
// height fitted to the content. `centreY` is the card's centre as a fraction
// of the window height. `fill` is the card (the main menu's is the ocean).
// Always call endScreen().
bool beginScreen(const char* id, float width, float centreY = 0.5f, ImU32 fill = kPanelBg);
void endScreen();
// The cream wash over the arena behind the menu screens.
void menuBackdrop();
// v0.97: an overlay, a card over whatever screen is open (the power-up
// picker, Settings): a modal window, so the screen behind it is dimmed
// (kScrim) and takes no pointer, and it stays above it. Drawn at the top
// level (outside every other window), each frame its model is open; when it
// returns true draw the content and call endOverlay(). A frame that no
// longer draws an overlay closes it in finishOverlays(), which main.cpp
// calls once a frame before debugUiFlush().
bool beginOverlay(const char* id, float width);
void endOverlay();
void finishOverlays();
// A heading line inside a screen: teal Lilita One.
void heading(const char* text, ImU32 color = kTitle, float size = kFontHeading);
// Wrapped body text in a colour (TextWrapped with a pushed colour).
void bodyText(const char* text, ImU32 color = kText, float size = kFontUi);
// v0.55: which item the menus highlight (core/menu_highlight.hpp): main.cpp
// points this at its MenuHighlight; without one the pointer rule applies.
// The widgets below and the screens read it, never change it.
void setMenuHighlight(const MenuHighlight* highlight);
// The pointer rule is on (the mouse was the last menu device): the hovered
// item is the highlighted one and takes its model's focus.
[[nodiscard]] bool pointerMode();
// Whether the pointer is over the last item, or over a rectangle of the
// current window (a touch counts only while the finger is down).
[[nodiscard]] bool pointerOverItem();
[[nodiscard]] bool pointerOverRect(ImVec2 min, ImVec2 max);
// Pointer mode and the pointer over the last item: it takes its model's
// focus, so a key press then starts there (a resting pointer in keyboard /
// pad mode does not).
[[nodiscard]] bool pointerTakesFocus();
// The same for an item whose hover the caller measured (pointerOverRect).
[[nodiscard]] bool pointerTakesFocus(bool pointerOver);
// Whether the last frame drawn had an item take the focus from the pointer:
// the focused item is the one under the pointer (MenuHighlight::reveal's
// `pointerOnFocus`; read between frames, from the event handler).
[[nodiscard]] bool pointerOnFocus();
// Whether an item is drawn highlighted: `focused` in keyboard / pad mode,
// `pointerOver` in pointer mode (MenuHighlight::shows).
[[nodiscard]] bool showsFocus(bool focused, bool pointerOver);
// A large pill button in its tone's colour (Tone): highlighted (showsFocus:
// the keyboard / pad focus, or under the pointer) it stands higher in the
// focus ring, under the pointer it lightens too, pressed it sinks onto its
// lip, disabled it lies flat in kDisabled. `width` > 0 is the width, < 0
// the rest of the line, 0 fits the label. True when clicked while enabled.
bool bigButton(const char* label, bool focused, float width, bool enabled = true,
               float size = kFontButton, Tone tone = Tone::Plain);
// The height of a bigButton's face at `size` (without its lip).
[[nodiscard]] float bigButtonHeight(float size = kFontButton);
// The width bigButton gives `label` at `size` when `width` is 0.
[[nodiscard]] float bigButtonWidth(const char* label, float size = kFontButton);
// The focused row of a menu: a sand bar in the focus ring.
void focusBar(ImVec2 min, ImVec2 max);
// The focus ring alone, round an item from `min` to `max` (a text field),
// drawn after it.
void focusRing(ImVec2 min, ImVec2 max, float rounding);
// A text field in the card look: cream with cocoa text and a sand edge.
// Wrap the InputText calls.
void pushFieldStyle();
void popFieldStyle();
// A small round foam arrow key (the "<" / ">" of a settings value), the
// frame height for a side. `enabled` false draws it flat and inert. True
// when clicked.
bool arrowButton(const char* id, ImGuiDir dir, bool enabled = true);

}  // namespace kfm::client::ui
