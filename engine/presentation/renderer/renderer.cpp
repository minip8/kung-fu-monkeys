// The M2 sokol_gfx renderer (architecture §7): placeholder meshes, one
// directional light with a three-band toon ramp, blob-shadow decals and
// disc-based effects, and GPU-skinned characters when a model is loaded. See
// renderer.hpp for the draw order and shaders.hpp for the GLSL and uniform
// layout.
#include "kfm/presentation/renderer.hpp"

#include "kfm/presentation/animation.hpp"
#include "kfm/presentation/gltf_loader.hpp"
#include "kfm/presentation/meshes.hpp"
#include "kfm/presentation/palette.hpp"
#include "kfm/presentation/tuning.hpp"
#include "shaders.hpp"

#include <sokol_gfx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace kfm::presentation {

namespace {

// Mesh slots in Renderer::meshes_.
constexpr std::size_t kCapsuleMesh = 0;
constexpr std::size_t kBananaMesh = 1;
constexpr std::size_t kDiscMesh = 2;
constexpr std::size_t kFloorMesh = 3;
constexpr std::size_t kCubeMesh = 4;
constexpr std::size_t kQuadMesh = 5;  // v0.33: a unit floor quad, uv centred on 0 (zones)
constexpr std::size_t kSkinnedMesh = 6;
constexpr std::size_t kStaticMeshCount = 6;  // built in init(); the skinned one is loaded later

// Shader and pipeline slots.
constexpr std::size_t kLitShader = 0;
constexpr std::size_t kDecalShader = 1;
constexpr std::size_t kSkinnedShader = 2;
constexpr std::size_t kStaticShaderCount = 2;
constexpr std::size_t kLitPipeline = 0;
constexpr std::size_t kDecalAlphaPipeline = 1;
constexpr std::size_t kDecalAdditivePipeline = 2;
constexpr std::size_t kSkinnedPipeline = 3;
constexpr std::size_t kStaticPipelineCount = 3;

// --- look ----------------------------------------------------------------------
// Direction toward the light: from above, a little left and toward the camera,
// so capsule fronts are lit and their right sides fall into the half band.
constexpr Vec3 kToLight{-0.42f, 1.0f, 0.55f};
constexpr float kShadeBand = 0.6f;  // brightness of the darkest toon band

constexpr Color kFloorColor{0.52f, 0.56f, 0.50f};
constexpr Color kWallColor{0.36f, 0.30f, 0.24f};
constexpr Color kBananaColor{1.0f, 0.86f, 0.16f};
constexpr Color kPickupColor{0.98f, 0.80f, 0.12f};
constexpr Color kEliminatedColor{0.42f, 0.42f, 0.44f};
constexpr Color kShadowColor{0.04f, 0.05f, 0.06f};
constexpr Color kGold{1.0f, 0.82f, 0.18f};
constexpr Color kWhite{1.0f, 1.0f, 1.0f};
constexpr Color kHitRed{1.0f, 0.16f, 0.10f};
constexpr Color kShieldBlue{0.45f, 0.85f, 1.0f};
constexpr Color kSpark{1.0f, 0.62f, 0.25f};
constexpr Color kSmoke{0.72f, 0.72f, 0.74f};
constexpr Color kDust{0.62f, 0.52f, 0.38f};

constexpr float kWallHeight = 0.35f;
constexpr float kWallThickness = 0.3f;
constexpr float kZoneLift = 0.006f;      // v0.33: zones lie under the shadows
constexpr float kShadowLift = 0.012f;    // above the floor, plus a depth bias
constexpr float kDecalLift = 0.022f;     // rings and trails sit above the shadows
constexpr float kShadowOpacity = 0.55f;  // multiplied by RenderShadow::opacity
constexpr float kAimLength = 0.5f;       // aim indicator beyond the capsule surface
constexpr float kAimThickness = 0.1f;
constexpr float kAimHeightFraction = 0.55f;  // of the capsule height
constexpr float kHeldBananaScale = 0.75f;
constexpr float kOwnerTint = 0.3f;            // flying bananas lean toward the owner's colour
constexpr float kMaxAnimStepSeconds = 0.25f;  // a stalled frame must not skip whole clips

[[nodiscard]] constexpr Color mix(Color a, Color b, float t) {
  return {lerp(a.r, b.r, t), lerp(a.g, b.g, t), lerp(a.b, b.b, t)};
}

[[nodiscard]] constexpr float easeOut(float t) {
  return 1.0f - ((1.0f - t) * (1.0f - t));
}

// A sim-plane direction as a world vector on the ground (world_basis.hpp).
[[nodiscard]] constexpr Vec3 planeToWorld(Vec2f d) {
  return {d.x, 0.0f, -d.y};
}

[[nodiscard]] Vec3 yawDirection(float yaw) {
  // rotateY(yaw) maps model +x onto (cos, 0, -sin) (world_basis.hpp).
  return {std::cos(yaw), 0.0f, -std::sin(yaw)};
}

// Uniform blocks: flat vec4 arrays, see shaders.hpp.
struct LitVsParams {
  Mat4 mvp;
  Mat4 model;
};
static_assert(sizeof(LitVsParams) == static_cast<std::size_t>(shaders::kLitVsVec4s) * 16);

struct LitFsParams {
  Vec4 color;
  Vec4 light;
  Vec4 params;  // mode, flash, emissive, unused
  Vec4 floor;   // arena half extents
};
static_assert(sizeof(LitFsParams) == static_cast<std::size_t>(shaders::kLitFsVec4s) * 16);

struct SkinnedVsParams {
  Mat4 mvp;
  Mat4 model;
  SkinMatrices joints;
};
static_assert(sizeof(SkinnedVsParams) == static_cast<std::size_t>(shaders::kSkinnedVsVec4s) * 16);

struct DecalVsParams {
  Mat4 mvp;
};
static_assert(sizeof(DecalVsParams) == static_cast<std::size_t>(shaders::kDecalVsVec4s) * 16);

struct DecalFsParams {
  Vec4 color;
  Vec4 shape;  // mode, inner, unused, unused
};
static_assert(sizeof(DecalFsParams) == static_cast<std::size_t>(shaders::kDecalFsVec4s) * 16);

enum class DecalMode : uint8_t { Shadow = 0, Ring = 1, Burst = 2, Rect = 3 };

template <typename T>
[[nodiscard]] sg_range rangeOf(const T& value) {
  return sg_range{&value, sizeof(T)};
}

[[nodiscard]] uint32_t makeBuffer(const void* data, std::size_t bytes, bool index,
                                  const char* label) {
  sg_buffer_desc desc{};
  desc.usage.vertex_buffer = !index;
  desc.usage.index_buffer = index;
  desc.data = sg_range{data, bytes};
  desc.label = label;
  const sg_buffer buffer = sg_make_buffer(&desc);
  return buffer.id;
}

[[nodiscard]] uint32_t makeShader(const char* vs, const char* fs, int vsVec4s, int fsVec4s,
                                  const char* label, std::size_t attrCount = 3) {
  sg_shader_desc desc{};
  desc.vertex_func.source = vs;
  desc.fragment_func.source = fs;
  // Attributes are bound by layout(location) in the GLSL (no glsl_name, so
  // a location the compiler drops, like the decal's unused normal, is fine).
  for (sg_shader_vertex_attr& attr : std::span(desc.attrs).first(attrCount))
    attr.base_type = SG_SHADERATTRBASETYPE_FLOAT;

  sg_shader_uniform_block& vsBlock = desc.uniform_blocks[0];
  vsBlock.stage = SG_SHADERSTAGE_VERTEX;
  vsBlock.size = static_cast<uint32_t>(vsVec4s) * 16U;
  vsBlock.layout = SG_UNIFORMLAYOUT_STD140;
  vsBlock.glsl_uniforms[0] = {.type = SG_UNIFORMTYPE_FLOAT4,
                              .array_count = static_cast<uint16_t>(vsVec4s),
                              .glsl_name = "vs_params"};

  sg_shader_uniform_block& fsBlock = desc.uniform_blocks[1];
  fsBlock.stage = SG_SHADERSTAGE_FRAGMENT;
  fsBlock.size = static_cast<uint32_t>(fsVec4s) * 16U;
  fsBlock.layout = SG_UNIFORMLAYOUT_STD140;
  fsBlock.glsl_uniforms[0] = {.type = SG_UNIFORMTYPE_FLOAT4,
                              .array_count = static_cast<uint16_t>(fsVec4s),
                              .glsl_name = "fs_params"};
  desc.label = label;
  return sg_make_shader(&desc).id;
}

enum class Blend : uint8_t { Opaque, Alpha, Additive };

[[nodiscard]] uint32_t makePipeline(uint32_t shader, Blend blend, const char* label) {
  sg_pipeline_desc desc{};
  desc.shader = sg_shader{shader};
  desc.layout.buffers[0].stride = static_cast<int>(sizeof(MeshVertex));
  desc.layout.attrs[0] = {.buffer_index = 0,
                          .offset = static_cast<int>(offsetof(MeshVertex, px)),
                          .format = SG_VERTEXFORMAT_FLOAT3};
  desc.layout.attrs[1] = {.buffer_index = 0,
                          .offset = static_cast<int>(offsetof(MeshVertex, nx)),
                          .format = SG_VERTEXFORMAT_FLOAT3};
  desc.layout.attrs[2] = {.buffer_index = 0,
                          .offset = static_cast<int>(offsetof(MeshVertex, u)),
                          .format = SG_VERTEXFORMAT_FLOAT2};
  desc.index_type = SG_INDEXTYPE_UINT16;
  desc.face_winding = SG_FACEWINDING_CCW;
  desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
  if (blend == Blend::Opaque) {
    desc.cull_mode = SG_CULLMODE_BACK;
    desc.depth.write_enabled = true;
  } else {
    // Decals and effects: depth-tested against the opaque scene, never
    // written, pulled slightly toward the camera against z-fighting.
    desc.cull_mode = SG_CULLMODE_NONE;
    desc.depth.write_enabled = false;
    desc.depth.bias = -1.0f;
    desc.depth.bias_slope_scale = -1.0f;
    sg_blend_state& b = desc.colors[0].blend;
    b.enabled = true;
    b.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    b.dst_factor_rgb =
        blend == Blend::Additive ? SG_BLENDFACTOR_ONE : SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    b.src_factor_alpha = SG_BLENDFACTOR_ZERO;
    b.dst_factor_alpha = SG_BLENDFACTOR_ONE;
  }
  desc.label = label;
  return sg_make_pipeline(&desc).id;
}

// The skinned character pipeline: opaque like `lit`, SkinnedVertex layout.
[[nodiscard]] uint32_t makeSkinnedPipeline(uint32_t shader) {
  sg_pipeline_desc desc{};
  desc.shader = sg_shader{shader};
  desc.layout.buffers[0].stride = static_cast<int>(sizeof(SkinnedVertex));
  desc.layout.attrs[0] = {.buffer_index = 0,
                          .offset = static_cast<int>(offsetof(SkinnedVertex, px)),
                          .format = SG_VERTEXFORMAT_FLOAT3};
  desc.layout.attrs[1] = {.buffer_index = 0,
                          .offset = static_cast<int>(offsetof(SkinnedVertex, nx)),
                          .format = SG_VERTEXFORMAT_FLOAT3};
  desc.layout.attrs[2] = {.buffer_index = 0,
                          .offset = static_cast<int>(offsetof(SkinnedVertex, joints)),
                          .format = SG_VERTEXFORMAT_FLOAT4};
  desc.layout.attrs[3] = {.buffer_index = 0,
                          .offset = static_cast<int>(offsetof(SkinnedVertex, weights)),
                          .format = SG_VERTEXFORMAT_FLOAT4};
  desc.index_type = SG_INDEXTYPE_UINT16;
  desc.face_winding = SG_FACEWINDING_CCW;
  desc.cull_mode = SG_CULLMODE_BACK;
  desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
  desc.depth.write_enabled = true;
  desc.label = "kfm-skinned";
  return sg_make_pipeline(&desc).id;
}

// Per-frame drawing context: the current pipeline/mesh (to skip redundant
// binds) and the stats. Lives on the stack inside draw().
class FrameDrawer {
 public:
  FrameDrawer(std::span<const uint32_t> pipelines, std::span<const uint32_t> vertexBuffers,
              std::span<const uint32_t> indexBuffers, std::span<const uint32_t> indexCounts,
              const Mat4& viewProj, Vec3 camRight, Vec3 camUp, Vec3 camBack, RendererStats& stats)
      : pipelines_(pipelines),
        vertexBuffers_(vertexBuffers),
        indexBuffers_(indexBuffers),
        indexCounts_(indexCounts),
        viewProj_(viewProj),
        camRight_(camRight),
        camUp_(camUp),
        camBack_(camBack),
        stats_(stats) {}

