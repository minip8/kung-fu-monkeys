// The sokol_gfx renderer (architecture §7): placeholder meshes, one
// directional light with two-band cel shading and ink outlines (v0.49, the
// Jungle Toon look), blob-shadow decals and disc-based effects, and
// GPU-skinned characters when a model is loaded. See
// renderer.hpp for the draw order and shaders.hpp for the GLSL and uniform
// layout.
#include "kfm/presentation/renderer.hpp"

#include "kfm/presentation/animation.hpp"
#include "kfm/presentation/builder.hpp"
#include "kfm/presentation/gltf_loader.hpp"
#include "kfm/presentation/ground.hpp"
#include "kfm/presentation/meshes.hpp"
#include "kfm/presentation/palette.hpp"
#include "kfm/presentation/tuning.hpp"
#include "kfm/presentation/world_basis.hpp"
#include "shaders.hpp"

#include <sokol_gfx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace kfm::presentation {

namespace {

// Mesh slots in Renderer::meshes_.
constexpr std::size_t kCapsuleMesh = 0;
constexpr std::size_t kBananaMesh = 1;
constexpr std::size_t kDiscMesh = 2;
constexpr std::size_t kGroundMesh = 3;  // v0.54: the arena plane plus kGroundMargin
constexpr std::size_t kCubeMesh = 4;
constexpr std::size_t kQuadMesh = 5;  // v0.33: a unit floor quad, uv centred on 0 (zones)
constexpr std::size_t kWallMesh = 6;  // v0.54: the arena's wall blocks, one mesh per theme
constexpr std::size_t kSkinnedMesh = kWallMesh + kArenaThemeCount;
// Built in init() from their own data (the walls after them, from the
// arena); the skinned one is loaded later.
constexpr std::size_t kStaticMeshCount = 6;

// Shader and pipeline slots: the static ones first (made in init()), then the
// character's (made in loadCharacterModel()).
constexpr std::size_t kLitShader = 0;
constexpr std::size_t kDecalShader = 1;
constexpr std::size_t kOutlineShader = 2;
constexpr std::size_t kGroundShader = 3;
constexpr std::size_t kSkinnedShader = 4;
constexpr std::size_t kSkinnedOutlineShader = 5;
constexpr std::size_t kStaticShaderCount = 4;
constexpr std::size_t kLitPipeline = 0;
constexpr std::size_t kDecalAlphaPipeline = 1;
constexpr std::size_t kDecalAdditivePipeline = 2;
constexpr std::size_t kOutlinePipeline = 3;
constexpr std::size_t kGroundPipeline = 4;
constexpr std::size_t kSkinnedPipeline = 5;
constexpr std::size_t kSkinnedOutlinePipeline = 6;
constexpr std::size_t kStaticPipelineCount = 5;

// The look's numbers are in tuning.hpp ("the renderer's look").
constexpr Color kGold = kEffectGold;
constexpr Color kWhite = kEffectWhite;

[[nodiscard]] constexpr Color mix(Color a, Color b, float t) {
  return {lerp(a.r, b.r, t), lerp(a.g, b.g, t), lerp(a.b, b.b, t)};
}

// A player's colour whitened by `whiten`, or plain white without a player.
[[nodiscard]] constexpr Color whitened(int slot, float whiten) {
  return slot >= 0 ? mix(playerColor(static_cast<uint32_t>(slot)), kWhite, whiten) : kWhite;
}

// An effect disc's radius at progress `t` (already eased where it eases).
[[nodiscard]] constexpr float discSize(const EffectDisc& disc, float t) {
  return lerp(disc.from, disc.to, t);
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
  Vec4 light;   // toward the light, shade band
  Vec4 params;  // surface, flash, emissive, unused
  Vec4 box;     // walls: the block's centre (world x, z) and half size
  Vec4 shade;   // shade tint, cel band edge
  Vec4 alt;     // the surface's second colour; walls: the height; toon: the fade
  Vec4 third;   // walls: the third colour
  Vec4 fourth;  // walls: the fourth colour
};
static_assert(sizeof(LitFsParams) == static_cast<std::size_t>(shaders::kLitFsVec4s) * 16);

struct SkinnedVsParams {
  Mat4 mvp;
  Mat4 model;
  SkinMatrices joints;
};
static_assert(sizeof(SkinnedVsParams) == static_cast<std::size_t>(shaders::kSkinnedVsVec4s) * 16);

struct OutlineVsParams {
  Mat4 viewProj;
  Mat4 model;
  Vec4 outline;  // width, mode (1 box), unused, unused
};
static_assert(sizeof(OutlineVsParams) == static_cast<std::size_t>(shaders::kOutlineVsVec4s) * 16);

struct OutlineFsParams {
  Vec4 ink;
};
static_assert(sizeof(OutlineFsParams) == static_cast<std::size_t>(shaders::kOutlineFsVec4s) * 16);

struct SkinnedOutlineVsParams {
  Mat4 viewProj;
  Mat4 model;
  SkinMatrices joints;
  Vec4 outline;  // width, unused...
};
static_assert(sizeof(SkinnedOutlineVsParams) ==
              static_cast<std::size_t>(shaders::kSkinnedOutlineVsVec4s) * 16);

struct DecalVsParams {
  Mat4 mvp;
};

struct GroundVsParams {
  Mat4 mvp;
};
static_assert(sizeof(GroundVsParams) == static_cast<std::size_t>(shaders::kGroundVsVec4s) * 16);

// The ground program's look, boxes and walls (shaders.hpp kGroundFs).
struct GroundFsParams {
  std::array<Vec4, shaders::kGroundFsVec4s> v{};
};
static_assert(sizeof(GroundFsParams) == static_cast<std::size_t>(shaders::kGroundFsVec4s) * 16);

[[nodiscard]] constexpr Vec4 rgbw(Color c, float w = 0.0f) {
  return {c.r, c.g, c.b, w};
}

// The ground program's uniforms: the look, where the field texture lies,
// the grid and the time.
[[nodiscard]] GroundFsParams groundParams(const RenderState& state, const ArenaLook& look,
                                          Vec2f fieldOrigin, Vec2f fieldSize) {
  GroundFsParams g;
  auto& v = g.v;
  v[0] = {state.arenaHalfExtents.x, state.arenaHalfExtents.y, look.cornerRadius, 0.0f};
  v[1] = {fieldOrigin.x, fieldOrigin.y, fieldSize.x, fieldSize.y};
  v[2] = {static_cast<float>(look.floorSurface), static_cast<float>(look.pitSurface),
          static_cast<float>(look.slowSurface), static_cast<float>(look.backdrop)};
  v[3] = rgbw(look.ink, look.inkWidth);
  v[4] = {look.cliffDepth, 0.0f, kWallShadowSpread, look.wallShadowAlpha};
  v[5] = {state.floorGridCell, kGridAlpha, 0.0f, look.edgeAlpha};
  v[6] = rgbw(look.background, look.haloWidth);
  v[7] = rgbw(look.backdropAlt, look.pitAltAlpha);
  v[8] = rgbw(look.decor, look.floorScale);
  v[9] = rgbw(look.decorAlt, look.floorDetail);
  v[10] = rgbw(look.floor, look.floorDensity);
  v[11] = rgbw(look.floorAlt, kWaterRippleSpeed);
  v[12] = rgbw(look.floorMark, kGridLineWidth);
  v[13] = rgbw(look.cliff, kWallShadowSoftness);
  v[14] = rgbw(look.pit);
  v[15] = rgbw(look.pitAlt, kMudBubbleHz);
  v[16] = rgbw(look.halo, std::max(look.inkWidth, kEdgeLineMinWidth));
  v[17] = rgbw(look.mud);
  v[18] = rgbw(look.mudAlt);
  v[19] = rgbw(look.mudDetail);
  v[20] = rgbw(look.wallShadow);
  v[21] = rgbw(look.edge, state.time);
  return g;
}

// Every wall of the arena as one rounded block in one mesh, for `look`
// (corner radius and bevel); each block has `perBlock` indices.
[[nodiscard]] MeshData makeWallMesh(const sim::ArenaDef& arena, const ArenaLook& look,
                                    uint32_t& perBlock) {
  MeshData all;
  perBlock = 0;
  for (uint8_t i = 0; i < arena.wallCount && i < sim::kMaxArenaWalls; ++i) {
    const Vec2f lo = toFloat(arena.walls[i].min);
    const Vec2f hi = toFloat(arena.walls[i].max);
    const Vec2f half{0.5f * (hi.x - lo.x), 0.5f * (hi.y - lo.y)};
    const MeshData block = makeRoundedBlock(half.x, half.y, kArenaWallHeight,
                                            wallCornerRadius(look, half), look.wallBevel);
    perBlock = static_cast<uint32_t>(block.indices.size());
    (void)appendMesh(all, block, simToWorld(Vec2f{0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y)}));
  }
  return all;
}
static_assert(sizeof(DecalVsParams) == static_cast<std::size_t>(shaders::kDecalVsVec4s) * 16);

