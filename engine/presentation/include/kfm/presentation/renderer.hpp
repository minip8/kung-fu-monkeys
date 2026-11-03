// The sokol_gfx renderer (architecture §7): draws a RenderState with a fixed
// camera. It lives in presentation and links kfm::sokol only; it never
// depends on engine/platform, which owns the window and the GL context.
//
// Lifecycle: the app calls platform::gfxInit() (sg_setup), then
// Renderer::init(); every frame it calls Renderer::draw() inside the default
// pass (between platform::beginDefaultPass and platform::debugUiDraw);
// Renderer::shutdown() runs before platform::gfxShutdown().
//
// Backends (M2): GL only. The GLSL is hand-written once per program and gets
// "#version 330 core" (SOKOL_GLCORE) or "#version 300 es" plus precision
// qualifiers (SOKOL_GLES3, WebGL2) prefixed at compile time. Uniforms are
// vec4 arrays (std140-compatible by construction) set with sg_apply_uniforms.
//
// Draw order per frame: opaque lit meshes (floor, wall rim, players, aim
// indicators, held/flying bananas, pickups), then alpha-blended ground decals
// with depth test on and depth write off (blob shadows, dash trails, catch
// window rings, effect rings), then additive billboards (flashes, bursts).
// The per-frame path allocates nothing; every mesh is uploaded once in init()
// or loadCharacterModel().
//
// Characters: players are drawn as the capsule placeholder until
// loadCharacterModel() succeeds; from then on as the GPU-skinned model
// (program `skinned`), scaled so its bind-pose height equals the capsule's
// (RenderState::playerHeight) and animated by the animation layer
// (animation.hpp) with one AnimState per player slot kept here, advanced by
// RenderState::time each draw. setForceCapsules(true) switches back to the
// capsule at any time. Shadows, rings, the aim bar and the banana in hand do
// not depend on which body is drawn.
#pragma once

#include "kfm/presentation/animation.hpp"
#include "kfm/presentation/camera.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/presentation/skinned_model.hpp"
#include "kfm/sim/content.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

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

  // Loads a skinned glTF character (gltf_loader.hpp) and uploads it; requires
  // init(). On failure returns false, keeps drawing capsules (and any model
  // loaded before) and leaves the reason in characterModelError(). Allocates;
  // call it at startup, not per frame.
  [[nodiscard]] bool loadCharacterModel(const char* path);
  [[nodiscard]] bool characterModelLoaded() const { return skinnedLoaded_; }
  [[nodiscard]] const std::string& characterModelError() const { return modelError_; }
  // Draw players as capsules even when a model is loaded.
  void setForceCapsules(bool force) { forceCapsules_ = force; }
  [[nodiscard]] bool forceCapsules() const { return forceCapsules_; }

 private:
  // sokol handles are stored as their raw ids so this header does not need
  // <sokol_gfx.h> (kfm::sokol is a private dependency of kfm_renderer).
  struct GpuMesh {
    uint32_t vertexBuffer{0};
    uint32_t indexBuffer{0};
    uint32_t indexCount{0};
  };
  // capsule, banana, disc, floor, cube, skinned character (empty until loaded)
  static constexpr std::size_t kMeshCount = 6;
  static constexpr std::size_t kShaderCount = 3;    // lit, decal, skinned
  static constexpr std::size_t kPipelineCount = 4;  // lit, decal alpha, decal additive, skinned

  void destroyResources();
  void destroyCharacterModel();

  RendererStats stats_{};
  bool initialised_{false};
  std::array<GpuMesh, kMeshCount> meshes_{};
  std::array<uint32_t, kShaderCount> shaders_{};
  std::array<uint32_t, kPipelineCount> pipelines_{};

  // The skinned character: CPU copy (joints and clips drive the animation),
  // clip binding and per-slot animation state.
  SkinnedModel model_;
  ClipBinding clips_{};
  CharacterAnimStates anim_{};
  float lastAnimTime_{-1.0f};  // RenderState::time of the previous draw; < 0: none yet
  bool skinnedLoaded_{false};
  bool forceCapsules_{false};
  std::string modelError_;
};

}  // namespace kfm::presentation
