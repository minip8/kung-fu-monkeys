// The sokol_gfx renderer (architecture §7): draws a RenderState with a fixed
// camera. It lives in presentation and links kfm::sokol only; it never
// depends on engine/platform, which owns the window and the GL context.
//
// Lifecycle: the app calls platform::gfxInit() (sg_setup), then
// Renderer::init(); every frame it calls Renderer::draw() inside the default
// pass (between platform::beginDefaultPass and platform::debugUiDraw);
// Renderer::shutdown() runs before platform::gfxShutdown().
#pragma once

#include "kfm/presentation/camera.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/sim/content.hpp"

#include <cstdint>

namespace kfm::presentation {

struct RendererStats {
  uint32_t drawCalls{0};
  uint32_t triangles{0};
};

class Renderer {
 public:
  // Builds meshes from content and the pipelines. Requires sg_setup() done.
  [[nodiscard]] bool init(const sim::ContentDB& content);
  void shutdown();

  // Draws everything in `state` (players, bananas, pickups, shadows, effects,
  // arena) inside the current default pass. `state.shakeOffset` is applied to
  // the camera here.
  void draw(const RenderState& state, const Camera& camera, int width, int height);

  [[nodiscard]] const RendererStats& stats() const { return stats_; }

 private:
  RendererStats stats_{};
  bool initialised_{false};
};

}  // namespace kfm::presentation
