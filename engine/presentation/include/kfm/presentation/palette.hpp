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

// A banana's brown tips (v0.49): the stem and the far end of every banana.
inline constexpr Color kBananaTip{0.36f, 0.22f, 0.08f};

// How the renderer's `lit` program patterns a surface (shaders.hpp kLitFs).
enum class SurfaceKind : uint8_t {
  Toon = 0,     // the base colour
  Grass = 1,    // floor: two-tone blotches and tufts
  Water = 2,    // pits: deep patches and drifting crest lines
  Mud = 3,      // slow patches: blotches and bubble rings
  Rock = 4,     // walls: moss (the second colour) on top faces
  Banana = 5,   // bananas: brown tips
  Paving = 6,   // floor: offset flagstones with mortar lines (v0.52)
  Sand = 7,     // floor: wind ripples and shells (v0.52)
  Chasm = 8,    // pits: a misty drop (v0.52)
  Planks = 9,   // walls: wooden crates (v0.52)
  Lacquer = 10  // walls: lacquered posts under a tiled roof (v0.52)
};

// The cartoon look of an arena (v0.49, architecture §7 "Look"): the colours
// of the ground, the statics and the ink lines, and how each is patterned.
// Values are the style board's swatches (sRGB hex / 255; the renderer writes
// them unconverted). The floor stays mid-value and muted so every player
// colour pops on it. v0.52: one look per arena theme (theme.hpp).
struct ArenaLook {
  Color background;  // the clear colour around the arena
  Color floor;
  Color floorAlt;   // grass blotches, mortar, sand ripples
  Color rim;        // the outer wall rim
  Color wall;       // interior walls
  Color wallAlt;    // moss, roof tiles, plank seams
  Color pitBank;    // the rim strip where a pit meets open floor
  Color pit;        // a pit's surface
  Color pitAlt;     // foam, mist
  Color mud;        // slow patches
  Color mudAlt;     // bubble rings
  Color shadow;     // blob shadows
  Color ink;        // outlines
  Color shadeTint;  // multiplies the shade band (a cool shadow)
  SurfaceKind floorSurface{SurfaceKind::Grass};
  SurfaceKind wallSurface{SurfaceKind::Rock};
  SurfaceKind pitSurface{SurfaceKind::Water};
  SurfaceKind slowSurface{SurfaceKind::Mud};
  // The floor pattern's scale (grass blotches, flagstones or ripples per
  // unit), detail grid (tuft or shell cells per unit) and detail density.
  float floorScale{0.32f};
  float floorDetail{1.6f};
  float floorDensity{0.22f};
};

// Jungle: a grass clearing, a river, mossy rocks (the style board's A).
inline constexpr ArenaLook kJungleLook{
    .background = {0.078f, 0.141f, 0.094f},  // the client's dark jungle green
    .floor = {0.45f, 0.64f, 0.31f},
    .floorAlt = {0.52f, 0.71f, 0.35f},
    .rim = {0.42f, 0.27f, 0.14f},      // #6b4423
    .wall = {0.64f, 0.63f, 0.56f},     // #a3a08f
    .wallAlt = {0.37f, 0.63f, 0.26f},  // #5fa043
    .pitBank = {0.50f, 0.34f, 0.18f},
    .pit = {0.16f, 0.56f, 0.66f},     // #2a8fa8
    .pitAlt = {0.75f, 0.92f, 0.95f},  // #bfeaf2
    .mud = {0.48f, 0.32f, 0.19f},     // #7a5230
    .mudAlt = {0.64f, 0.48f, 0.30f},  // #a37a4c
    .shadow = {0.05f, 0.12f, 0.06f},
    .ink = {0.17f, 0.11f, 0.055f},  // #2b1d0e
    .shadeTint = {0.82f, 0.86f, 1.0f},
    .floorSurface = SurfaceKind::Grass,
    .wallSurface = SurfaceKind::Rock,
    .pitSurface = SurfaceKind::Water,
    .slowSurface = SurfaceKind::Mud,
    .floorScale = 0.32f,
    .floorDetail = 1.6f,
    .floorDensity = 0.22f,
};

// Temple: a flagstone courtyard on a cliff, red lacquered shrines under jade
// roofs, misty chasms, rice paddies (the style board's B, Ink Temple).
inline constexpr ArenaLook kTempleLook{
    .background = {0.12f, 0.13f, 0.19f},  // dusk over the mountains
    .floor = {0.69f, 0.61f, 0.47f},
    .floorAlt = {0.50f, 0.43f, 0.32f},  // mortar
    .rim = {0.60f, 0.19f, 0.12f},       // lacquer railing
    .wall = {0.76f, 0.25f, 0.17f},      // #c8402e
    .wallAlt = {0.24f, 0.56f, 0.45f},   // #3e8f74 jade roof
    .pitBank = {0.53f, 0.46f, 0.35f},
    .pit = {0.23f, 0.29f, 0.36f},     // #3b4a5c
    .pitAlt = {0.84f, 0.87f, 0.90f},  // mist
    .mud = {0.52f, 0.53f, 0.30f},     // paddy
    .mudAlt = {0.80f, 0.83f, 0.66f},
    .shadow = {0.10f, 0.08f, 0.06f},
    .ink = {0.11f, 0.09f, 0.07f},  // #1c1712
    .shadeTint = {0.86f, 0.84f, 0.96f},
    .floorSurface = SurfaceKind::Paving,
    .wallSurface = SurfaceKind::Lacquer,
    .pitSurface = SurfaceKind::Chasm,
    .slowSurface = SurfaceKind::Mud,
    .floorScale = 0.5f,
    .floorDetail = 0.0f,
    .floorDensity = 0.0f,
};

// Beach: a sand bar in the sea, crate stacks, tide pools (the style board's
// C, Banana Beach, drawn in this cartoon style with ink outlines).
inline constexpr ArenaLook kBeachLook{
    .background = {0.05f, 0.22f, 0.33f},  // deep sea
    .floor = {0.83f, 0.72f, 0.49f},
    .floorAlt = {0.73f, 0.61f, 0.39f},  // ripples
    .rim = {0.56f, 0.43f, 0.31f},       // driftwood
    .wall = {0.79f, 0.48f, 0.27f},      // #c97b45 crates
    .wallAlt = {0.58f, 0.33f, 0.16f},   // plank seams
    .pitBank = {0.70f, 0.60f, 0.42f},   // wet sand
    .pit = {0.12f, 0.60f, 0.76f},       // sea
    .pitAlt = {0.92f, 0.98f, 1.0f},     // foam
    .mud = {0.52f, 0.72f, 0.72f},       // tide pool: shallow water over sand
    .mudAlt = {0.86f, 0.96f, 0.96f},
    .shadow = {0.28f, 0.20f, 0.10f},
    .ink = {0.23f, 0.15f, 0.08f},  // cocoa
    .shadeTint = {0.88f, 0.88f, 1.0f},
    .floorSurface = SurfaceKind::Sand,
    .wallSurface = SurfaceKind::Planks,
    .pitSurface = SurfaceKind::Water,
    .slowSurface = SurfaceKind::Mud,
    .floorScale = 1.8f,
    .floorDetail = 1.2f,
    .floorDensity = 0.08f,
};

}  // namespace kfm::presentation