struct DecalFsParams {
  Vec4 color;
  Vec4 shape;  // mode, inner, unused, unused
};
static_assert(sizeof(DecalFsParams) == static_cast<std::size_t>(shaders::kDecalFsVec4s) * 16);

enum class DecalMode : uint8_t { Shadow = 0, Ring = 1, Burst = 2, Rect = 3 };

// The lit program's surfaces (palette.hpp, shaders.hpp kLitFs).
using Surface = SurfaceKind;

// How an outline hull is pushed out (shaders.hpp kOutlineVs).
enum class Hull : uint8_t { Smooth = 0, Box = 1 };

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
                                  const char* label, std::size_t attrCount = 3,
                                  const char* dataTexture = nullptr) {
  sg_shader_desc desc{};
  if (dataTexture != nullptr) {
    // One filtered float texture (v0.54, the ground's distance fields).
    sg_shader_texture_view& tex = desc.views[0].texture;
    tex.stage = SG_SHADERSTAGE_FRAGMENT;
    tex.image_type = SG_IMAGETYPE_2D;
    tex.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
    desc.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    desc.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
    sg_shader_texture_sampler_pair& pair = desc.texture_sampler_pairs[0];
    pair.stage = SG_SHADERSTAGE_FRAGMENT;
    pair.view_slot = 0;
    pair.sampler_slot = 0;
    pair.glsl_name = dataTexture;
  }
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

// Outline is opaque with front faces culled: only the hull's inside shows,
// around the mesh drawn normally.
enum class Blend : uint8_t { Opaque, Alpha, Additive, Outline };

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
  if (blend == Blend::Opaque || blend == Blend::Outline) {
    desc.cull_mode = blend == Blend::Outline ? SG_CULLMODE_FRONT : SG_CULLMODE_BACK;
    desc.depth.write_enabled = true;
  } else {
    // Decals and effects: depth-tested against the opaque scene, never
    // written, pulled slightly toward the camera against z-fighting.
    desc.cull_mode = SG_CULLMODE_NONE;
    desc.depth.write_enabled = false;
    desc.depth.bias = kDecalDepthBias;
    desc.depth.bias_slope_scale = kDecalDepthBias;
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

// The skinned character pipelines: opaque like `lit` (or, for the outline,
// front faces culled), SkinnedVertex layout.
[[nodiscard]] uint32_t makeSkinnedPipeline(uint32_t shader, bool outline, const char* label) {
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
  desc.layout.attrs[4] = {.buffer_index = 0,
                          .offset = static_cast<int>(offsetof(SkinnedVertex, color)),
                          .format = SG_VERTEXFORMAT_FLOAT4};
  desc.index_type = SG_INDEXTYPE_UINT16;
  desc.face_winding = SG_FACEWINDING_CCW;
  desc.cull_mode = outline ? SG_CULLMODE_FRONT : SG_CULLMODE_BACK;
  desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
  desc.depth.write_enabled = true;
  if (outline) {
    // v0.54: the hull is pushed slightly away from the camera, so where it
    // grazes the character's own surface (a headband on the head, an arm
    // against the gi) it stays hidden instead of speckling through.
    desc.depth.bias = kOutlineSkinnedDepthBias;
    desc.depth.bias_slope_scale = kOutlineSkinnedDepthBias;
  }
  desc.label = label;
  return sg_make_pipeline(&desc).id;
}

// Per-frame drawing context: the current pipeline/mesh (to skip redundant
// binds) and the stats. Lives on the stack inside draw().
class FrameDrawer {
 public:
  FrameDrawer(std::span<const uint32_t> pipelines, std::span<const uint32_t> vertexBuffers,
              std::span<const uint32_t> indexBuffers, std::span<const uint32_t> indexCounts,
              const Mat4& viewProj, Vec3 camRight, Vec3 camUp, Vec3 camBack, const ArenaLook& look,
              RendererStats& stats)
      : pipelines_(pipelines),
        vertexBuffers_(vertexBuffers),
        indexBuffers_(indexBuffers),
        indexCounts_(indexCounts),
        viewProj_(viewProj),
        camRight_(camRight),
        camUp_(camUp),
        camBack_(camBack),
        look_(look),
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
    const LitFsParams fs = litFs(color, Surface::Toon, {}, 0.0f, flash, emissive);
    submit(mesh, rangeOf(vs), rangeOf(fs));
  }

  // A patterned surface (water, mud, mossy rock): `alt` is its second colour,
  // `phase` its animation time.
  void surface(std::size_t mesh, const Mat4& model, Color color, Surface kind, Color alt,
               float phase = 0.0f, float emissive = 0.0f) {
    usePipeline(kLitPipeline);
    const LitVsParams vs{.mvp = viewProj_ * model, .model = model};
    const LitFsParams fs = litFs(color, kind, alt, phase, 0.0f, emissive);
    submit(mesh, rangeOf(vs), rangeOf(fs));
  }

  // A skinned character with the lit cel shading (shaders.hpp, kSkinnedVs):
  // `tint` on the parts its vertex colours mask, the whole faded `fade`
  // toward `fadeTarget` (an eliminated monkey greys out, fur and all).
  void skinned(const Mat4& model, const SkinMatrices& joints, Color tint, float flash,
               float emissive, Color fadeTarget, float fade) {
    usePipeline(kSkinnedPipeline);
    const SkinnedVsParams vs{.mvp = viewProj_ * model, .model = model, .joints = joints};
    LitFsParams fs = litFs(tint, Surface::Toon, fadeTarget, 0.0f, flash, emissive);
    fs.alt.w = fade;
    submit(kSkinnedMesh, rangeOf(vs), rangeOf(fs));
  }

  // The skinned character's ink hull, drawn right after the body (the joints
  // are not kept for a later pass).
  void skinnedOutline(const Mat4& model, const SkinMatrices& joints, float width) {
    usePipeline(kSkinnedOutlinePipeline);
    const SkinnedOutlineVsParams vs{.viewProj = viewProj_,
                                    .model = model,
                                    .joints = joints,
                                    .outline = {width, 0.0f, 0.0f, 0.0f}};
    const OutlineFsParams fs{.ink = {look_.ink.r, look_.ink.g, look_.ink.b, 1.0f}};
    submit(kSkinnedMesh, rangeOf(vs), rangeOf(fs));
  }

  // v0.54: the whole arena plane (shaders.hpp kGroundFs), its distance
  // fields in the texture `view` (read through `sampler`).
  void ground(const GroundFsParams& fs, uint32_t view, uint32_t sampler) {
    usePipeline(kGroundPipeline);
    sg_bindings bind{};
    bind.vertex_buffers[0] = sg_buffer{vertexBuffers_[kGroundMesh]};
    bind.index_buffer = sg_buffer{indexBuffers_[kGroundMesh]};
    bind.views[0] = sg_view{view};
    bind.samplers[0] = sg_sampler{sampler};
    sg_apply_bindings(&bind);
    currentMesh_ = kNone;  // the next draw binds its mesh without the texture
    const GroundVsParams vs{.mvp = viewProj_};
    sg_apply_uniforms(0, rangeOf(vs));
    sg_apply_uniforms(1, rangeOf(fs));
    const uint32_t count = indexCounts_[kGroundMesh];
    sg_draw(0, static_cast<int>(count), 1);
    ++stats_.drawCalls;
    stats_.triangles += count / 3U;
  }

  // v0.54: one wall block (`count` indices from `first` of a wall mesh,
  // already in world space) patterned after its box: `center` on the
  // ground, `size` its world extents.
  void wall(std::size_t mesh, uint32_t first, uint32_t count, Vec3 center, Vec3 size) {
    usePipeline(kLitPipeline);
    const Mat4 model = Mat4::identity();
    const LitVsParams vs{.mvp = viewProj_, .model = model};
    LitFsParams fs = litFs(look_.wall, look_.wallSurface, look_.wallAlt, 0.0f, 0.0f, 0.0f);
    fs.box = {center.x, center.z, 0.5f * size.x, 0.5f * size.z};
    fs.alt.w = size.y;
    fs.third = {look_.wallSide.r, look_.wallSide.g, look_.wallSide.b, 0.0f};
    fs.fourth = {look_.wallDetail.r, look_.wallDetail.g, look_.wallDetail.b, 0.0f};
    submit(mesh, rangeOf(vs), rangeOf(fs), first, count);
  }

  // Queues an ink hull around a static mesh (or `count` of its indices from
  // `first`; 0 = all); flushOutlines() draws the queue in one pipeline run.
  // A full queue drops the rest (counted in the stats as nothing: the frame
  // just has fewer outlines).
  void outline(std::size_t mesh, const Mat4& model, float width, Hull hull, uint32_t first = 0,
               uint32_t count = 0) {
    if (outlineCount_ >= outlines_.size()) return;
    outlines_[outlineCount_++] = OutlineItem{
        .model = model, .mesh = mesh, .width = width, .hull = hull, .first = first, .count = count};
  }

  void flushOutlines() {
    if (outlineCount_ == 0) return;
    usePipeline(kOutlinePipeline);
    const OutlineFsParams fs{.ink = {look_.ink.r, look_.ink.g, look_.ink.b, 1.0f}};
    for (const OutlineItem& o : std::span(outlines_).first(outlineCount_)) {
      const OutlineVsParams vs{.viewProj = viewProj_,
                               .model = o.model,
                               .outline = {o.width, o.hull == Hull::Box ? 1.0f : 0.0f, 0.0f, 0.0f}};
      submit(o.mesh, rangeOf(vs), rangeOf(fs), o.first, o.count);
    }
    outlineCount_ = 0;
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
  // Walls (a tilemap arena has up to kMaxArenaWalls), the rim, players,
  // bananas, pickups: 160 covers a full 10-player tilemap match.
  static constexpr std::size_t kMaxOutlines = 160;

  struct OutlineItem {
    Mat4 model;
    std::size_t mesh{0};
    float width{0.0f};
    Hull hull{Hull::Smooth};
    uint32_t first{0};
    uint32_t count{0};  // 0: the whole mesh
  };

  [[nodiscard]] LitFsParams litFs(Color color, Surface kind, Color alt, float phase, float flash,
                                  float emissive) {
    const Vec3 l = normalize(kToLight);
    return LitFsParams{
        .color = {color.r, color.g, color.b, 1.0f},
        .light = {l.x, l.y, l.z, kShadeBand},
        .params = {static_cast<float>(kind), flash, emissive, phase},
        .box = {},
        .shade = {look_.shadeTint.r, look_.shadeTint.g, look_.shadeTint.b, kCelBandEdge},
        .alt = {alt.r, alt.g, alt.b, 0.0f},
        .third = {},
        .fourth = {}};
  }

  void decal(const Mat4& model, Color color, float alpha, DecalMode mode, float inner) {
    if (alpha <= 0.0f) return;
    const DecalVsParams vs{.mvp = viewProj_ * model};
    const DecalFsParams fs{.color = {color.r, color.g, color.b, clamp01(alpha)},
                           .shape = {static_cast<float>(mode), inner, 0.0f, 0.0f}};
    submit(kDiscMesh, rangeOf(vs), rangeOf(fs));
  }

  void submit(std::size_t mesh, const sg_range& vs, const sg_range& fs, uint32_t first = 0,
              uint32_t count = 0) {
    if (mesh != currentMesh_) {
      sg_bindings bind{};
      bind.vertex_buffers[0] = sg_buffer{vertexBuffers_[mesh]};
      bind.index_buffer = sg_buffer{indexBuffers_[mesh]};
      sg_apply_bindings(&bind);
      currentMesh_ = mesh;
    }
    sg_apply_uniforms(0, &vs);
    sg_apply_uniforms(1, &fs);
    if (count == 0) {
      first = 0;
      count = indexCounts_[mesh];
    }
    sg_draw(static_cast<int>(first), static_cast<int>(count), 1);
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
  const ArenaLook& look_;
  RendererStats& stats_;
  std::size_t currentPipeline_{kNone};
  std::size_t currentMesh_{kNone};
  std::array<OutlineItem, kMaxOutlines> outlines_{};
  std::size_t outlineCount_{0};
};

// `count` billboards flying out from an effect's position (tuning.hpp
// SparkLook): around the full circle, or within `spread` radians of the
// effect's direction when it has one. The slot rotates a full circle's
// pattern so overlapping bursts differ.
void drawSparks(FrameDrawer& d, const RenderEffect& e, int count, const SparkLook& s, Color color) {
  const float t = e.progress();
  const float fade = 1.0f - t;
  const bool cone = length(e.direction) > 1e-4f;
  const float base = cone ? std::atan2(e.direction.y, e.direction.x)
                          : kSparkSlotTurn * static_cast<float>(e.slot + 2);
  const float reach = s.reach * easeOut(t);
  for (int k = 0; k < count; ++k) {
    const float u = count > 1 ? static_cast<float>(k) / static_cast<float>(count - 1) : 0.5f;
    const float a = cone ? base + (s.spread * (u - 0.5f))
                         : base + (kTwoPi * static_cast<float>(k) / static_cast<float>(count));
    // Alternate spark lengths a little so the burst is not a perfect circle.
    const float r = reach * ((k % 2 == 0) ? 1.0f : kSparkAltReach);
    const Vec3 at =
        e.position + (planeToWorld({std::cos(a), std::sin(a)}) * r) + Vec3{0.0f, s.rise * t, 0.0f};
    d.billboard(at, s.size * (1.0f - (s.shrink * t)), color, s.alpha * fade, DecalMode::Burst,
                kSparkInner);
  }
}

// A power-up pickup (§5.8): the def's shape in its palette colour, spinning
// and glowing a little, from the placeholder meshes. playerRadius and
// playerHeight are the capsule mesh's dimensions (orb and coin are scaled
// capsules).
void drawPowerUp(FrameDrawer& d, const RenderPickup& p, float playerRadius, float playerHeight,
                 float time) {
  const Color c = playerColor(p.colorIndex);
  const float glow =
      kPowerUpGlow + (kPowerUpGlowSwing * std::sin(kTwoPi * kPowerUpRingPulseHz * time));
  const float s = kPowerUpSize;
  const Vec3 at = p.position + Vec3{0.0f, kPowerUpHeight - kPickupHeight, 0.0f};
  const Mat4 base = translate(at) * rotateY(p.spin);
  const float rx = playerRadius > 0.0f ? (0.5f * s) / playerRadius : 1.0f;
  const float hy = playerHeight > 0.0f ? 1.0f / playerHeight : 1.0f;
  switch (p.shape) {
    case PowerUpShapeView::Orb: {
      // The capsule squashed to its own width: a ball centred on `at`.
      const Mat4 m = base * translate({0.0f, -0.5f * s, 0.0f}) * scale({rx, s * hy, rx});
      d.lit(kCapsuleMesh, m, c, 0.0f, glow);
      d.outline(kCapsuleMesh, m, kOutlinePowerUp, Hull::Smooth);
      break;
    }
    case PowerUpShapeView::Cube: {
      const float k = kPowerUpCubeScale * s;
      const Mat4 m = base * rotateX(kPowerUpCubeTilt) * scale({k, k, k});
      d.lit(kCubeMesh, m, c, 0.0f, glow);
      d.outline(kCubeMesh, m, kOutlinePowerUp, Hull::Box);
      break;
    }
    case PowerUpShapeView::Diamond: {
      // The cube stood on a corner.
      const float k = kPowerUpDiamondScale * s;
      const Mat4 m = base * rotateZ(0.25f * kPi) * rotateX(0.25f * kPi) * scale({k, k, k});
      d.lit(kCubeMesh, m, c, 0.0f, glow);
      d.outline(kCubeMesh, m, kOutlinePowerUp, Hull::Box);
      break;
    }
    case PowerUpShapeView::Coin: {
      // A flat capsule stood on its edge.
      const Mat4 m = base * rotateX(0.5f * kPi) *
                     translate({0.0f, -0.5f * kPowerUpCoinThickness * s, 0.0f}) *
                     scale({rx, kPowerUpCoinThickness * s * hy, rx});
      d.lit(kCapsuleMesh, m, c, 0.0f, glow);
      d.outline(kCapsuleMesh, m, kOutlinePowerUp, Hull::Smooth);
      break;
    }
  }
}

// v0.82: a tree (RenderTree): a trunk at the back of its cell, a canopy of
// three balls round its top, and the next fruit ripening under the canopy's
// front. playerRadius and playerHeight are the capsule mesh's dimensions
// (the canopy balls are scaled capsules, as a power-up orb is).
void drawTree(FrameDrawer& d, const RenderTree& t, float playerRadius, float playerHeight) {
  const float c = t.cell;
  const Vec3 back{0.0f, 0.0f, -kTreeTrunkBack * c};  // world -z is sim +y: away from the camera
  const Vec3 root = t.ground + back;
  const float trunkW = kTreeTrunkWidth * c;
  const float trunkH = kTreeTrunkHeight * c;
  const Mat4 trunk =
      translate(root + Vec3{0.0f, 0.5f * trunkH, 0.0f}) * scale({trunkW, trunkH, trunkW});
  d.lit(kCubeMesh, trunk, kTreeTrunk);
  d.outline(kCubeMesh, trunk, kOutlinePowerUp, Hull::Box);
  const float size = kTreeCanopySize * c;
  const float rx = playerRadius > 0.0f ? (0.5f * size) / playerRadius : 1.0f;
  const float hy = playerHeight > 0.0f ? size / playerHeight : 1.0f;
  const float spread = kTreeCanopySpread * c;
  const Vec3 top = root + Vec3{0.0f, trunkH, 0.0f};
  // Left, right and a raised middle one, the middle a lighter green.
  const std::array<Vec3, 3> balls = {Vec3{-spread, 0.0f, 0.0f}, Vec3{spread, 0.0f, 0.0f},
                                     Vec3{0.0f, 0.6f * spread, -0.3f * spread}};
  for (std::size_t k = 0; k < balls.size(); ++k) {
    const Mat4 m = translate(top + balls[k] + Vec3{0.0f, -0.5f * size, 0.0f}) * scale({rx, hy, rx});
    d.lit(kCapsuleMesh, m, k == 2 ? kTreeLeafAlt : kTreeLeaf);
    d.outline(kCapsuleMesh, m, kOutlinePowerUp, Hull::Smooth);
  }
  if (t.ripeness > 0.0f) {
    // The next fruit, hanging in front of the canopy, growing toward the beat.
    const float k = kHeldBananaScale * kTreeFruitScale * t.ripeness;
    const Vec3 at = top + Vec3{0.0f, -(0.5f * size) - kTreeFruitDrop * c, 0.5f * size};
    const Mat4 m = translate(at) * rotateZ(-0.5f * kPi) * scale({k, k, k});
    d.surface(kBananaMesh, m, kBananaColor, Surface::Banana, kBananaTip, 0.0f, kHeldBananaGlow);
    d.outline(kBananaMesh, m, kOutlineBanana, Hull::Smooth);
  }
}

}  // namespace

bool Renderer::init(const sim::MatchContent& match) {
  const sim::ContentDB& content = match.db;
  if (initialised_) shutdown();
  arenaName_ = match.arena.name;
  theme_ = resolveTheme(themeChoice_, arenaName_.view());
  if (!sg_isvalid()) return false;

  const std::array<MeshData, kStaticMeshCount> data = {
      makeMonkeyCapsule(content.player),
      makeBananaMesh(content.boomerang),
      makeDisc(1.0f, 40),
      makeFloorQuad(match.arena.halfExtents.x.toFloat() + kGroundMargin,
                    match.arena.halfExtents.y.toFloat() + kGroundMargin),
      makeUnitCube(),
      makeFloorQuad(0.5f, 0.5f),
  };
  constexpr std::array<const char*, kStaticMeshCount> kLabels = {
      "kfm-capsule", "kfm-banana", "kfm-disc", "kfm-ground", "kfm-cube", "kfm-quad"};
  const auto upload = [&](std::size_t slot, const MeshData& m, const char* label) {
    meshes_[slot].vertexBuffer =
        makeBuffer(m.vertices.data(), m.vertices.size() * sizeof(MeshVertex), false, label);
    meshes_[slot].indexBuffer =
        makeBuffer(m.indices.data(), m.indices.size() * sizeof(uint16_t), true, label);
    meshes_[slot].indexCount = static_cast<uint32_t>(m.indices.size());
  };
  for (std::size_t i = 0; i < kStaticMeshCount; ++i)
    upload(i, data[i], kLabels[i]);
  // v0.54: the wall blocks, in every theme's shape (a theme switch only
  // picks another mesh). An arena without walls has none to upload.
  const bool hasWalls = match.arena.wallCount > 0;
  for (std::size_t t = 0; t < kArenaThemeCount; ++t) {
    wallIndexCount_[t] = 0;
    if (!hasWalls) continue;
    const MeshData walls =
        makeWallMesh(match.arena, arenaLook(static_cast<ArenaTheme>(t)), wallIndexCount_[t]);
    upload(kWallMesh + t, walls, "kfm-walls");
  }

  shaders_[kLitShader] = makeShader(shaders::kLitVs, shaders::kLitFs, shaders::kLitVsVec4s,
                                    shaders::kLitFsVec4s, "kfm-lit-shader");
  shaders_[kDecalShader] = makeShader(shaders::kDecalVs, shaders::kDecalFs, shaders::kDecalVsVec4s,
                                      shaders::kDecalFsVec4s, "kfm-decal-shader");
  shaders_[kOutlineShader] =
      makeShader(shaders::kOutlineVs, shaders::kOutlineFs, shaders::kOutlineVsVec4s,
                 shaders::kOutlineFsVec4s, "kfm-outline-shader");
  shaders_[kGroundShader] =
      makeShader(shaders::kGroundVs, shaders::kGroundFs, shaders::kGroundVsVec4s,
                 shaders::kGroundFsVec4s, "kfm-ground-shader", 3, "ground_field");
  pipelines_[kLitPipeline] = makePipeline(shaders_[kLitShader], Blend::Opaque, "kfm-lit");
  pipelines_[kGroundPipeline] = makePipeline(shaders_[kGroundShader], Blend::Opaque, "kfm-ground");
  pipelines_[kDecalAlphaPipeline] =
      makePipeline(shaders_[kDecalShader], Blend::Alpha, "kfm-decal-alpha");
  pipelines_[kDecalAdditivePipeline] =
      makePipeline(shaders_[kDecalShader], Blend::Additive, "kfm-decal-additive");
  pipelines_[kOutlinePipeline] =
      makePipeline(shaders_[kOutlineShader], Blend::Outline, "kfm-outline");

  // v0.54: the ground's distance fields, one texture per theme (their
  // corners round differently), bilinear, clamped at the edge.
  {
    auto pieces = std::make_unique<RenderState>();
    buildArenaPieces(match.arena, *pieces);
    pieces->arenaHalfExtents = toFloat(match.arena.halfExtents);
    sg_sampler_desc sampler{};
    sampler.min_filter = SG_FILTER_LINEAR;
    sampler.mag_filter = SG_FILTER_LINEAR;
    sampler.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    sampler.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    sampler.label = "kfm-ground-sampler";
    groundSampler_ = sg_make_sampler(&sampler).id;
    for (std::size_t t = 0; t < kArenaThemeCount; ++t) {
      const ArenaLook& look = arenaLook(static_cast<ArenaTheme>(t));
      const GroundField field = buildGroundField(buildGroundLayout(*pieces, look), look);
      groundFieldOrigin_ = field.origin;
      groundFieldSize_ = field.size;
      sg_image_desc image{};
      image.type = SG_IMAGETYPE_2D;
      image.width = field.width;
      image.height = field.height;
      image.pixel_format = SG_PIXELFORMAT_RGBA16F;
      image.data.mip_levels[0] =
          sg_range{field.texels.data(), field.texels.size() * sizeof(uint16_t)};
      image.label = "kfm-ground-field";
      groundImages_[t] = sg_make_image(&image).id;
      sg_view_desc view{};
      view.texture.image = sg_image{groundImages_[t]};
      view.label = "kfm-ground-field";
      groundViews_[t] = sg_make_view(&view).id;
    }
  }

  bool ok = true;
  ok = ok && sg_query_sampler_state(sg_sampler{groundSampler_}) == SG_RESOURCESTATE_VALID;
  for (std::size_t t = 0; t < kArenaThemeCount; ++t) {
    ok = ok && sg_query_image_state(sg_image{groundImages_[t]}) == SG_RESOURCESTATE_VALID &&
         sg_query_view_state(sg_view{groundViews_[t]}) == SG_RESOURCESTATE_VALID;
  }
  for (std::size_t i = 0; i < kSkinnedMesh; ++i) {
    const GpuMesh& m = meshes_[i];
    if (i >= kWallMesh && !hasWalls) continue;
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

void Renderer::setThemeChoice(ThemeChoice choice) {
  themeChoice_ = choice;
  theme_ = resolveTheme(choice, arenaName_.view());
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
  for (std::size_t t = 0; t < kArenaThemeCount; ++t) {
    sg_destroy_view(sg_view{groundViews_[t]});
    sg_destroy_image(sg_image{groundImages_[t]});
  }
  sg_destroy_sampler(sg_sampler{groundSampler_});
  pipelines_ = {};
  shaders_ = {};
  meshes_ = {};
  groundViews_ = {};
  groundImages_ = {};
  groundSampler_ = 0;
}

void Renderer::destroyCharacterModel() {
  if (sg_isvalid()) {
    sg_destroy_pipeline(sg_pipeline{pipelines_[kSkinnedPipeline]});
    sg_destroy_pipeline(sg_pipeline{pipelines_[kSkinnedOutlinePipeline]});
    sg_destroy_shader(sg_shader{shaders_[kSkinnedShader]});
    sg_destroy_shader(sg_shader{shaders_[kSkinnedOutlineShader]});
    sg_destroy_buffer(sg_buffer{meshes_[kSkinnedMesh].vertexBuffer});
    sg_destroy_buffer(sg_buffer{meshes_[kSkinnedMesh].indexBuffer});
  }
  pipelines_[kSkinnedPipeline] = 0;
  pipelines_[kSkinnedOutlinePipeline] = 0;
  shaders_[kSkinnedShader] = 0;
  shaders_[kSkinnedOutlineShader] = 0;
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
                 shaders::kLitFsVec4s, "kfm-skinned-shader", 5);
  pipelines_[kSkinnedPipeline] =
      makeSkinnedPipeline(shaders_[kSkinnedShader], false, "kfm-skinned");
  // The outline's fragment stage is kOutlineFs; its vertex stage reads
  // position, normal, joints and weights (the colour is bound, unread).
  shaders_[kSkinnedOutlineShader] =
      makeShader(shaders::kSkinnedOutlineVs, shaders::kOutlineFs, shaders::kSkinnedOutlineVsVec4s,
                 shaders::kOutlineFsVec4s, "kfm-skinned-outline-shader", 5);
  pipelines_[kSkinnedOutlinePipeline] =
      makeSkinnedPipeline(shaders_[kSkinnedOutlineShader], true, "kfm-skinned-outline");

  const bool ok =
      sg_query_buffer_state(sg_buffer{mesh.vertexBuffer}) == SG_RESOURCESTATE_VALID &&
      sg_query_buffer_state(sg_buffer{mesh.indexBuffer}) == SG_RESOURCESTATE_VALID &&
      sg_query_shader_state(sg_shader{shaders_[kSkinnedShader]}) == SG_RESOURCESTATE_VALID &&
      sg_query_pipeline_state(sg_pipeline{pipelines_[kSkinnedPipeline]}) ==
          SG_RESOURCESTATE_VALID &&
      sg_query_shader_state(sg_shader{shaders_[kSkinnedOutlineShader]}) == SG_RESOURCESTATE_VALID &&
      sg_query_pipeline_state(sg_pipeline{pipelines_[kSkinnedOutlinePipeline]}) ==
          SG_RESOURCESTATE_VALID;
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

  // Screen shake moves the eye and the target together, until it is below a
  // pixel (visibleShakeOffset).
  const Vec3 shake = visibleShakeOffset(camera, state.shakeAmplitude, state.shakeOffset);
  const Mat4 view = lookAt(camera.eye + shake, camera.target + shake, Vec3{0.0f, 1.0f, 0.0f});
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
  const ArenaLook& look = arenaLook(theme_);
  FrameDrawer d(pipelines_, vbufs, ibufs, counts, viewProj, camRight, camUp, camBack, look, stats_);

  const float r = state.playerRadius;
  const float bodyHeight = state.playerHeight;

  // --- opaque ------------------------------------------------------------------
  // v0.54: the ground program draws the backdrop, the pits, the land and its
  // cliffs, the slow patches, the walls' shadows and every ground ink line in
  // one pass, so nothing on the ground is a slab laid over the floor.
  {
    const auto t = static_cast<std::size_t>(theme_);
    d.ground(groundParams(state, look, groundFieldOrigin_, groundFieldSize_), groundViews_[t],
             groundSampler_);
  }
  // Wall blocks: rounded rock, shrines or crates (the theme's mesh), each
  // patterned after its own box, with an ink hull. Blocks are in content
  // order, as the arena pieces' walls are.
  {
    const auto themeIndex = static_cast<std::size_t>(theme_);
    const uint32_t perBlock = wallIndexCount_[themeIndex];
    const std::size_t wallMesh = kWallMesh + themeIndex;
    uint32_t block = 0;
    for (uint16_t i = 0; i < state.arenaPieceCount && perBlock > 0; ++i) {
      const RenderArenaPiece& piece = state.arenaPieces[i];
      if (piece.kind != ArenaPieceKind::Wall) continue;
      if (block * perBlock >= meshes_[wallMesh].indexCount) break;
      const uint32_t first = block * perBlock;
      d.wall(wallMesh, first, perBlock, piece.center, piece.size);
      if (look.wallInk)
        d.outline(wallMesh, Mat4::identity(), kOutlineWall, Hull::Smooth, first, perBlock);
      ++block;
    }
  }

  // v0.82: the arena's trees, after the walls (they stand like them).
  for (uint8_t i = 0; i < state.treeCount; ++i)
    drawTree(d, state.trees[i], state.playerRadius, state.playerHeight);

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

  const float blink = std::fmod(state.time * kInvulnerableBlinkHz, 1.0f) < 0.5f ? 1.0f : 0.0f;
  std::array<Vec3, kMaxRenderPlayers> handAt{};  // v0.53: per RenderState player index
  std::array<bool, kMaxRenderPlayers> hasHand{};
  for (uint16_t i = 0; i < state.playerCount; ++i) {
    const RenderPlayer& p = state.players[i];
    if (!p.visible) continue;
    const bool eliminated = p.has(PlayerFlag::Eliminated);
    const bool falling = p.has(PlayerFlag::Falling);  // v0.57
    const Color own = playerColor(p.colorIndex);
    const Color fadeTo = falling ? kFallShade : kEliminatedColor;
    float fade = falling ? kFallDarken * p.fall : 0.0f;
    if (eliminated) fade = kEliminatedFade;
    const Color body = mix(own, fadeTo, fade);
    float flash = p.flash;
    if (p.has(PlayerFlag::Invulnerable)) flash = std::max(flash, kInvulnerableFlash * blink);
    const float emissive = p.has(PlayerFlag::Dashing) ? kDashEmissive : 0.0f;
    // v0.57: a falling monkey also tips about its side axis (RenderPlayer::tilt).
    const Mat4 placed =
        translate(p.position) * rotateY(p.bodyYaw) * rotateZ(p.tilt) * scale(p.scale);
    if (skinnedBodies && p.slot < anim_.size()) {
      SkinMatrices joints;
      SkinMatrices globals;
      evaluateAnimState(model_, clips_, anim_[p.slot], joints, &globals);
      const Mat4 model = placed * scale({modelScale, modelScale, modelScale});
      d.skinned(model, joints, own, flash, emissive, fadeTo, fade);
      d.skinnedOutline(model, joints, kOutlineMonkey);
      // v0.53: the held banana sits in the animated hand (wound up while a
      // throw charges, shaking with the strain).
      if (clips_.hand >= 0 && static_cast<std::size_t>(clips_.hand) < globals.size()) {
        handAt[i] = transformPoint(model * globals[static_cast<std::size_t>(clips_.hand)],
                                   Vec3{0.0f, 0.0f, 0.0f});
        hasHand[i] = true;
      }
    } else {
      d.lit(kCapsuleMesh, placed, body, flash, emissive);
      d.outline(kCapsuleMesh, placed, kOutlineMonkey, Hull::Smooth);
    }
  }

  // Aim indicators and bananas in hand, after all the bodies so the pipeline
  // changes once. hasHand[i]: player i's hand position from its skinned pose.
  for (uint16_t i = 0; i < state.playerCount; ++i) {
    const RenderPlayer& p = state.players[i];
    if (!p.visible || p.has(PlayerFlag::Eliminated)) continue;
    const Color own = playerColor(p.colorIndex);
    // v0.57: a falling monkey aims at nothing; a banana in its hand goes
    // down the pit with it (drawn below, in the sinking hand).
    const bool falling = p.has(PlayerFlag::Falling);

    // Aim indicator: a thin bar from the capsule surface along the aim. A
    // charging throw (v0.48) stretches it and warms it toward gold.
    const Vec3 aimBase = p.position + Vec3{0.0f, bodyHeight * kAimHeightFraction, 0.0f};
    const float charge = p.has(PlayerFlag::Charging) ? p.throwCharge : 0.0f;
    const float aimLength =
        p.has(PlayerFlag::Charging) ? lerp(kAimLength, kChargeAimLength, charge) : kAimLength;
    if (!falling) {
      d.lit(kCubeMesh,
            translate(aimBase) * rotateY(p.aimYaw) *
                translate({r + (aimLength * 0.5f), 0.0f, 0.0f}) *
                scale({aimLength, kAimThickness, kAimThickness}),
            mix(mix(own, kWhite, kAimWhiten), kGold, charge), 0.0f,
            kAimGlow + (kAimChargeGlow * charge));
    }
    // v0.33: a capture flag's carrier wears a small gold diamond overhead.
    if (p.has(PlayerFlag::Carrying)) {
      const float s = kFlagCarrySize;
      const Mat4 m = translate(p.position + Vec3{0.0f, bodyHeight + kFlagCarryLift, 0.0f}) *
                     rotateY(state.time * kFlagCarrySpinRadPerSec) * rotateZ(0.25f * kPi) *
                     rotateX(0.25f * kPi) * scale({s, s, s});
      d.lit(kCubeMesh, m, kGold, 0.0f, kFlagCarryGlow);
      d.outline(kCubeMesh, m, kOutlinePowerUp, Hull::Box);
    }
    // A banana in hand while the player has ammo: in the skinned hand when
    // there is one (v0.53), else beside the capsule along the aim. v0.82:
    // the cargo, several held bananas fan out from the grip as a bunch (up
    // to kHeldBunchShown), each a little higher, so the count reads.
    if (p.has(PlayerFlag::HasAmmo)) {
      const Mat4 at = hasHand[i]
                          ? translate(handAt[i]) * rotateY(p.aimYaw)
                          : translate(aimBase) * rotateY(p.aimYaw) *
                                translate({r * kHeldBananaForward, 0.0f, r * kHeldBananaSide});
      // v0.57: shrinking with a falling body.
      const float held = kHeldBananaScale * (falling ? lerp(1.0f, kFallEndScale, p.fall) : 1.0f);
      // The bunch turns about the bananas' shared stem end (the mesh's chord
      // runs along z, state.bananaRadius each side of its centre).
      const float half = state.bananaRadius * held;
      const int shown = std::clamp<int>(p.ammo, 1, kHeldBunchShown);
      for (int k = 0; k < shown; ++k) {
        const float fan =
            kHeldBunchFan * (static_cast<float>(k) - (0.5f * static_cast<float>(shown - 1)));
        const Mat4 m = at * translate({0.0f, kHeldBunchLift * r * static_cast<float>(k), -half}) *
                       rotateY(fan) * translate({0.0f, 0.0f, half}) * scale({held, held, held});
        d.surface(kBananaMesh, m, kBananaColor, Surface::Banana, kBananaTip, 0.0f, kHeldBananaGlow);
        d.outline(kBananaMesh, m, kOutlineBanana, Hull::Smooth);
      }
    }
  }

  for (uint16_t i = 0; i < state.bananaCount; ++i) {
    const RenderBanana& b = state.bananas[i];
    Color c = kBananaColor;
    if (b.ownerSlot >= 0) c = mix(c, playerColor(static_cast<uint32_t>(b.ownerSlot)), kOwnerTint);
    const Mat4 m = translate(b.position) * rotateY(b.headingYaw + b.spin);
    d.surface(kBananaMesh, m, c, Surface::Banana, kBananaTip, 0.0f, kFlyingBananaGlow);
    d.outline(kBananaMesh, m, kOutlineBanana, Hull::Smooth);
  }
  for (uint16_t i = 0; i < state.pickupCount; ++i) {
    const RenderPickup& p = state.pickups[i];
    if (p.kind == PickupKindView::PowerUp || p.kind == PickupKindView::Flag) {
      drawPowerUp(d, p, r, bodyHeight, state.time);  // a flag: its team's colour (v0.33)
    } else {
      // v0.57: a stacked pickup shows up to kPickupStackShown bananas, each
      // a little higher and turned.
      const int shown = std::clamp<int>(p.count, 1, kPickupStackShown);
      for (int k = 0; k < shown; ++k) {
        const float fk = static_cast<float>(k);
        const Mat4 m = translate(p.position + Vec3{0.0f, kPickupStackLift * fk, 0.0f}) *
                       rotateY(p.spin + (kPickupStackTurn * fk));
        d.surface(kBananaMesh, m, kPickupColor, Surface::Banana, kBananaTip, 0.0f,
                  kPickupBananaGlow);
        d.outline(kBananaMesh, m, kOutlineBanana, Hull::Smooth);
      }
    }
  }
  d.flushOutlines();

  // --- ground decals (alpha) -------------------------------------------------------
  d.usePipeline(kDecalAlphaPipeline);
  // Objective zones (v0.33) under everything else on the floor.
  for (uint8_t i = 0; i < state.zoneCount; ++i) {
    const RenderZone& z = state.zones[i];
    const Color c = z.team >= 0 ? playerColor(static_cast<uint32_t>(z.team)) : kWhite;
    d.groundRect(z.center, kZoneLift, z.size.x, z.size.z, c, kZoneRimOpacity, kZoneRimStart,
                 kZoneOpacity / kZoneRimOpacity);
    // v0.80, control: the capture under way grows from the centre in the
    // challenger's colour.
    if (z.challenger >= 0 && z.capture > 0.0f) {
      d.groundRect(z.center, kZoneCaptureLift, z.size.x * z.capture, z.size.z * z.capture,
                   playerColor(static_cast<uint32_t>(z.challenger)), kZoneCaptureRimOpacity,
                   kZoneCaptureRimStart, kZoneCaptureOpacity / kZoneCaptureRimOpacity);
    }
  }
  // v0.82: a tree's shade over the cell its fruit is laid on, and a ring
  // round it in banana yellow marking the spot.
  for (uint8_t i = 0; i < state.treeCount; ++i) {
    const RenderTree& t = state.trees[i];
    const float shade = kTreeShadeRadius * t.cell;
    d.groundDisc(t.ground, kShadowLift, 0.0f, shade, shade, look.shadow, kTreeShadeOpacity,
                 DecalMode::Shadow);
    d.groundDisc(t.ground, kDecalLift, 0.0f, shade, shade, kTreeRing, kTreeRingAlpha,
                 DecalMode::Ring, kTreeRingInner);
  }
  for (uint16_t i = 0; i < state.shadowCount; ++i) {
    const RenderShadow& s = state.shadows[i];
    d.groundDisc(s.ground, kShadowLift, 0.0f, s.radius, s.radius, look.shadow,
                 kShadowOpacity * s.opacity, DecalMode::Shadow);
  }
  for (uint16_t i = 0; i < state.playerCount; ++i) {
    const RenderPlayer& p = state.players[i];
    if (!p.visible || p.has(PlayerFlag::Eliminated)) continue;
    const Color own = playerColor(p.colorIndex);
    if (p.has(PlayerFlag::Dashing)) {
      // A stretched trail behind the dash.
      const Vec3 behind = p.ground - (yawDirection(p.bodyYaw) * (r * kDashStreakBehind));
      d.groundDisc(behind, kDecalLift, p.bodyYaw, r * kDashStreakAlong, r * kDashStreakAcross,
                   mix(own, kWhite, kDashStreakWhiten), kDashStreakAlpha, DecalMode::Burst,
                   kDashStreakInner);
    }
    if (p.has(PlayerFlag::PerfectDash)) {
      // The catch window: a bright ring around the feet while it is open.
      const EffectDisc& k = kPerfectDashRing;
      d.groundDisc(p.ground, kDecalLift, 0.0f, r * k.from, r * k.to, kGold, k.alpha,
                   DecalMode::Ring, k.inner);
    }
    if (p.has(PlayerFlag::Shielded)) {
      const EffectDisc& k = kShieldRing;
      d.groundDisc(p.ground, kDecalLift, 0.0f, r * k.from, r * k.to, kShieldBlue, k.alpha,
                   DecalMode::Ring, k.inner);
    }
  }
  // A pulsing ring in the power-up's colour marks a power-up pickup.
  for (uint16_t i = 0; i < state.pickupCount; ++i) {
    const RenderPickup& p = state.pickups[i];
    if (p.kind == PickupKindView::Banana) continue;
    const float pulse = 0.5f + (0.5f * std::sin(kTwoPi * kPowerUpRingPulseHz * state.time));
    d.groundDisc(p.ground, kDecalLift, 0.0f, kPowerUpRingRadius, kPowerUpRingRadius,
                 playerColor(p.colorIndex), lerp(kPowerUpRingAlphaMin, kPowerUpRingAlphaMax, pulse),
                 DecalMode::Ring, kPowerUpRingInner);
  }
  // A ring on the floor at `size`, `color` at the disc's alpha × `fade`.
  const auto ring = [&d](const RenderEffect& e, float size, Color color, const EffectDisc& k,
                         float fade) {
    d.groundDisc(e.position, kDecalLift, 0.0f, size, size, color, k.alpha * fade, DecalMode::Ring,
                 k.inner);
  };
  // A billboard at `at`, `size`, `color` at the disc's alpha × `fade`.
  const auto flare = [&d](Vec3 at, float size, Color color, const EffectDisc& k, float fade,
                          DecalMode mode = DecalMode::Burst) {
    d.billboard(at, size, color, k.alpha * fade, mode, k.inner);
  };
  for (uint16_t i = 0; i < state.effectCount; ++i) {
    const RenderEffect& e = state.effects[i];
    const float t = e.progress();
    const float fade = 1.0f - t;
    switch (e.kind) {
      case EffectKind::DashRing:
        ring(e, r * discSize(kDashRingDisc, t), kWhite, kDashRingDisc, fade);
        break;
      case EffectKind::PerfectCatchFlash:
        ring(e, r * discSize(kPerfectCatchRing, t), kGold, kPerfectCatchRing, fade);
        break;
      case EffectKind::EliminationBurst: {
        // Alpha-blended so the player's colour stays saturated over the floor.
        const Color c = e.slot >= 0 ? playerColor(static_cast<uint32_t>(e.slot)) : kWhite;
        ring(e, r * discSize(kEliminationRing, t), c, kEliminationRing, fade);
        flare(e.position, discSize(kEliminationFlash, t), c, kEliminationFlash, fade);
        break;
      }
      case EffectKind::HitFlash:
        // Alpha-blended: additive red over a grey floor would read as pink.
        flare(e.position, discSize(kHitFlashDisc, t), kHitRed, kHitFlashDisc, fade);
        break;
      case EffectKind::DashAnticipation: {
        // A ring collapsing onto the dasher: the dash reads as deliberate at
        // the press, whatever it resolves to later (§12).
        const Color c = whitened(e.slot, kDashAnticipationWhiten);
        ring(e, r * discSize(kDashAnticipationRing, easeOut(t)), c, kDashAnticipationRing, fade);
        break;
      }
      case EffectKind::PitFall: {
        // v0.57: two rings closing in over the pit where the monkey went
        // down, the inner one a beat behind.
        const Color c = whitened(e.slot, kPitFallWhiten);
        // `size` set: a faller's banana dropping in (smaller rings).
        const float base = e.size > 0.0f ? e.size : r;
        ring(e, base * discSize(kPitFallRing, easeOut(t)), c, kPitFallRing, fade);
        const float inner =
            base * discSize(kPitFallInnerRing, easeOut(clamp01(t * kPitFallInnerLead)));
        ring(e, inner, kWhite, kPitFallInnerRing, fade);
        break;
      }
      case EffectKind::DashTrail:
        flare(e.position, r * discSize(kDashTrailDisc, t), whitened(e.slot, kDashTrailWhiten),
              kDashTrailDisc, fade);
        break;
      case EffectKind::EliminationPoof:
        drawSparks(d, e, kEliminationPuffs, kEliminationPuffLook, kSmokeColor);
        break;
      case EffectKind::PowerUpBurst: {
        const Color c = e.slot >= 0 ? playerColor(static_cast<uint32_t>(e.slot)) : kWhite;
        ring(e, discSize(kPowerUpBurstRing, easeOut(t)), c, kPowerUpBurstRing, fade);
        break;
      }
      case EffectKind::ShieldShatter:
        ring(e, r * discSize(kShieldShatterRing, easeOut(t)), kShieldBlue, kShieldShatterRing,
             fade);
        break;
      case EffectKind::GroundDust:
        drawSparks(d, e, kDustPuffs, kDustPuffLook, kDustColor);
        break;
      case EffectKind::RespawnBurst: {
        // A ring collapsing onto the spawn point and a glow rising out of it.
        const Color c = whitened(e.slot, kRespawnWhiten);
        ring(e, r * discSize(kRespawnRing, easeOut(t)), c, kRespawnRing, fade);
        flare(e.position + Vec3{0.0f, lerp(kRespawnGlowRiseFrom, kRespawnGlowRiseTo, t), 0.0f},
              r * discSize(kRespawnGlow, t), c, kRespawnGlow, fade);
        break;
      }
      case EffectKind::ScorePop:
        ring(e, r * discSize(kScorePopRing, easeOut(t)), kGold, kScorePopRing, fade);
        break;
      case EffectKind::ZoneCaptured:
        // v0.80: a ring in the new owner's colour racing out over the zone.
        ring(e, std::max(e.size, r) * discSize(kZoneCapturedRing, easeOut(t)),
             whitened(e.slot, kZoneCapturedWhiten), kZoneCapturedRing, fade);
        break;
      case EffectKind::BananaStolen: {
        // v0.82: two rings closing in over the robbed home, the inner a beat
        // behind, in the raider's team colour: the opposite of a capture's.
        const float size = std::max(e.size, r);
        const Color c = whitened(e.slot, kBananaStolenWhiten);
        ring(e, size * discSize(kBananaStolenRing, easeOut(t)), c, kBananaStolenRing, fade);
        const float inner =
            size * discSize(kBananaStolenInnerRing, easeOut(clamp01(t * kBananaStolenInnerLead)));
        ring(e, inner, c, kBananaStolenInnerRing, fade);
        break;
      }
      case EffectKind::SlamShockwave:
        // A ring racing out to the reach: what it touches is pushed.
        ring(e, std::max(e.size, r) * discSize(kSlamRing, easeOut(t)),
             whitened(e.slot, kSlamWhiten), kSlamRing, fade);
        break;
      case EffectKind::ParryGuard:
        ring(e, r * discSize(kParryGuardRing, easeOut(t)), whitened(e.slot, kParryGuardWhiten),
             kParryGuardRing, fade);
        break;
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
        const EffectDisc& k = kCallShoutRing;
        flare(e.position, lerp(k.from, k.to + (kCallShoutRingPerSize * e.size), easeOut(t)),
              whitened(e.slot, kCallShoutWhiten), k, fade, DecalMode::Ring);
        flare(e.position, discSize(kCallShoutCore, easeOut(t)), kWhite, kCallShoutCore, fade);
        break;
      }
      case EffectKind::PerfectCatchFlash:
        flare(e.position, discSize(kPerfectCatchGlow, t), kGold, kPerfectCatchGlow, fade);
        flare(e.position, discSize(kPerfectCatchCore, t), kWhite, kPerfectCatchCore, fade);
        break;
      case EffectKind::AutoCatchFlash:
        flare(e.position, discSize(kAutoCatchDisc, t), kWhite, kAutoCatchDisc, fade);
        break;
      case EffectKind::ThrowPuff:
        flare(e.position, discSize(kThrowPuffDisc, t), kWhite, kThrowPuffDisc, fade);
        break;
      case EffectKind::DashAnticipation: {
        // A flash just ahead of the dasher, along the dash.
        const Vec3 ahead = e.position + (planeToWorld(e.direction) * (r * kDashAnticipationAhead)) +
                           Vec3{0.0f, kEffectBodyHeight * kDashAnticipationFlashHeight, 0.0f};
        flare(ahead, discSize(kDashAnticipationFlash, t), kWhite, kDashAnticipationFlash, fade);
        break;
      }
      case EffectKind::BananaTrail: {
        Color c = kBananaColor;
        if (e.slot >= 0) c = mix(c, playerColor(static_cast<uint32_t>(e.slot)), kOwnerTint);
        flare(e.position, discSize(kBananaTrailDisc, t), c, kBananaTrailDisc, fade);
        break;
      }
      case EffectKind::PerfectCatchBurst:
        drawSparks(d, e, kPerfectCatchSparks, kPerfectCatchSparkLook, kGold);
        break;
      case EffectKind::HitSpark:
        drawSparks(d, e, kHitSparks, kHitSparkLook, kSparkColor);
        break;
      case EffectKind::PickupSparkle:
        drawSparks(d, e, kPickupTwinkles, kPickupTwinkleLook,
                   mix(kGold, kWhite, kPickupTwinkleWhiten));
        break;
      case EffectKind::BounceSpark:
        flare(e.position, discSize(kBounceSparkDisc, t), kWhite, kBounceSparkDisc, fade);
        break;
      case EffectKind::PowerUpBurst:
        drawSparks(d, e, kPowerUpSparks, kPowerUpSparkLook, whitened(e.slot, kPowerUpSparkWhiten));
        break;
      case EffectKind::ShieldShatter:
        flare(e.position, discSize(kShieldShatterFlash, t), kShieldBlue, kShieldShatterFlash, fade);
        drawSparks(d, e, kShieldShards, kShieldShardLook,
                   mix(kShieldBlue, kWhite, kShieldShardWhiten));
        break;
      case EffectKind::BlinkFlash:
        flare(e.position, discSize(kBlinkFlashDisc, easeOut(t)), whitened(e.slot, kBlinkWhiten),
              kBlinkFlashDisc, fade);
        break;
      case EffectKind::StealFlash:
        // v0.82: the raider lights up in its team colour as the banana jumps
        // into its hand.
        flare(e.position, discSize(kStealFlashDisc, easeOut(t)),
              whitened(e.slot, kStealFlashWhiten), kStealFlashDisc, fade);
        break;
      case EffectKind::ParrySpark:
        flare(e.position, discSize(kParrySparkFlash, t), kWhite, kParrySparkFlash, fade);
        drawSparks(d, e, kParrySparks, kParrySparkLook, mix(kGold, kWhite, kParrySparkWhiten));
        break;
      default:
        break;  // rings and alpha bursts are drawn above; banners are HUD work
    }
  }
}

}  // namespace kfm::presentation