  void usePipeline(std::size_t pipeline) {
    if (pipeline == currentPipeline_) return;
    sg_apply_pipeline(sg_pipeline{pipelines_[pipeline]});
    currentPipeline_ = pipeline;
    currentMesh_ = kNone;  // bindings must be re-applied after a pipeline change
  }

  void lit(std::size_t mesh, const Mat4& model, Color color, float flash = 0.0f,
           float emissive = 0.0f) {
    usePipeline(kLitPipeline);
    const LitVsParams vs{.mvp = viewProj_ * model, .model = model};
    const Vec3 l = normalize(kToLight);
    const LitFsParams fs{.color = {color.r, color.g, color.b, 1.0f},
                         .light = {l.x, l.y, l.z, kShadeBand},
                         .params = {0.0f, flash, emissive, 0.0f},
                         .floor = {}};
    submit(mesh, rangeOf(vs), rangeOf(fs));
  }

  // A skinned character with the lit toon ramp (shaders.hpp, kSkinnedVs).
  void skinned(const Mat4& model, const SkinMatrices& joints, Color color, float flash,
               float emissive) {
    usePipeline(kSkinnedPipeline);
    const SkinnedVsParams vs{.mvp = viewProj_ * model, .model = model, .joints = joints};
    const Vec3 l = normalize(kToLight);
    const LitFsParams fs{.color = {color.r, color.g, color.b, 1.0f},
                         .light = {l.x, l.y, l.z, kShadeBand},
                         .params = {0.0f, flash, emissive, 0.0f},
                         .floor = {}};
    submit(kSkinnedMesh, rangeOf(vs), rangeOf(fs));
  }

