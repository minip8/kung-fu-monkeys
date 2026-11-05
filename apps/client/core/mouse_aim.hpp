// Mouse aim's host side (architecture §5.4, v0.45). The sampler knows no
// world state and platform never sees the camera, so before every sampled
// tick the session turns the sampler's cursor into a ground point through
// the arena camera (presentation::screenToGround) and hands it each mouse
// slot's monkey position from the world it is about to step: MatchSession's
// current world locally, NetClient::world() (the predicted one) online, so
// the aim never lags the monkey. Float math: this is host code, never sim.
#pragma once

#include "kfm/platform/input_sampler.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/world_state.hpp"

namespace kfm::client {

// The drawable the cursor moves over: framebuffer pixels and the window's
// pixel density (framebuffer pixels per window coordinate). Width 0 = no
// window yet: the cursor has no ground point.
struct AimViewport {
  float width{0.0f};
  float height{0.0f};
  float pixelDensity{1.0f};
};

// Sets the sampler's cursor ground point (nothing without mouse aim, a
// viewport or a ray that meets the ground) and the aim origin of every slot
// that aims with the mouse (its monkey's position; nothing while it has no
// monkey).
void feedMouseAim(platform::InputSampler& sampler, const sim::WorldState& world,
                  const sim::ArenaDef& arena, const AimViewport& viewport);

}  // namespace kfm::client
