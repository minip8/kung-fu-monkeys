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
// Draw order per frame: the ground (v0.54, program `ground`: backdrop, pits,
// land and its cliffs, slow patches, wall shadows and ink lines in one quad),
// then opaque lit meshes (wall blocks, players, aim
// indicators, held/flying bananas, pickups) with their ink outlines (v0.49:
// inverted hulls, program `outline`; a skinned body's right after it), then
// alpha-blended ground decals
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
#include "kfm/presentation/theme.hpp"
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
  // Builds meshes from content (the floor from the match's arena) and the
  // pipelines. Requires sg_setup() done. Call again when the arena or the
  // content changes.
  [[nodiscard]] bool init(const sim::MatchContent& match);
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

  // The arena theme (v0.52, theme.hpp): Auto draws each arena with its
  // suggested theme. Takes effect at once; init() resolves it for the new
  // arena. look() is what the next draw uses (the app clears the frame with
  // its background).
  void setThemeChoice(ThemeChoice choice);
  [[nodiscard]] ThemeChoice themeChoice() const { return themeChoice_; }
  [[nodiscard]] ArenaTheme theme() const { return theme_; }
  [[nodiscard]] const ArenaLook& look() const { return arenaLook(theme_); }

 private:
  // sokol handles are stored as their raw ids so this header does not need
  // <sokol_gfx.h> (kfm::sokol is a private dependency of kfm_renderer).
  struct GpuMesh {
    uint32_t vertexBuffer{0};
    uint32_t indexBuffer{0};
    uint32_t indexCount{0};
  };
  // capsule, banana, disc, ground, cube, quad, the arena's walls in each of
  // the three themes (v0.54), skinned character (empty until loaded)
  static constexpr std::size_t kMeshCount = 10;
  // lit, decal, outline, ground, skinned, skinned outline
  static constexpr std::size_t kShaderCount = 6;
  // lit, decal alpha, decal additive, outline, ground, skinned, skinned outline
  static constexpr std::size_t kPipelineCount = 7;

  void destroyResources();
  void destroyCharacterModel();

  RendererStats stats_{};
  bool initialised_{false};
  std::array<GpuMesh, kMeshCount> meshes_{};
  std::array<uint32_t, kShaderCount> shaders_{};
  std::array<uint32_t, kPipelineCount> pipelines_{};
  // v0.54: the indices of one wall block in each theme's wall mesh (every
  // block of a theme has as many; block i starts at i times this).
  std::array<uint32_t, kArenaThemeCount> wallIndexCount_{};
  // v0.54: the ground program's distance fields (ground.hpp GroundField),
  // one half-float texture per theme built in init() (the themes round
  // corners differently), and the sim rectangle they cover.
  std::array<uint32_t, kArenaThemeCount> groundImages_{};
  std::array<uint32_t, kArenaThemeCount> groundViews_{};
  uint32_t groundSampler_{0};
  Vec2f groundFieldOrigin_{};
  Vec2f groundFieldSize_{};

  // The skinned character: CPU copy (joints and clips drive the animation),
  // clip binding and per-slot animation state.
  SkinnedModel model_;
  ClipBinding clips_{};
  CharacterAnimStates anim_{};
  float lastAnimTime_{-1.0f};  // RenderState::time of the previous draw; < 0: none yet
  bool skinnedLoaded_{false};
  bool forceCapsules_{false};
  std::string modelError_;
  ThemeChoice themeChoice_{ThemeChoice::Auto};
  ArenaTheme theme_{ArenaTheme::Jungle};
  sim::ContentName arenaName_{};  // the arena of the last init(), for Auto
};

}  // namespace kfm::presentation