  void floor(const Mat4& model, Vec2f halfExtents) {
    usePipeline(kLitPipeline);
    const LitVsParams vs{.mvp = viewProj_ * model, .model = model};
    const Vec3 l = normalize(kToLight);
    const LitFsParams fs{.color = {kFloorColor.r, kFloorColor.g, kFloorColor.b, 1.0f},
                         .light = {l.x, l.y, l.z, kShadeBand},
                         .params = {1.0f, 0.0f, 0.0f, 0.0f},
                         .floor = {halfExtents.x, halfExtents.y, 0.0f, 0.0f}};
    submit(kFloorMesh, rangeOf(vs), rangeOf(fs));
  }

  // A disc lying on the ground (y = lift) under `center`, scaled `along` the
  // yaw direction and `across` it.
  void groundDisc(Vec3 center, float lift, float yaw, float along, float across, Color color,
                  float alpha, DecalMode mode, float inner = 0.0f) {
    const Mat4 model = translate({center.x, lift, center.z}) * rotateY(yaw) *
                       scale({std::max(along, 1e-3f), 1.0f, std::max(across, 1e-3f)});
    decal(model, color, alpha, mode, inner);
  }

  // A floor rectangle (v0.33, zones): `alpha` at the rim band from `inner`
  // of the half-size outward, `fill` x alpha inside it.
  void groundRect(Vec3 center, float lift, float sizeX, float sizeZ, Color color, float alpha,
                  float inner, float fill) {
    if (alpha <= 0.0f) return;
    const Mat4 model = translate({center.x, lift, center.z}) *
                       scale({std::max(sizeX, 1e-3f), 1.0f, std::max(sizeZ, 1e-3f)});
    const DecalVsParams vs{.mvp = viewProj_ * model};
    const DecalFsParams fs{.color = {color.r, color.g, color.b, clamp01(alpha)},
                           .shape = {static_cast<float>(DecalMode::Rect), inner, fill, 0.0f}};
    submit(kQuadMesh, rangeOf(vs), rangeOf(fs));
  }

  // A disc at `center` facing the camera.
  void billboard(Vec3 center, float radius, Color color, float alpha, DecalMode mode,
                 float inner = 0.0f) {
    const float s = std::max(radius, 1e-3f);
    // Disc model axes: x -> camera right, y (its normal) -> toward the camera,
    // z -> screen down, which keeps the basis right-handed.
    Mat4 model = Mat4::identity();
    const Vec3 x = camRight_ * s;
    const Vec3 y = camBack_ * s;
    const Vec3 z = camUp_ * -s;
    model.at(0, 0) = x.x;
    model.at(1, 0) = x.y;
    model.at(2, 0) = x.z;
    model.at(0, 1) = y.x;
    model.at(1, 1) = y.y;
    model.at(2, 1) = y.z;
    model.at(0, 2) = z.x;
    model.at(1, 2) = z.y;
    model.at(2, 2) = z.z;
    model.at(0, 3) = center.x;
    model.at(1, 3) = center.y;
    model.at(2, 3) = center.z;
    decal(model, color, alpha, mode, inner);
  }

 private:
  static constexpr std::size_t kNone = ~std::size_t{0};

  void decal(const Mat4& model, Color color, float alpha, DecalMode mode, float inner) {
    if (alpha <= 0.0f) return;
    const DecalVsParams vs{.mvp = viewProj_ * model};
    const DecalFsParams fs{.color = {color.r, color.g, color.b, clamp01(alpha)},
                           .shape = {static_cast<float>(mode), inner, 0.0f, 0.0f}};
    submit(kDiscMesh, rangeOf(vs), rangeOf(fs));
  }

  void submit(std::size_t mesh, const sg_range& vs, const sg_range& fs) {
    if (mesh != currentMesh_) {
      sg_bindings bind{};
      bind.vertex_buffers[0] = sg_buffer{vertexBuffers_[mesh]};
      bind.index_buffer = sg_buffer{indexBuffers_[mesh]};
      sg_apply_bindings(&bind);
      currentMesh_ = mesh;
    }
    sg_apply_uniforms(0, &vs);
    sg_apply_uniforms(1, &fs);
    const uint32_t count = indexCounts_[mesh];
    sg_draw(0, static_cast<int>(count), 1);
    ++stats_.drawCalls;
    stats_.triangles += count / 3U;
  }

