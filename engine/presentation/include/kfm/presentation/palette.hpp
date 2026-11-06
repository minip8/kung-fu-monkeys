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

// How a surface is patterned. Walls (Rock, Planks, Lacquer), bananas and the
// toon base are the renderer's `lit` program (shaders.hpp kLitFs); floors,
// pits and slow patches are styles of the `ground` program (kGroundFs,
// v0.54), which draws the whole arena plane in one pass.
enum class SurfaceKind : uint8_t {
  Toon = 0,      // the base colour
  Grass = 1,     // floor: soft lighter patches and small tufts
  Water = 2,     // pits: a river with short ripple arcs
  Mud = 3,       // slow patches: bubble rings
  Rock = 4,      // walls: a boulder, lighter top, darker sides, a moss cap
  Banana = 5,    // bananas: brown tips
  Paving = 6,    // floor: large flagstones with grout lines and a few cracks
  Sand = 7,      // floor: a sun-bleached centre, the odd shell
  Chasm = 8,     // pits: a misty drop with drifting cloud arcs
  Planks = 9,    // walls: crates with strapped tops and a latch
  Lacquer = 10,  // walls: lacquered shrines, dark posts, a gold medallion, a jade roof
  Sea = 11,      // pits: the open sea (the backdrop's gradient) with wave squiggles (v0.54)
  Paddy = 12,    // slow patches: a rice paddy, shoots and a ripple (v0.54)
  TidePool = 13  // slow patches: shallow water in a sand rim (v0.54)
};

// What lies around the arena (v0.54): the backdrop the ground program draws
// beyond the arena's edge, never over the play area.
enum class BackdropKind : uint8_t {
  Canopy = 0,     // dark jungle green, big ink-outlined leaves at the corners
  Parchment = 1,  // a parchment sky with a mountain range beyond the far edge
  Sea = 2         // the open sea (a gradient, far to near) with wave squiggles and leaves
};

// The cartoon look of an arena (v0.49, architecture §7 "Look"): the colours
// of the ground, the statics and the ink lines, and how each is patterned.
// Values are the style board's swatches (sRGB hex / 255; the renderer writes
// them unconverted). v0.52: one look per arena theme (theme.hpp). v0.54: the
// style board's flat language: big simple shapes, ink outlines, sparse
// detail; the ground program draws floor, pits, slow patches, the cliff
// under every edge of land that faces the camera, the wall shadows and the
// backdrop from these.
struct ArenaLook {
  // Around the arena.
  Color background;   // the clear colour and the backdrop
  Color backdropAlt;  // parchment: the mountains; sea: the far end of the gradient
  Color decor;        // leaves (canopy, sea), clouds (parchment)
  Color decorAlt;     // the second leaf green
  // The ground.
  Color floor;       // sand: the edge of the gradient
  Color floorAlt;    // grass patches, grout lines, sand: the bleached centre
  Color floorMark;   // tufts, cracks, shells
  Color cliff;       // the face under an edge of land that faces the camera
  Color pit;         // a pit's surface (a sea pit: the preview's)
  Color pitAlt;      // ripples, cloud arcs, wave squiggles
  Color halo;        // shallow water around the land (haloWidth 0: none)
  Color mud;         // a slow patch's surface
  Color mudAlt;      // bubble rings, the paddy's ripple, the tide pool's rim
  Color mudDetail;   // rice shoots, the tide pool's ripple
  Color wallShadow;  // the soft shadow at a wall's foot (× wallShadowAlpha)
  // Walls (the lit program's Rock / Lacquer / Planks).
  Color wall;        // rock and crate tops, lacquer sides
  Color wallSide;    // rock and crate sides, lacquer posts
  Color wallAlt;     // moss, the jade roof, crate straps
  Color wallDetail;  // the gold medallion, the crate latch
  // Everything.
  Color shadow;     // blob shadows
  Color ink;        // outlines
  Color shadeTint;  // multiplies the shade band (a cool shadow)
  Color edge;       // the line along the arena's edge (× edgeAlpha)
  // The client's lobby preview (hud.cpp) only.
  Color rim;      // the preview's arena frame
  Color pitBank;  // the preview's pit border
  SurfaceKind floorSurface{SurfaceKind::Grass};
  SurfaceKind wallSurface{SurfaceKind::Rock};
  SurfaceKind pitSurface{SurfaceKind::Water};
  SurfaceKind slowSurface{SurfaceKind::Mud};
  BackdropKind backdrop{BackdropKind::Canopy};
  // Sizes, in arena units. floorScale: grass patch cells / flagstone size;
  // floorDetail: tuft / crack / shell cells; floorDensity: the share of
  // those cells that hold one.
  float floorScale{7.0f};
  float floorDetail{2.6f};
  float floorDensity{0.28f};
  float inkWidth{0.15f};     // ground outlines (0: none); walls: kOutlineWall
  float edgeAlpha{1.0f};     // the arena edge line
  float cornerRadius{1.2f};  // the arena's corners and every convex corner of land
  float pitRadius{0.6f};     // a pit's free corners (not against a pit or the edge)
  float slowRadius{0.9f};    // a slow patch's free corners (capped at half its side)
  float cliffDepth{0.65f};   // how far the cliff hangs below the edge, toward the camera
  float haloWidth{0.0f};     // the shallow water band around the land
  float pitAltAlpha{0.8f};   // ripple opacity
  float wallShadowAlpha{0.2f};
  float wallRadius{0.4f};  // a wall block's rounded corners (capped by kWallRadiusShare)
  float wallBevel{0.3f};   // its rounded top edge (at most wallRadius)
  bool wallInk{true};      // walls get an ink hull
};

