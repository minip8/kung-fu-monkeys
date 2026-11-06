// The fixed arena camera (architecture §7): one tilted perspective camera per
// arena, built once per viewport size. No rotation during gameplay.
//
// World basis (world_basis.hpp): sim (x, y) at height h is world (x, h, -y),
// right-handed, +y up. The camera sits on the world +z side of the arena (the
// sim -y side, "in front"), raised and pitched down toward the arena centre,
// so with an ordinary right-handed lookAt (up = +y) and perspective:
//   - sim +x projects to screen right,
//   - sim +y projects higher on screen (it is away from the camera),
//   - sim angles turn counter-clockwise on screen.
//
// Framing: the camera keeps the field of view and pitch from CameraParams and
// solves the distance so the whole ArenaDef box, plus a box of
// `framingHeight` above it (heads and hops at the far wall), fits the viewport
// with `margin` to spare on the limiting axis (width-limited for narrow
// viewports, height-limited for wide ones). Perspective makes the near edge
// project larger than the far edge, so the box is re-centred vertically with
// a lens shift (an off-axis projection) rather than by moving the target.
//
// Screen coordinates are pixels with the origin at the top-left of the
// viewport and y growing downward (SDL, ImGui).
#pragma once

#include "kfm/presentation/math.hpp"
#include "kfm/sim/content.hpp"

#include <optional>

namespace kfm::presentation {

struct CameraParams {
  float fovYDegrees{30.0f};   // long lens: little perspective distortion
  float pitchDegrees{55.0f};  // view direction below the horizon
  float margin{0.06f};        // fraction of the NDC half-extent kept free on the limiting axis
  float framingHeight{1.8f};  // world height that must also stay in frame (monkey + hop)
  ClipDepth clipDepth{ClipDepth::NegativeOneToOne};
};

struct Camera {
  CameraParams params{};
  float viewportWidth{1.0f};  // pixels
  float viewportHeight{1.0f};
  float aspect{1.0f};
  float fovY{0.0f};  // radians
  float distance{0.0f};
  float zNear{0.1f};
  float zFar{100.0f};
  Vec2f lensShift{};  // NDC offset applied after the projection
  Vec3 eye{};
  Vec3 target{};  // the arena centre on the ground
  Mat4 view{};
  Mat4 proj{};  // includes the lens shift
  Mat4 viewProj{};
  Mat4 invViewProj{};
};

// Camera framing a centred box arena with the given sim half extents.
[[nodiscard]] Camera makeArenaCamera(float halfExtentX, float halfExtentY, float viewportWidth,
                                     float viewportHeight, const CameraParams& params = {});
[[nodiscard]] Camera makeArenaCamera(const sim::ArenaDef& arena, float viewportWidth,
                                     float viewportHeight, const CameraParams& params = {});

struct ScreenPoint {
  float x{0.0f};        // pixels from the left edge
  float y{0.0f};        // pixels from the top edge
  float depth{0.0f};    // NDC depth
  bool inFront{false};  // false for points behind the camera (x, y meaningless)
};

[[nodiscard]] ScreenPoint worldToScreen(const Camera& camera, Vec3 world);

// Framebuffer pixels per world unit, across the view, at the camera's target
// (the arena centre).
[[nodiscard]] float pixelsPerUnitAtTarget(const Camera& camera);

// The camera offset to draw for a shake of `amplitude` world units whose
// offset this frame is `offset` (RenderState::shakeAmplitude/shakeOffset):
// `offset`, or none once the amplitude moves the target by less than
// kShakeMinPixels (tuning.hpp).
[[nodiscard]] Vec3 visibleShakeOffset(const Camera& camera, float amplitude, Vec3 offset);

// The world point on the horizontal plane at `groundHeight` under a screen
// pixel, or nothing when the ray through it misses the plane (at or above the
// horizon). For mouse aim: worldToSim() of the result is the sim position.
[[nodiscard]] std::optional<Vec3> screenToGround(const Camera& camera, float screenX, float screenY,
                                                 float groundHeight = 0.0f);

}  // namespace kfm::presentation