  std::span<const uint32_t> pipelines_;
  std::span<const uint32_t> vertexBuffers_;
  std::span<const uint32_t> indexBuffers_;
  std::span<const uint32_t> indexCounts_;
  Mat4 viewProj_;
  Vec3 camRight_;
  Vec3 camUp_;
  Vec3 camBack_;
  RendererStats& stats_;
  std::size_t currentPipeline_{kNone};
  std::size_t currentMesh_{kNone};
};

// n billboards flying out from an effect's position: around the full circle,
// or within `spread` radians of `bias` when it is non-zero. `seed` rotates
// the pattern so overlapping bursts differ.
struct SparkStyle {
  int count{0};
  float reach{0.0f};   // distance travelled at t = 1
  float size{0.0f};    // billboard radius at t = 0
  float shrink{0.0f};  // fraction of size lost by t = 1
  float rise{0.0f};    // height gained by t = 1
  float spread{0.0f};  // radians around bias (ignored without a bias)
  Color color{};
  float alpha{0.0f};
  DecalMode mode{DecalMode::Burst};
};

void drawSparks(FrameDrawer& d, const RenderEffect& e, const SparkStyle& s) {
  const float t = e.progress();
  const float fade = 1.0f - t;
  const bool cone = length(e.direction) > 1e-4f;
  const float base =
      cone ? std::atan2(e.direction.y, e.direction.x) : 0.61f * static_cast<float>(e.slot + 2);
  const float reach = s.reach * easeOut(t);
  for (int k = 0; k < s.count; ++k) {
    const float u = s.count > 1 ? static_cast<float>(k) / static_cast<float>(s.count - 1) : 0.5f;
    const float a = cone ? base + (s.spread * (u - 0.5f))
                         : base + (kTwoPi * static_cast<float>(k) / static_cast<float>(s.count));
    // Alternate spark lengths a little so the burst is not a perfect circle.
    const float r = reach * ((k % 2 == 0) ? 1.0f : 0.75f);
    const Vec3 at =
        e.position + (planeToWorld({std::cos(a), std::sin(a)}) * r) + Vec3{0.0f, s.rise * t, 0.0f};
    d.billboard(at, s.size * (1.0f - (s.shrink * t)), s.color, s.alpha * fade, s.mode, 0.15f);
  }
}

// A power-up pickup (§5.8): the def's shape in its palette colour, spinning
// and glowing a little, from the placeholder meshes. playerRadius and
// playerHeight are the capsule mesh's dimensions (orb and coin are scaled
// capsules).
void drawPowerUp(FrameDrawer& d, const RenderPickup& p, float playerRadius, float playerHeight,
                 float time) {
  const Color c = playerColor(p.colorIndex);
  const float glow = 0.25f + (0.15f * std::sin(kTwoPi * kPowerUpRingPulseHz * time));
  const float s = kPowerUpSize;
  const Vec3 at = p.position + Vec3{0.0f, kPowerUpHeight - kPickupHeight, 0.0f};
  const Mat4 base = translate(at) * rotateY(p.spin);
  const float rx = playerRadius > 0.0f ? (0.5f * s) / playerRadius : 1.0f;
  const float hy = playerHeight > 0.0f ? 1.0f / playerHeight : 1.0f;
  switch (p.shape) {
    case PowerUpShapeView::Orb:
      // The capsule squashed to its own width: a ball centred on `at`.
      d.lit(kCapsuleMesh, base * translate({0.0f, -0.5f * s, 0.0f}) * scale({rx, s * hy, rx}), c,
            0.0f, glow);
      break;
    case PowerUpShapeView::Cube:
      d.lit(kCubeMesh, base * rotateX(0.35f) * scale({0.75f * s, 0.75f * s, 0.75f * s}), c, 0.0f,
            glow);
      break;
    case PowerUpShapeView::Diamond:
      d.lit(kCubeMesh,
            base * rotateZ(0.25f * kPi) * rotateX(0.25f * kPi) *
                scale({0.62f * s, 0.62f * s, 0.62f * s}),
            c, 0.0f, glow);
      break;
    case PowerUpShapeView::Coin:
      // A flat capsule stood on its edge.
      d.lit(kCapsuleMesh,
            base * rotateX(0.5f * kPi) * translate({0.0f, -0.1f * s, 0.0f}) *
                scale({rx, 0.2f * s * hy, rx}),
            c, 0.0f, glow);
      break;
  }
}

}  // namespace

bool Renderer::init(const sim::MatchContent& match) {
  const sim::ContentDB& content = match.db;
  if (initialised_) shutdown();
  if (!sg_isvalid()) return false;

  const std::array<MeshData, kStaticMeshCount> data = {
      makeMonkeyCapsule(content.player),
      makeBananaMesh(content.boomerang),
      makeDisc(1.0f, 40),
      makeFloorQuad(match.arena.halfExtents.x.toFloat(), match.arena.halfExtents.y.toFloat()),
      makeUnitCube(),
      makeFloorQuad(0.5f, 0.5f),
  };
  constexpr std::array<const char*, kStaticMeshCount> kLabels = {
      "kfm-capsule", "kfm-banana", "kfm-disc", "kfm-floor", "kfm-cube", "kfm-quad"};
  for (std::size_t i = 0; i < kStaticMeshCount; ++i) {
    const MeshData& m = data[i];
    meshes_[i].vertexBuffer =
        makeBuffer(m.vertices.data(), m.vertices.size() * sizeof(MeshVertex), false, kLabels[i]);
    meshes_[i].indexBuffer =
        makeBuffer(m.indices.data(), m.indices.size() * sizeof(uint16_t), true, kLabels[i]);
    meshes_[i].indexCount = static_cast<uint32_t>(m.indices.size());
  }

  shaders_[kLitShader] = makeShader(shaders::kLitVs, shaders::kLitFs, shaders::kLitVsVec4s,
                                    shaders::kLitFsVec4s, "kfm-lit-shader");
  shaders_[kDecalShader] = makeShader(shaders::kDecalVs, shaders::kDecalFs, shaders::kDecalVsVec4s,
                                      shaders::kDecalFsVec4s, "kfm-decal-shader");
  pipelines_[kLitPipeline] = makePipeline(shaders_[kLitShader], Blend::Opaque, "kfm-lit");
  pipelines_[kDecalAlphaPipeline] =
      makePipeline(shaders_[kDecalShader], Blend::Alpha, "kfm-decal-alpha");
  pipelines_[kDecalAdditivePipeline] =
      makePipeline(shaders_[kDecalShader], Blend::Additive, "kfm-decal-additive");

  bool ok = true;
  for (const GpuMesh& m : std::span(meshes_).first(kStaticMeshCount)) {
    ok = ok && sg_query_buffer_state(sg_buffer{m.vertexBuffer}) == SG_RESOURCESTATE_VALID &&
         sg_query_buffer_state(sg_buffer{m.indexBuffer}) == SG_RESOURCESTATE_VALID;
  }
  for (const uint32_t s : std::span(shaders_).first(kStaticShaderCount))
    ok = ok && sg_query_shader_state(sg_shader{s}) == SG_RESOURCESTATE_VALID;
  for (const uint32_t p : std::span(pipelines_).first(kStaticPipelineCount))
    ok = ok && sg_query_pipeline_state(sg_pipeline{p}) == SG_RESOURCESTATE_VALID;
  initialised_ = true;  // so shutdown() releases whatever was created
  if (!ok) {
    shutdown();
    return false;
  }
  return true;
}

void Renderer::destroyResources() {
  // An id of 0 (a slot never created) is invalid, which sokol_gfx ignores.
  for (const uint32_t p : pipelines_)
    sg_destroy_pipeline(sg_pipeline{p});
  for (const uint32_t s : shaders_)
    sg_destroy_shader(sg_shader{s});
  for (const GpuMesh& m : meshes_) {
    sg_destroy_buffer(sg_buffer{m.vertexBuffer});
    sg_destroy_buffer(sg_buffer{m.indexBuffer});
  }
  pipelines_ = {};
  shaders_ = {};
  meshes_ = {};
}