// Jungle: a grass island in a river under the canopy (the style board's A).
inline constexpr ArenaLook kJungleLook{
    .background = {0.122f, 0.290f, 0.165f},   // #1f4a2a canopy
    .backdropAlt = {0.090f, 0.227f, 0.129f},  // unused
    .decor = {0.184f, 0.478f, 0.227f},        // #2f7a3a leaf
    .decorAlt = {0.247f, 0.541f, 0.247f},     // #3f8a3f leaf
    .floor = {0.549f, 0.776f, 0.353f},        // #8cc65a grass
    .floorAlt = {0.624f, 0.831f, 0.416f},     // #9fd46a patches
    .floorMark = {0.373f, 0.604f, 0.227f},    // #5f9a3a tufts
    .cliff = {0.420f, 0.267f, 0.137f},        // #6b4423 dirt
    .pit = {0.165f, 0.561f, 0.659f},          // #2a8fa8 river
    .pitAlt = {0.749f, 0.918f, 0.949f},       // #bfeaf2 ripples
    .halo = {0.165f, 0.561f, 0.659f},
    .mud = {0.478f, 0.322f, 0.188f},        // #7a5230
    .mudAlt = {0.639f, 0.478f, 0.298f},     // #a37a4c bubbles
    .mudDetail = {0.639f, 0.478f, 0.298f},  // unused
    .wallShadow = {0.0f, 0.0f, 0.0f},
    .wall = {0.639f, 0.627f, 0.561f},        // #a3a08f rock top
    .wallSide = {0.490f, 0.478f, 0.424f},    // #7d7a6c rock side
    .wallAlt = {0.373f, 0.627f, 0.263f},     // #5fa043 moss
    .wallDetail = {0.373f, 0.627f, 0.263f},  // unused
    .shadow = {0.05f, 0.12f, 0.06f},
    .ink = {0.169f, 0.114f, 0.055f},  // #2b1d0e
    .shadeTint = {0.82f, 0.86f, 1.0f},
    .edge = {0.169f, 0.114f, 0.055f},
    .rim = {0.420f, 0.267f, 0.137f},
    .pitBank = {0.420f, 0.267f, 0.137f},
    .floorSurface = SurfaceKind::Grass,
    .wallSurface = SurfaceKind::Rock,
    .pitSurface = SurfaceKind::Water,
    .slowSurface = SurfaceKind::Mud,
    .backdrop = BackdropKind::Canopy,
    .floorScale = 7.0f,
    .floorDetail = 2.6f,
    .floorDensity = 0.28f,
    .inkWidth = 0.15f,
    .edgeAlpha = 1.0f,
    .cornerRadius = 1.2f,
    .pitRadius = 0.6f,
    .slowRadius = 0.9f,
    .cliffDepth = 0.65f,
    .haloWidth = 0.0f,
    .pitAltAlpha = 0.8f,
    .wallShadowAlpha = 0.2f,
    .wallRadius = 0.4f,
    .wallBevel = 0.3f,
    .wallInk = true,
};

