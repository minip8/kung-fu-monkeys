#include "mouse_aim.hpp"

#include "kfm/presentation/camera.hpp"
#include "kfm/presentation/world_basis.hpp"

#include <optional>

namespace kfm::client {

void feedMouseAim(platform::InputSampler& sampler, const sim::WorldState& world,
                  const sim::ArenaDef& arena, const AimViewport& viewport) {
  bool any = false;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    const PlayerId slot{i};
    if (!sampler.usesMouseAim(slot)) continue;
    any = true;
    const sim::Player* p = world.playerBySlot(slot);
    if (p == nullptr) {
      sampler.setAimOrigin(slot, std::nullopt);
      continue;
    }
    const presentation::Vec2f pos = presentation::toFloat(p->transform.pos);
    sampler.setAimOrigin(slot, platform::AimPoint{.x = pos.x, .y = pos.y});
  }
  const std::optional<platform::CursorPos> cursor = sampler.cursor();
  if (!any || !cursor || viewport.width <= 0.0f || viewport.height <= 0.0f) {
    sampler.setCursorGround(std::nullopt);
    return;
  }
  const presentation::Camera camera =
      presentation::makeArenaCamera(arena, viewport.width, viewport.height);
  const float density = viewport.pixelDensity > 0.0f ? viewport.pixelDensity : 1.0f;
  const std::optional<presentation::Vec3> ground =
      presentation::screenToGround(camera, cursor->x * density, cursor->y * density);
  if (!ground) {
    sampler.setCursorGround(std::nullopt);
    return;
  }
  const presentation::Vec2f g = presentation::worldToSim(*ground);
  sampler.setCursorGround(platform::AimPoint{.x = g.x, .y = g.y});
}

}  // namespace kfm::client