void Renderer::destroyCharacterModel() {
  if (sg_isvalid()) {
    sg_destroy_pipeline(sg_pipeline{pipelines_[kSkinnedPipeline]});
    sg_destroy_shader(sg_shader{shaders_[kSkinnedShader]});
    sg_destroy_buffer(sg_buffer{meshes_[kSkinnedMesh].vertexBuffer});
    sg_destroy_buffer(sg_buffer{meshes_[kSkinnedMesh].indexBuffer});
  }
  pipelines_[kSkinnedPipeline] = 0;
  shaders_[kSkinnedShader] = 0;
  meshes_[kSkinnedMesh] = {};
  model_ = SkinnedModel{};
  clips_ = {};
  anim_ = {};
  lastAnimTime_ = -1.0f;
  skinnedLoaded_ = false;
}

void Renderer::shutdown() {
  if (initialised_ && sg_isvalid()) destroyResources();
  initialised_ = false;
  stats_ = {};
  // destroyResources() released the character's GPU side; drop the CPU side.
  model_ = SkinnedModel{};
  clips_ = {};
  anim_ = {};
  lastAnimTime_ = -1.0f;
  skinnedLoaded_ = false;
}

bool Renderer::loadCharacterModel(const char* path) {
  if (!initialised_ || !sg_isvalid()) {
    modelError_ = "the renderer is not initialised";
    return false;
  }
  SkinnedModel loaded;
  std::string error;
  if (!loadSkinnedModel(path, loaded, error)) {
    modelError_ = error;
    return false;
  }
  destroyCharacterModel();
  model_ = std::move(loaded);

  GpuMesh& mesh = meshes_[kSkinnedMesh];
  mesh.vertexBuffer =
      makeBuffer(model_.vertices.data(), model_.vertices.size() * sizeof(SkinnedVertex), false,
                 "kfm-character");
  mesh.indexBuffer = makeBuffer(model_.indices.data(), model_.indices.size() * sizeof(uint16_t),
                                true, "kfm-character");
  mesh.indexCount = static_cast<uint32_t>(model_.indices.size());
  shaders_[kSkinnedShader] =
      makeShader(shaders::kSkinnedVs, shaders::kLitFs, shaders::kSkinnedVsVec4s,
                 shaders::kLitFsVec4s, "kfm-skinned-shader", 4);
  pipelines_[kSkinnedPipeline] = makeSkinnedPipeline(shaders_[kSkinnedShader]);

  const bool ok =
      sg_query_buffer_state(sg_buffer{mesh.vertexBuffer}) == SG_RESOURCESTATE_VALID &&
      sg_query_buffer_state(sg_buffer{mesh.indexBuffer}) == SG_RESOURCESTATE_VALID &&
      sg_query_shader_state(sg_shader{shaders_[kSkinnedShader]}) == SG_RESOURCESTATE_VALID &&
      sg_query_pipeline_state(sg_pipeline{pipelines_[kSkinnedPipeline]}) == SG_RESOURCESTATE_VALID;
  if (!ok) {
    destroyCharacterModel();
    modelError_ = "creating the skinned character's GPU resources failed";
    return false;
  }
  clips_ = bindClips(model_);
  skinnedLoaded_ = true;
  modelError_.clear();
  return true;
}