// Temple: a flagstone terrace over a misty chasm, a red lacquered shrine
// under a jade roof, rice paddies, a parchment sky (the style board's B, Ink
// Temple).
inline constexpr ArenaLook kTempleLook{
    .background = {0.914f, 0.851f, 0.706f},   // #e9d9b4 parchment
    .backdropAlt = {0.725f, 0.663f, 0.533f},  // #b9a988 mountains
    .decor = {0.953f, 0.902f, 0.784f},        // #f3e6c8 clouds
    .decorAlt = {0.953f, 0.902f, 0.784f},
    .floor = {0.788f, 0.706f, 0.561f},      // #c9b48f flagstones
    .floorAlt = {0.659f, 0.576f, 0.427f},   // #a8936d grout
    .floorMark = {0.110f, 0.090f, 0.071f},  // #1c1712 cracks
    .cliff = {0.541f, 0.463f, 0.337f},      // #8a7656
    .pit = {0.231f, 0.290f, 0.361f},        // #3b4a5c chasm
    .pitAlt = {0.914f, 0.933f, 0.949f},     // #e9eef2 clouds
    .halo = {0.231f, 0.290f, 0.361f},
    .mud = {0.545f, 0.541f, 0.306f},        // #8b8a4e paddy
    .mudAlt = {0.847f, 0.863f, 0.706f},     // #d8dcb4 ripple
    .mudDetail = {0.243f, 0.420f, 0.184f},  // #3e6b2f rice
    .wallShadow = {0.110f, 0.090f, 0.071f},
    .wall = {0.659f, 0.196f, 0.122f},        // #a8321f lacquer
    .wallSide = {0.110f, 0.090f, 0.071f},    // #1c1712 posts
    .wallAlt = {0.243f, 0.561f, 0.455f},     // #3e8f74 jade roof
    .wallDetail = {0.851f, 0.651f, 0.180f},  // #d9a62e medallion
    .shadow = {0.10f, 0.08f, 0.06f},
    .ink = {0.110f, 0.090f, 0.071f},  // #1c1712
    .shadeTint = {0.86f, 0.84f, 0.96f},
    .edge = {0.110f, 0.090f, 0.071f},
    .rim = {0.541f, 0.463f, 0.337f},
    .pitBank = {0.541f, 0.463f, 0.337f},
    .floorSurface = SurfaceKind::Paving,
    .wallSurface = SurfaceKind::Lacquer,
    .pitSurface = SurfaceKind::Chasm,
    .slowSurface = SurfaceKind::Paddy,
    .backdrop = BackdropKind::Parchment,
    .floorScale = 3.6f,
    .floorDetail = 3.2f,
    .floorDensity = 0.12f,
    .inkWidth = 0.2f,
    .edgeAlpha = 1.0f,
    .cornerRadius = 0.35f,
    .pitRadius = 0.2f,
    .slowRadius = 0.16f,
    .cliffDepth = 0.8f,
    .haloWidth = 0.0f,
    .pitAltAlpha = 0.75f,
    .wallShadowAlpha = 0.25f,
    .wallRadius = 0.04f,
    .wallBevel = 0.04f,
    .wallInk = true,
};

// Beach: a sand bar in the sea, crate stacks, tide pools (the style board's
// C, Banana Beach: soft shapes without ink on the ground).
inline constexpr ArenaLook kBeachLook{
    .background = {0.122f, 0.561f, 0.769f},   // #1f8fc4 the near sea
    .backdropAlt = {0.298f, 0.788f, 0.878f},  // #4cc9e0 the far sea
    .decor = {0.310f, 0.749f, 0.416f},        // #4fbf6a leaves
    .decorAlt = {0.310f, 0.749f, 0.416f},
    .floor = {0.910f, 0.773f, 0.518f},     // sand (the board's #f5d08a, a shade darker)
    .floorAlt = {0.969f, 0.863f, 0.624f},  // the bleached centre
    .floorMark = {1.0f, 0.541f, 0.420f},   // #ff8a6b shells
    .cliff = {0.851f, 0.678f, 0.400f},     // #d9ad66
    .pit = {0.20f, 0.67f, 0.84f},          // the sea (preview; drawn as the gradient)
    .pitAlt = {1.0f, 1.0f, 1.0f},          // wave squiggles
    .halo = {0.561f, 0.878f, 0.925f},      // #8fe0ec shallows
    .mud = {0.58f, 0.81f, 0.81f},          // #7fd2e2 over sand
    .mudAlt = {0.722f, 0.604f, 0.416f},    // #b89a6a rim
    .mudDetail = {1.0f, 1.0f, 1.0f},       // ripple
    .wallShadow = {0.0f, 0.0f, 0.0f},
    .wall = {0.788f, 0.482f, 0.271f},      // #c97b45 crate top
    .wallSide = {0.659f, 0.384f, 0.184f},  // #a8622f crate side
    .wallAlt = {0.659f, 0.384f, 0.184f},   // straps
    .wallDetail = {1.0f, 0.851f, 0.227f},  // #ffd93a latch
    .shadow = {0.28f, 0.20f, 0.10f},
    .ink = {0.23f, 0.15f, 0.08f},  // cocoa (monkeys and bananas)
    .shadeTint = {0.88f, 0.88f, 1.0f},
    .edge = {1.0f, 1.0f, 1.0f},
    .rim = {0.851f, 0.678f, 0.400f},
    .pitBank = {0.851f, 0.678f, 0.400f},
    .floorSurface = SurfaceKind::Sand,
    .wallSurface = SurfaceKind::Planks,
    .pitSurface = SurfaceKind::Sea,
    .slowSurface = SurfaceKind::TidePool,
    .backdrop = BackdropKind::Sea,
    .floorScale = 1.0f,
    .floorDetail = 3.0f,
    .floorDensity = 0.12f,
    .inkWidth = 0.0f,
    .edgeAlpha = 0.35f,
    .cornerRadius = 1.5f,
    .pitRadius = 0.8f,
    .slowRadius = 1.2f,
    .cliffDepth = 0.45f,
    .haloWidth = 0.55f,
    .pitAltAlpha = 0.6f,
    .wallShadowAlpha = 0.15f,
    .wallRadius = 0.2f,
    .wallBevel = 0.16f,
    .wallInk = false,
};

}  // namespace kfm::presentation
