// The arena plane as the renderer's `ground` program draws it (v0.54,
// architecture §7 "Look"): the backdrop, the pits, the land with the cliff
// under its edges that face the camera, the slow patches and the wall
// shadows, in one pass over one quad, from signed distances. This is the
// headless half: which boxes, in sim coordinates, with which corners rounded
// (GroundLayout), and the distance fields sampled from them once per arena
// and look (GroundField), which the shader reads with a filtered fetch instead
// of looping over boxes per pixel (shaders.hpp kGroundFs).
//
// The land (the arena minus its pits) is cut into rectangles here, so its
// distance is exact outside it, and everything drawn from that distance (its
// ink line, the cliff under it, the shallows around it) follows its true
// outline. Water is whatever of the arena is not land.
//
// Rounding is cosmetic and never misstates a box by more than its corner
// radius: a box corner is rounded only when it is free, that is not inside
// another box of the same kind, so adjoining boxes of one kind (a tilemap's
// merged cells, the land around a notch) still read as one shape. The boxes
// of a group (boxes that touch) join with a smooth minimum, which rounds the
// concave corners between them by a small kGroundFillet, and separate groups
// with a plain one, so a narrow pit between two stretches of land never looks
// narrower than it is. Two boxes that share a stretch of
// edge also get a bridge, a box straddling that stretch joined with a plain
// minimum: without it the union's distance would be about zero along the
// seam, and the shader would draw an ink line across the middle of the land.
#pragma once

#include "kfm/presentation/math.hpp"
#include "kfm/presentation/palette.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/sim/content/arena.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kfm::presentation {

struct GroundBox {
  Vec2f center{};  // sim units
  Vec2f half{};
  // Corner radii in the order the shader reads them: (+x +y), (+x -y),
  // (-x +y), (-x -y).
  std::array<float, 4> radii{};
  uint8_t group{0};  // boxes of one group touch (directly or through others)
};

// Boxes the ground program takes: the land's (or, when the land does not
// fit, the pits') and the slow patches', each kind's bridges after it.
// Bridges past the cap are dropped (a seam may show).
inline constexpr std::size_t kMaxGroundBoxes = 40;

struct GroundLayout {
  Vec2f half{};  // the arena box
  // The land's boxes, then the slow patches', each kind's bridges last.
  std::array<GroundBox, kMaxGroundBoxes> boxes{};
  uint8_t landCount{0};  // with its bridges
  uint8_t landBridges{0};
  uint8_t slowCount{0};  // with its bridges
  uint8_t slowBridges{0};
  // The pits cut the land into more boxes than fit: the first boxes are the
  // pits instead, and the shader takes the land as the arena minus them.
  bool landFromPits{false};
  std::array<GroundBox, sim::kMaxArenaWalls> walls{};  // radii: wallCornerRadius()
  uint8_t wallCount{0};
};

// The rounding of a wall block's vertical edges, for its mesh and its shadow:
// the look's wallRadius, at most kWallRadiusShare of its shorter half side.
[[nodiscard]] float wallCornerRadius(const ArenaLook& look, Vec2f half);

// The layout of `state`'s arena pieces (RenderState::arenaPieces; pit rims are
// not part of it) under `look`.
[[nodiscard]] GroundLayout buildGroundLayout(const RenderState& state, const ArenaLook& look);

// The signed distances (sim units, negative inside) of a layout at p: the
// land (x), the slow patches (y, not cut to the land), the walls (z) and the
// walls moved kWallShadowOffset toward the camera (w, their shadow's shape).
// Boxes of a group join with a smooth minimum that rounds their concave
// corners by about kGroundFillet.
[[nodiscard]] Vec4 groundDistances(const GroundLayout& layout, const ArenaLook& look, Vec2f p);

// groundDistances() sampled on a grid over the arena and kGroundFieldMargin
// around it, kGroundFieldTexelsPerUnit a unit, clamped to
// ±kGroundFieldRange, as RGBA16F texels (half floats), the first row at the
// bottom (sim -y). Texel (i, j)'s centre lies at origin + ((i + 0.5) / width,
// (j + 0.5) / height) × size. Allocates: build it when the arena or the look
// changes, not per frame.
struct GroundField {
  int width{0};
  int height{0};
  Vec2f origin{};  // sim position of the texture's (0, 0) corner
  Vec2f size{};    // sim extent the texture covers
  std::vector<uint16_t> texels;
};
[[nodiscard]] GroundField buildGroundField(const GroundLayout& layout, const ArenaLook& look);

// IEEE 754 binary16 for `f` (round to nearest; overflow to infinity), for
// the field's texels.
[[nodiscard]] uint16_t toHalf(float f);
[[nodiscard]] float fromHalf(uint16_t h);

}  // namespace kfm::presentation