void Renderer::draw(const RenderState& state, const Camera& camera, int width, int height) {
  stats_ = {};
  if (!initialised_ || width <= 0 || height <= 0) return;
  sg_apply_viewport(0, 0, width, height, true);

  // Screen shake moves the eye and the target together.
  const Mat4 view = lookAt(camera.eye + state.shakeOffset, camera.target + state.shakeOffset,
                           Vec3{0.0f, 1.0f, 0.0f});
  const Mat4 viewProj = camera.proj * view;
  const Vec3 camRight{view.at(0, 0), view.at(0, 1), view.at(0, 2)};
  const Vec3 camUp{view.at(1, 0), view.at(1, 1), view.at(1, 2)};
  const Vec3 camBack{view.at(2, 0), view.at(2, 1), view.at(2, 2)};

  std::array<uint32_t, kMeshCount> vbufs{};
  std::array<uint32_t, kMeshCount> ibufs{};
  std::array<uint32_t, kMeshCount> counts{};
  for (std::size_t i = 0; i < kMeshCount; ++i) {
    vbufs[i] = meshes_[i].vertexBuffer;
    ibufs[i] = meshes_[i].indexBuffer;
    counts[i] = meshes_[i].indexCount;
  }
  FrameDrawer d(pipelines_, vbufs, ibufs, counts, viewProj, camRight, camUp, camBack, stats_);

  const float r = state.playerRadius;
  const float bodyHeight = state.playerHeight;
  const Vec2f half = state.arenaHalfExtents;

  // --- opaque ------------------------------------------------------------------
  d.floor(Mat4::identity(), half);
  {
    // Wall rim: four low boxes just outside the arena bounds.
    const float y = kWallHeight * 0.5f;
    const float t = kWallThickness;
    const float lenX = (2.0f * half.x) + (2.0f * t);
    const float lenZ = 2.0f * half.y;
    d.lit(kCubeMesh, translate({0.0f, y, -(half.y + (t * 0.5f))}) * scale({lenX, kWallHeight, t}),
          kWallColor);
    d.lit(kCubeMesh, translate({0.0f, y, half.y + (t * 0.5f)}) * scale({lenX, kWallHeight, t}),
          kWallColor);
    d.lit(kCubeMesh, translate({-(half.x + (t * 0.5f)), y, 0.0f}) * scale({t, kWallHeight, lenZ}),
          kWallColor);
    d.lit(kCubeMesh, translate({half.x + (t * 0.5f), y, 0.0f}) * scale({t, kWallHeight, lenZ}),
          kWallColor);
  }
  // Interior walls and hazards (v0.22): unit cubes scaled to each box. Pits
  // and mud are slabs just above the floor and under the blob shadows; a
  // pit's rim strips sit on top of its near-black slab (half its height).
  for (uint16_t i = 0; i < state.arenaPieceCount; ++i) {
    const RenderArenaPiece& piece = state.arenaPieces[i];
    const Vec3 c = piece.center;
    const Vec3 s = piece.size;
    switch (piece.kind) {
      case ArenaPieceKind::Wall:
        d.lit(kCubeMesh, translate({c.x, s.y * 0.5f, c.z}) * scale(s), kArenaWallColor);
        break;
      case ArenaPieceKind::Pit:
        d.lit(kCubeMesh, translate({c.x, s.y * 0.25f, c.z}) * scale({s.x, s.y * 0.5f, s.z}),
              kArenaPitColor);
        break;
      case ArenaPieceKind::PitRim:
        d.lit(kCubeMesh, translate({c.x, s.y * 0.5f, c.z}) * scale(s), kArenaPitRimColor);
        break;
      case ArenaPieceKind::Slow:
        d.lit(kCubeMesh, translate({c.x, s.y * 0.5f, c.z}) * scale(s), kArenaSlowColor);
        break;
    }
  }

  // Character animation advances with presentation time; time going
  // backwards (a fresh PresentationState) counts as no time.
  const bool skinnedBodies = skinnedLoaded_ && !forceCapsules_;
  if (skinnedBodies) {
    const float dt = lastAnimTime_ < 0.0f ? 0.0f : state.time - lastAnimTime_;
    updateCharacterAnimation(anim_, state, std::clamp(dt, 0.0f, kMaxAnimStepSeconds));
    lastAnimTime_ = state.time;
  }
  // The model's bind pose is scaled to the capsule's height.
  const float modelScale =
      skinnedBodies && model_.height() > 0.0f ? bodyHeight / model_.height() : 1.0f;

  const float blink = std::fmod(state.time * 8.0f, 1.0f) < 0.5f ? 1.0f : 0.0f;
  for (uint16_t i = 0; i < state.playerCount; ++i) {
    const RenderPlayer& p = state.players[i];
    if (!p.visible) continue;
    const bool eliminated = p.has(PlayerFlag::Eliminated);
    const Color own = playerColor(p.colorIndex);
    const Color body = eliminated ? mix(own, kEliminatedColor, 0.8f) : own;
    float flash = p.flash;
    if (p.has(PlayerFlag::Invulnerable)) flash = std::max(flash, 0.35f * blink);
    const float emissive = p.has(PlayerFlag::Dashing) ? 0.25f : 0.0f;
    const Mat4 placed = translate(p.position) * rotateY(p.bodyYaw) * scale(p.scale);
    if (skinnedBodies && p.slot < anim_.size()) {
      SkinMatrices joints;
      evaluateAnimState(model_, clips_, anim_[p.slot], joints);
      d.skinned(placed * scale({modelScale, modelScale, modelScale}), joints, body, flash,
                emissive);
    } else {
      d.lit(kCapsuleMesh, placed, body, flash, emissive);
    }
  }

  // Aim indicators and bananas in hand, after all the bodies so the pipeline
  // changes once.
  for (uint16_t i = 0; i < state.playerCount; ++i) {
    const RenderPlayer& p = state.players[i];
    if (!p.visible || p.has(PlayerFlag::Eliminated)) continue;
    const Color own = playerColor(p.colorIndex);

    // Aim indicator: a thin bar from the capsule surface along the aim. A
    // charging throw (v0.48) stretches it and warms it toward gold.
    const Vec3 aimBase = p.position + Vec3{0.0f, bodyHeight * kAimHeightFraction, 0.0f};
    const float charge = p.has(PlayerFlag::Charging) ? p.throwCharge : 0.0f;
    const float aimLength =
        p.has(PlayerFlag::Charging) ? lerp(kAimLength, kChargeAimLength, charge) : kAimLength;
    d.lit(kCubeMesh,
          translate(aimBase) * rotateY(p.aimYaw) * translate({r + (aimLength * 0.5f), 0.0f, 0.0f}) *
              scale({aimLength, kAimThickness, kAimThickness}),
          mix(mix(own, kWhite, 0.45f), kGold, charge), 0.0f, 0.15f + (0.35f * charge));
    // v0.33: a capture flag's carrier wears a small gold diamond overhead.
    if (p.has(PlayerFlag::Carrying)) {
      const float s = kFlagCarrySize;
      d.lit(kCubeMesh,
            translate(p.position + Vec3{0.0f, bodyHeight + kFlagCarryLift, 0.0f}) *
                rotateY(state.time * kFlagCarrySpinRadPerSec) * rotateZ(0.25f * kPi) *
                rotateX(0.25f * kPi) * scale({s, s, s}),
            kGold, 0.0f, 0.3f);
    }
    // A banana in hand while the player has ammo.
    if (p.has(PlayerFlag::HasAmmo)) {
      d.lit(kBananaMesh,
            translate(aimBase) * rotateY(p.aimYaw) * translate({r * 0.55f, 0.0f, r * 0.85f}) *
                scale({kHeldBananaScale, kHeldBananaScale, kHeldBananaScale}),
            kBananaColor, 0.0f, 0.1f);
    }
  }

  for (uint16_t i = 0; i < state.bananaCount; ++i) {
    const RenderBanana& b = state.bananas[i];
    Color c = kBananaColor;
    if (b.ownerSlot >= 0) c = mix(c, playerColor(static_cast<uint32_t>(b.ownerSlot)), kOwnerTint);
    d.lit(kBananaMesh, translate(b.position) * rotateY(b.headingYaw + b.spin), c, 0.0f, 0.15f);
  }
  for (uint16_t i = 0; i < state.pickupCount; ++i) {
    const RenderPickup& p = state.pickups[i];
    if (p.kind == PickupKindView::PowerUp || p.kind == PickupKindView::Flag) {
      drawPowerUp(d, p, r, bodyHeight, state.time);  // a flag: its team's colour (v0.33)
    } else {
      d.lit(kBananaMesh, translate(p.position) * rotateY(p.spin), kPickupColor, 0.0f, 0.1f);
    }
  }

  // --- ground decals (alpha) -------------------------------------------------------
  d.usePipeline(kDecalAlphaPipeline);
  // Objective zones (v0.33) under everything else on the floor.
  for (uint8_t i = 0; i < state.zoneCount; ++i) {
    const RenderZone& z = state.zones[i];
    const Color c = z.team >= 0 ? playerColor(static_cast<uint32_t>(z.team)) : kWhite;
    d.groundRect(z.center, kZoneLift, z.size.x, z.size.z, c, kZoneRimOpacity, kZoneRimStart,
                 kZoneOpacity / kZoneRimOpacity);
  }
  for (uint16_t i = 0; i < state.shadowCount; ++i) {
    const RenderShadow& s = state.shadows[i];
    d.groundDisc(s.ground, kShadowLift, 0.0f, s.radius, s.radius, kShadowColor,
                 kShadowOpacity * s.opacity, DecalMode::Shadow);
  }
  for (uint16_t i = 0; i < state.playerCount; ++i) {
    const RenderPlayer& p = state.players[i];
    if (!p.visible || p.has(PlayerFlag::Eliminated)) continue;
    const Color own = playerColor(p.colorIndex);
    if (p.has(PlayerFlag::Dashing)) {
      // A stretched trail behind the dash.
      const Vec3 behind = p.ground - (yawDirection(p.bodyYaw) * (r * 1.4f));
      d.groundDisc(behind, kDecalLift, p.bodyYaw, r * 2.2f, r * 0.85f, mix(own, kWhite, 0.4f), 0.5f,
                   DecalMode::Burst, 0.2f);
    }
    if (p.has(PlayerFlag::PerfectDash)) {
      // The catch window: a bright ring around the feet while it is open.
      d.groundDisc(p.ground, kDecalLift, 0.0f, r * 1.9f, r * 1.9f, kGold, 0.95f, DecalMode::Ring,
                   0.72f);
    }
    if (p.has(PlayerFlag::Shielded)) {
      d.groundDisc(p.ground, kDecalLift, 0.0f, r * 1.5f, r * 1.5f, kShieldBlue, 0.7f,
                   DecalMode::Ring, 0.8f);
    }
    if (p.has(PlayerFlag::Charging)) {
      // v0.48: where a banana released now turns for home.
      const Vec3 at = p.ground + (yawDirection(p.aimYaw) * p.throwReach);
      d.groundDisc(at, kDecalLift, 0.0f, kChargeMarkerRadius, kChargeMarkerRadius,
                   mix(mix(own, kWhite, 0.3f), kGold, p.throwCharge), kChargeMarkerOpacity,
                   DecalMode::Ring, kChargeMarkerRingInner);
    }
  }
  // A pulsing ring in the power-up's colour marks a power-up pickup.
  for (uint16_t i = 0; i < state.pickupCount; ++i) {
    const RenderPickup& p = state.pickups[i];
    if (p.kind == PickupKindView::Banana) continue;
    const float pulse = 0.5f + (0.5f * std::sin(kTwoPi * kPowerUpRingPulseHz * state.time));
    d.groundDisc(p.ground, kDecalLift, 0.0f, kPowerUpRingRadius, kPowerUpRingRadius,
                 playerColor(p.colorIndex), lerp(0.45f, 0.9f, pulse), DecalMode::Ring, 0.75f);
  }
  for (uint16_t i = 0; i < state.effectCount; ++i) {
    const RenderEffect& e = state.effects[i];
    const float t = e.progress();
    const float fade = 1.0f - t;
    switch (e.kind) {
      case EffectKind::DashRing:
        d.groundDisc(e.position, kDecalLift, 0.0f, r * lerp(0.9f, 2.4f, t), r * lerp(0.9f, 2.4f, t),
                     kWhite, 0.8f * fade, DecalMode::Ring, 0.78f);
        break;
      case EffectKind::PerfectCatchFlash:
        d.groundDisc(e.position, kDecalLift, 0.0f, r * lerp(1.0f, 4.0f, t), r * lerp(1.0f, 4.0f, t),
                     kGold, fade, DecalMode::Ring, 0.82f);
        break;
      case EffectKind::EliminationBurst: {
        // Alpha-blended so the player's colour stays saturated over the floor.
        const Color c = e.slot >= 0 ? playerColor(static_cast<uint32_t>(e.slot)) : kWhite;
        d.groundDisc(e.position, kDecalLift, 0.0f, r * lerp(1.0f, 6.0f, t), r * lerp(1.0f, 6.0f, t),
                     c, 0.9f * fade, DecalMode::Ring, 0.85f);
        d.billboard(e.position, lerp(0.8f, 2.6f, t), c, 0.85f * fade, DecalMode::Burst, 0.35f);
        break;
      }
      case EffectKind::HitFlash:
        // Alpha-blended: additive red over a grey floor would read as pink.
        d.billboard(e.position, lerp(0.6f, 1.4f, t), kHitRed, 0.9f * fade, DecalMode::Burst, 0.3f);
        break;
      case EffectKind::DashAnticipation: {
        // A ring collapsing onto the dasher: the dash reads as deliberate at
        // the press, whatever it resolves to later (§12).
        const Color c =
            e.slot >= 0 ? mix(playerColor(static_cast<uint32_t>(e.slot)), kWhite, 0.5f) : kWhite;
        const float rr = r * lerp(2.6f, 1.0f, easeOut(t));
        d.groundDisc(e.position, kDecalLift, 0.0f, rr, rr, c, 0.9f * fade, DecalMode::Ring, 0.8f);
        break;
      }
      case EffectKind::DashTrail: {
        const Color c =
            e.slot >= 0 ? mix(playerColor(static_cast<uint32_t>(e.slot)), kWhite, 0.3f) : kWhite;
        d.billboard(e.position, r * lerp(1.0f, 0.4f, t), c, 0.45f * fade, DecalMode::Burst, 0.3f);
        break;
      }
      case EffectKind::EliminationPoof:
        drawSparks(d, e,
                   SparkStyle{.count = kEliminationPuffs,
                              .reach = 0.9f,
                              .size = 0.3f,
                              .shrink = -1.0f,
                              .rise = 0.8f,
                              .spread = 0.0f,
                              .color = kSmoke,
                              .alpha = 0.6f,
                              .mode = DecalMode::Burst});
        break;
      case EffectKind::PowerUpBurst: {
        const Color c = e.slot >= 0 ? playerColor(static_cast<uint32_t>(e.slot)) : kWhite;
        const float rr = lerp(kPowerUpRingRadius, 2.2f, easeOut(t));
        d.groundDisc(e.position, kDecalLift, 0.0f, rr, rr, c, 0.9f * fade, DecalMode::Ring, 0.8f);
        break;
      }
      case EffectKind::ShieldShatter: {
        const float rr = r * lerp(1.5f, 2.8f, easeOut(t));
        d.groundDisc(e.position, kDecalLift, 0.0f, rr, rr, kShieldBlue, 0.9f * fade,
                     DecalMode::Ring, 0.85f);
        break;
      }
      case EffectKind::GroundDust:
        drawSparks(d, e,
                   SparkStyle{.count = kDustPuffs,
                              .reach = 0.6f,
                              .size = 0.16f,
                              .shrink = -1.2f,
                              .rise = 0.15f,
                              .spread = 0.0f,
                              .color = kDust,
                              .alpha = 0.5f,
                              .mode = DecalMode::Burst});
        break;
      case EffectKind::RespawnBurst: {
        // A ring collapsing onto the spawn point and a glow rising out of it.
        const Color c =
            e.slot >= 0 ? mix(playerColor(static_cast<uint32_t>(e.slot)), kWhite, 0.35f) : kWhite;
        const float rr = r * lerp(3.0f, 1.0f, easeOut(t));
        d.groundDisc(e.position, kDecalLift, 0.0f, rr, rr, c, 0.9f * fade, DecalMode::Ring, 0.8f);
        d.billboard(e.position + Vec3{0.0f, lerp(0.2f, 1.6f, t), 0.0f}, r * lerp(1.6f, 0.6f, t), c,
                    0.6f * fade, DecalMode::Burst, 0.35f);
        break;
      }
      case EffectKind::ScorePop:
        d.groundDisc(e.position, kDecalLift, 0.0f, r * lerp(1.2f, 3.2f, easeOut(t)),
                     r * lerp(1.2f, 3.2f, easeOut(t)), kGold, 0.9f * fade, DecalMode::Ring, 0.85f);
        break;
      case EffectKind::SlamShockwave: {
        // A ring racing out to the reach: what it touches is pushed.
        const float rr = std::max(e.size, r) * lerp(0.3f, 1.0f, easeOut(t));
        const Color c =
            e.slot >= 0 ? mix(playerColor(static_cast<uint32_t>(e.slot)), kWhite, 0.4f) : kWhite;
        d.groundDisc(e.position, kDecalLift, 0.0f, rr, rr, c, 0.9f * fade, DecalMode::Ring, 0.9f);
        break;
      }
      case EffectKind::ParryGuard: {
        const Color c =
            e.slot >= 0 ? mix(playerColor(static_cast<uint32_t>(e.slot)), kWhite, 0.6f) : kWhite;
        const float rr = r * lerp(1.0f, 1.8f, easeOut(t));
        d.groundDisc(e.position, kDecalLift, 0.0f, rr, rr, c, 0.8f * fade, DecalMode::Ring, 0.85f);
        break;
      }
      default:
        break;
    }
  }

  // --- bursts (additive billboards) ------------------------------------------------
  d.usePipeline(kDecalAdditivePipeline);
  for (uint16_t i = 0; i < state.effectCount; ++i) {
    const RenderEffect& e = state.effects[i];
    const float t = e.progress();
    const float fade = 1.0f - t;
    switch (e.kind) {
      case EffectKind::CallShout: {
        // v0.35: a call, rings popping over the caller's head.
        const Color c =
            e.slot >= 0 ? mix(playerColor(static_cast<uint32_t>(e.slot)), kWhite, 0.3f) : kWhite;
        d.billboard(e.position, lerp(0.25f, 0.7f + (0.3f * e.size), easeOut(t)), c, fade,
                    DecalMode::Ring, 0.7f);
        d.billboard(e.position, lerp(0.1f, 0.35f, easeOut(t)), kWhite, fade, DecalMode::Burst,
                    0.2f);
        break;
      }
      case EffectKind::PerfectCatchFlash:
        d.billboard(e.position, lerp(0.6f, 1.8f, t), kGold, fade, DecalMode::Burst, 0.25f);
        d.billboard(e.position, lerp(0.3f, 0.8f, t), kWhite, fade, DecalMode::Burst, 0.1f);
        break;
      case EffectKind::AutoCatchFlash:
        d.billboard(e.position, lerp(0.4f, 0.9f, t), kWhite, 0.7f * fade, DecalMode::Burst, 0.2f);
        break;
      case EffectKind::ThrowPuff:
        d.billboard(e.position, lerp(0.2f, 0.55f, t), kWhite, 0.55f * fade, DecalMode::Burst, 0.1f);
        break;
      case EffectKind::DashAnticipation: {
        // A flash just ahead of the dasher, along the dash.
        const Vec3 ahead = e.position + (planeToWorld(e.direction) * (r * 1.3f)) +
                           Vec3{0.0f, kEffectBodyHeight * 0.6f, 0.0f};
        d.billboard(ahead, lerp(0.5f, 0.2f, t), kWhite, 0.8f * fade, DecalMode::Burst, 0.1f);
        break;
      }
      case EffectKind::BananaTrail: {
        Color c = kBananaColor;
        if (e.slot >= 0) c = mix(c, playerColor(static_cast<uint32_t>(e.slot)), kOwnerTint);
        d.billboard(e.position, lerp(0.16f, 0.04f, t), c, 0.6f * fade, DecalMode::Burst, 0.1f);
        break;
      }
      case EffectKind::PerfectCatchBurst:
        drawSparks(d, e,
                   SparkStyle{.count = kPerfectCatchSparks,
                              .reach = 1.8f,
                              .size = 0.14f,
                              .shrink = 0.5f,
                              .rise = 0.3f,
                              .spread = 0.0f,
                              .color = kGold,
                              .alpha = 1.0f,
                              .mode = DecalMode::Burst});
        break;
      case EffectKind::HitSpark:
        drawSparks(d, e,
                   SparkStyle{.count = kHitSparks,
                              .reach = 1.2f,
                              .size = 0.12f,
                              .shrink = 0.6f,
                              .rise = 0.2f,
                              .spread = 1.6f,
                              .color = kSpark,
                              .alpha = 1.0f,
                              .mode = DecalMode::Burst});
        break;
      case EffectKind::PickupSparkle:
        drawSparks(d, e,
                   SparkStyle{.count = kPickupTwinkles,
                              .reach = 0.35f,
                              .size = 0.12f,
                              .shrink = 0.7f,
                              .rise = 0.6f,
                              .spread = 0.0f,
                              .color = mix(kGold, kWhite, 0.4f),
                              .alpha = 1.0f,
                              .mode = DecalMode::Burst});
        break;
      case EffectKind::BounceSpark:
        d.billboard(e.position, lerp(0.15f, 0.45f, t), kWhite, 0.8f * fade, DecalMode::Burst, 0.1f);
        break;
      case EffectKind::PowerUpBurst:
        drawSparks(d, e,
                   SparkStyle{.count = kPowerUpSparks,
                              .reach = 1.1f,
                              .size = 0.14f,
                              .shrink = 0.6f,
                              .rise = 0.7f,
                              .spread = 0.0f,
                              .color = e.slot >= 0 ? mix(playerColor(static_cast<uint32_t>(e.slot)),
                                                         kWhite, 0.35f)
                                                   : kWhite,
                              .alpha = 1.0f,
                              .mode = DecalMode::Burst});
        break;
      case EffectKind::ShieldShatter:
        d.billboard(e.position, lerp(0.7f, 1.5f, t), kShieldBlue, 0.8f * fade, DecalMode::Burst,
                    0.3f);
        drawSparks(d, e,
                   SparkStyle{.count = kShieldShards,
                              .reach = 1.0f,
                              .size = 0.12f,
                              .shrink = 0.5f,
                              .rise = 0.3f,
                              .spread = 0.0f,
                              .color = mix(kShieldBlue, kWhite, 0.4f),
                              .alpha = 1.0f,
                              .mode = DecalMode::Burst});
        break;
      case EffectKind::BlinkFlash: {
        const Color c =
            e.slot >= 0 ? mix(playerColor(static_cast<uint32_t>(e.slot)), kWhite, 0.5f) : kWhite;
        d.billboard(e.position, lerp(1.1f, 0.2f, easeOut(t)), c, 0.9f * fade, DecalMode::Burst,
                    0.3f);
        break;
      }
      case EffectKind::ParrySpark:
        d.billboard(e.position, lerp(0.5f, 1.2f, t), kWhite, 0.9f * fade, DecalMode::Burst, 0.2f);
        drawSparks(d, e,
                   SparkStyle{.count = kParrySparks,
                              .reach = 1.3f,
                              .size = 0.13f,
                              .shrink = 0.5f,
                              .rise = 0.2f,
                              .spread = 1.2f,
                              .color = mix(kGold, kWhite, 0.6f),
                              .alpha = 1.0f,
                              .mode = DecalMode::Burst});
        break;
      default:
        break;  // rings and alpha bursts are drawn above; banners are HUD work
    }
  }
}

}  // namespace kfm::presentation
