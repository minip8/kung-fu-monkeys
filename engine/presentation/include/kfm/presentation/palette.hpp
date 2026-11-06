// Player colours (architecture §7): one per slot, a material parameter rather
// than a texture variant. Linear-ish RGB in [0, 1]; RenderPlayer::colorIndex
// is the slot. Chosen for mutual distinctness at a glance on the grass floor:
//
//   0 red      1 blue     2 purple   3 green    4 orange
//   5 yellow   6 cyan     7 pink     8 white    9 charcoal
//
// Slots 0..3 (the usual local and small online matches) use the four most
// separable hues. v0.49: yellow moved from slot 2 to slot 5 (purple the other
// way) so that no monkey in a match of up to five wears banana yellow.
#pragma once

#include "kfm/sim/constants.hpp"

#include <array>
#include <cstdint>

namespace kfm::presentation {

struct Color {
  float r{0.0f};
  float g{0.0f};
  float b{0.0f};
};

inline constexpr std::array<Color, sim::kMaxPlayers> kPlayerColors = {{
    {0.90f, 0.18f, 0.18f},  // 0 red
    {0.18f, 0.42f, 0.95f},  // 1 blue
    {0.58f, 0.28f, 0.86f},  // 2 purple
    {0.16f, 0.72f, 0.28f},  // 3 green
    {1.00f, 0.52f, 0.08f},  // 4 orange
    {0.98f, 0.84f, 0.12f},  // 5 yellow
    {0.10f, 0.84f, 0.88f},  // 6 cyan
    {0.98f, 0.46f, 0.74f},  // 7 pink
    {0.94f, 0.94f, 0.94f},  // 8 white
    {0.22f, 0.22f, 0.25f},  // 9 charcoal
}};

// Colour for a slot; out-of-range slots wrap.
[[nodiscard]] constexpr Color playerColor(uint32_t slot) {
  return kPlayerColors[slot % kPlayerColors.size()];
}

// The cartoon look of an arena (v0.49, architecture §7 "Look"): the colours
// of the ground, the statics and the ink lines. Values are the style board's
// Jungle Toon swatches (sRGB hex / 255; the renderer writes them unconverted).
// The floor stays mid-value and muted so every player colour pops on it.
struct ArenaLook {
  Color background;  // the clear colour around the arena (canopy)
  Color floor;       // grass
  Color floorPatch;  // the grass's lighter blotches
  Color rim;         // the outer wall rim (logs)
  Color wall;        // interior walls (rock)
  Color wallTop;     // moss on their tops
  Color pitBank;     // the rim strip where a pit meets open floor
  Color water;       // a pit's surface (river)
  Color waterFoam;   // its ripples
  Color mud;         // slow patches
  Color mudBubble;   // their bubble rings
  Color shadow;      // blob shadows
  Color ink;         // outlines
  Color shadeTint;   // multiplies the shade band (a cool shadow)
};

inline constexpr ArenaLook kJungleLook{
    .background = {0.12f, 0.29f, 0.165f},  // #1f4a2a
    .floor = {0.45f, 0.64f, 0.31f},
    .floorPatch = {0.52f, 0.71f, 0.35f},
    .rim = {0.42f, 0.27f, 0.14f},      // #6b4423
    .wall = {0.64f, 0.63f, 0.56f},     // #a3a08f
    .wallTop = {0.37f, 0.63f, 0.26f},  // #5fa043
    .pitBank = {0.50f, 0.34f, 0.18f},
    .water = {0.16f, 0.56f, 0.66f},      // #2a8fa8
    .waterFoam = {0.75f, 0.92f, 0.95f},  // #bfeaf2
    .mud = {0.48f, 0.32f, 0.19f},        // #7a5230
    .mudBubble = {0.64f, 0.48f, 0.30f},  // #a37a4c
    .shadow = {0.05f, 0.12f, 0.06f},
    .ink = {0.17f, 0.11f, 0.055f},  // #2b1d0e
    .shadeTint = {0.82f, 0.86f, 1.0f},
};

}  // namespace kfm::presentation
