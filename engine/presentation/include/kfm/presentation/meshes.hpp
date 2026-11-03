// Procedural placeholder meshes for M2 (architecture §7, §11): CPU-side
// vertex and 16-bit index arrays, generated once at init (they allocate) and
// uploaded by the renderer. Deterministic for given parameters; normals are
// unit length; triangles are counter-clockwise seen from the side their
// normals point to (GL default front face), with no degenerate triangles.
//
// Model space follows the world basis (world_basis.hpp): +y up, +x forward
// (rotateY(yaw) turns +x onto a heading). Meshes that stand on the ground have
// their base at y = 0.
#pragma once

#include "kfm/sim/content.hpp"

#include <cstdint>
#include <vector>

namespace kfm::presentation {

// 32 bytes, tightly packed: position, normal, uv.
struct MeshVertex {
  float px{0.0f};
  float py{0.0f};
  float pz{0.0f};
  float nx{0.0f};
  float ny{0.0f};
  float nz{0.0f};
  float u{0.0f};
  float v{0.0f};
};
static_assert(sizeof(MeshVertex) == 32);

struct MeshData {
  std::vector<MeshVertex> vertices;
  std::vector<uint16_t> indices;  // triangle list
};

// Capsule standing on y = 0 (axis +y): hemispheres of `radius` joined by a
// cylinder, total height `height` (> 2 × radius). `segments` around (>= 3),
// `hemisphereRings` latitude rings per hemisphere (>= 1). uv: u around, v up.
[[nodiscard]] MeshData makeCapsule(float radius, float height, uint32_t segments = 16,
                                   uint32_t hemisphereRings = 4);

// The monkey placeholder: a capsule with PlayerDef::radius and height
// radius × kMonkeyHeightOverRadius.
[[nodiscard]] MeshData makeMonkeyCapsule(const sim::PlayerDef& player);

// Banana: a tube bent along a quarter circle lying in the horizontal (xz)
// plane, tapering to points at both ends, centred on its bounding box at the
// origin with its middle bulging toward +x (forward). `length` is the chord
// between the tips, `thickness` the tube radius at the middle.
// `segmentsAlong` rings along the arc (>= 2), `segmentsAround` (>= 3).
[[nodiscard]] MeshData makeBanana(float length, float thickness, uint32_t segmentsAlong = 10,
                                  uint32_t segmentsAround = 8);

// The flying banana placeholder sized from BoomerangDef::radius (the tip-to-tip
// chord spans the hitbox diameter).
[[nodiscard]] MeshData makeBananaMesh(const sim::BoomerangDef& boomerang);

// Flat disc at y = 0 facing +y (blob shadow, pickup marker, dash ring base).
// uv: (0.5, 0.5) at the centre, the rim on the unit circle around it scaled by 0.5.
[[nodiscard]] MeshData makeDisc(float radius, uint32_t segments = 24);

// Floor quad at y = 0 facing +y over [-halfX, halfX] × [-halfZ, halfZ]. uv is
// the sim-plane position (x, -z) in units, so fract(uv) draws a one-unit grid
// aligned with sim coordinates.
[[nodiscard]] MeshData makeFloorQuad(float halfX, float halfZ);

// Axis-aligned unit cube centred at the origin ([-0.5, 0.5]^3), 24 vertices
// with flat face normals (arena wall rim segments are scaled instances).
[[nodiscard]] MeshData makeUnitCube();

}  // namespace kfm::presentation
