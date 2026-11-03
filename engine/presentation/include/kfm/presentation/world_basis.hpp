// The world basis: how the 2D sim plane sits in the 3D presentation world
// (architecture §7). Pinned; the renderer, HUD, input sampler and camera all
// rely on it.
//
//   sim (x, y), cosmetic height h  ->  world (x, h, -y)
//
// World space is right-handed with +y up. Sim +x is world +x (screen right);
// sim +y is world -z, which the fixed camera (camera.hpp) looks along, so sim
// +y is "away from the camera", i.e. up on screen. Seen from above (+y), the
// sim's counter-clockwise angles stay counter-clockwise, so a sim Angle theta
// is a world yaw of theta radians about +y: rotateY(theta) (math.hpp) turns a
// model's +x (its forward axis) onto the sim direction (cos theta, sin theta).
// Height is cosmetic and never read back into the sim.
#pragma once

#include "kfm/core/vec2.hpp"
#include "kfm/presentation/math.hpp"

namespace kfm::presentation {

// Sim plane position (float sim units) to world, at height h.
[[nodiscard]] constexpr Vec3 simToWorld(Vec2f sim, float h = 0.0f) {
  return {sim.x, h, -sim.y};
}
[[nodiscard]] constexpr Vec2f toFloat(Vec2 v) {
  return {v.x.toFloat(), v.y.toFloat()};
}
[[nodiscard]] constexpr Vec3 simToWorld(Vec2 sim, float h = 0.0f) {
  return simToWorld(toFloat(sim), h);
}
// World point to the sim plane (drops the height).
[[nodiscard]] constexpr Vec2f worldToSim(Vec3 world) {
  return {world.x, -world.z};
}

// A sim heading in radians is the world yaw about +y (see above).
[[nodiscard]] constexpr float simAngleToYaw(Angle a) {
  return angleToRadians(a);
}

}  // namespace kfm::presentation
