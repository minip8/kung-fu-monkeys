// Hand-written GLSL for the M2 renderer (architecture §7). GL backends only:
// one source per stage, prefixed at compile time with the version line for
// the active sokol backend:
//   SOKOL_GLCORE -> "#version 330 core"   (desktop GL 4.3 / 4.1 contexts)
//   SOKOL_GLES3  -> "#version 300 es" + precision qualifiers (WebGL2)
// The bodies use only the common subset of GLSL 3.30 and GLSL ES 3.00
// (layout(location) inputs, one `out vec4`, fwidth, inverse, transpose).
//
// Uniform blocks are flat vec4 arrays (what sokol-shdc emits for GL): their
// std140 layout equals a packed float[4 * N] on the C++ side, so there is no
// vec3 padding to get wrong. Matrices travel as four column vec4s.
#pragma once

#include "kfm/presentation/skinned_model.hpp"

namespace kfm::presentation::shaders {

#ifdef SOKOL_GLES3
#define KFM_GLSL_HEADER "#version 300 es\nprecision highp float;\nprecision highp int;\n"
#else
#ifndef SOKOL_GLCORE
#error "kfm_renderer supports the sokol GL backends only (SOKOL_GLCORE, SOKOL_GLES3)"
#endif
#define KFM_GLSL_HEADER "#version 330 core\n"
#endif

// vec4 counts of each uniform block (the C++ structs in renderer.cpp match).
inline constexpr int kLitVsVec4s = 8;      // mvp, model
inline constexpr int kLitFsVec4s = 6;      // color, light, params, floor, shade, alt
inline constexpr int kDecalVsVec4s = 4;    // mvp
inline constexpr int kDecalFsVec4s = 2;    // color, shape
inline constexpr int kOutlineVsVec4s = 9;  // viewProj, model, outline
inline constexpr int kOutlineFsVec4s = 1;  // ink
// mvp, model, then 4 column vec4s per joint. With kMaxSkinJoints = 16 that is
// 72 vec4s: WebGL2 guarantees at least 256 vertex-shader vec4 uniforms
// (GL_MAX_VERTEX_UNIFORM_VECTORS), so the budget leaves room for 2.5× the
// joints before anything else is needed (dual quaternions, a texture).
inline constexpr int kSkinnedVsVec4s = 72;
static_assert(kSkinnedVsVec4s == 8 + (4 * static_cast<int>(kMaxSkinJoints)));
static_assert(kSkinnedVsVec4s <= 256);
// The skinned outline: viewProj, model, the joints, then the outline vec4.
inline constexpr int kSkinnedOutlineVsVec4s = kSkinnedVsVec4s + 1;
static_assert(kSkinnedOutlineVsVec4s <= 256);

// --- lit: opaque meshes, two-band cel shading (v0.49) -----------------------------
// fs_params[0] = base colour (rgb)
// fs_params[1] = unit direction toward the light (xyz), shade band level (w)
// fs_params[2] = surface (0 toon, 1 grass floor, 2 water, 3 mud, 4 mossy rock),
//                flash (0..1 toward white), emissive (adds base colour), the
//                surface's animation phase (seconds × its speed)
// fs_params[3] = floor only: arena half extents (xy) in sim units; grass only:
//                patch scale (z), tuft cells per unit (w)
// fs_params[4] = shade tint (rgb), cel band edge on n.l (w)
// fs_params[5] = the surface's second colour (rgb: grass patches, foam, mud
//                bubbles, moss), grass only: tuft density (w); tufts are the
//                base colour darkened
// Surfaces read the world position (v_world), so patterns run continuously
// across pieces and never stretch with a box's scale.
inline constexpr const char* kLitVs = KFM_GLSL_HEADER R"(
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
uniform vec4 vs_params[8];
out vec3 v_normal;
out vec2 v_uv;
out vec3 v_world;
void main() {
  mat4 mvp = mat4(vs_params[0], vs_params[1], vs_params[2], vs_params[3]);
  mat4 model = mat4(vs_params[4], vs_params[5], vs_params[6], vs_params[7]);
  v_normal = transpose(inverse(mat3(model))) * a_normal;
  v_uv = a_uv;
  v_world = (model * vec4(a_position, 1.0)).xyz;
  gl_Position = mvp * vec4(a_position, 1.0);
}
)";

inline constexpr const char* kLitFs = KFM_GLSL_HEADER R"(
in vec3 v_normal;
in vec2 v_uv;
in vec3 v_world;
uniform vec4 fs_params[6];
out vec4 frag_color;
float hash(vec2 p) {
  return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float noise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x),
             mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}
// A hard-edged step, anti-aliased over one pixel of `x`.
float edge(float threshold, float x) {
  float w = max(fwidth(x), 1e-4);
  return smoothstep(threshold - w, threshold + w, x);
}
void main() {
  vec3 base = fs_params[0].rgb;
  vec3 toLight = fs_params[1].xyz;
  float shade = fs_params[1].w;
  float surface = fs_params[2].x;
  float flash = fs_params[2].y;
  float emissive = fs_params[2].z;
  float time = fs_params[2].w;
  vec3 tint = fs_params[4].rgb;
  float bandEdge = fs_params[4].w;
  vec3 alt = fs_params[5].rgb;
  vec3 n = normalize(v_normal);
  vec3 c = base;
  vec2 p = v_world.xz;
  if (surface < 0.5) {
    // Toon: the base colour alone.
  } else if (surface < 1.5) {
    // Grass: flat two-tone blotches, small darker tufts, a faint unit grid on
    // the sim coordinates (uv) and a darker band along the walls.
    c = mix(base, alt, edge(0.56, noise(p * fs_params[3].z)));
    vec2 tc = p * fs_params[3].w;
    vec2 cell = floor(tc);
    vec2 spot = vec2(hash(cell + 3.1), hash(cell + 7.7)) * 0.6 + 0.2;
    float tuft = step(1.0 - fs_params[5].w, hash(cell)) *
                 (1.0 - edge(0.09, length(fract(tc) - spot)));
    c = mix(c, base * 0.78, tuft);
    vec2 dist = abs(fract(v_uv + 0.5) - 0.5);
    vec2 w = max(fwidth(v_uv), vec2(1e-4));
    vec2 g = 1.0 - smoothstep(0.5 * w, 1.5 * w, dist);
    c = mix(c, c * 0.88, 0.5 * max(g.x, g.y));
    vec2 wall = fs_params[3].xy - abs(v_uv);
    c *= mix(0.8, 1.0, clamp(min(wall.x, wall.y) / 1.25, 0.0, 1.0));
  } else if (surface < 2.5) {
    // Water: darker deep patches, then thin wavy crest lines that drift and
    // break up (a cartoon river, not a texture).
    float t = time;
    c = mix(base, base * 0.85, edge(0.55, noise(p * 0.45 - 0.08 * t)));
    float wave = sin(dot(p, vec2(0.9, 0.45)) * 2.6 + 2.4 * noise(p * 0.6 + 0.12 * t) + t);
    float crest = edge(0.9, wave) * edge(0.5, noise(p * 1.1 - 0.2 * t));
    c = mix(c, alt, 0.8 * crest);
  } else if (surface < 3.5) {
    // Mud: darker blotches and bubble rings that swell and fade.
    c = mix(base, base * 0.82, edge(0.5, noise(p * 1.3)));
    vec2 q = p * 1.4;
    vec2 cell = floor(q);
    float h = hash(cell);
    float phase = fract(time + h);
    vec2 centre = cell + 0.3 + 0.4 * vec2(hash(cell + 3.1), hash(cell + 7.7));
    float d = length(q - centre);
    float ring = 1.0 - edge(0.05, abs(d - (0.06 + 0.22 * phase)));
    c = mix(c, alt, ring * (1.0 - phase) * step(0.45, h));
  } else if (surface < 4.5) {
    // Mossy rock: moss blotches over the top faces.
    float top = edge(0.7, n.y);
    c = mix(base, alt, top * edge(0.38, noise(p * 1.6)));
  }
  float lit = edge(bandEdge, dot(n, toLight));
  c = mix(c * shade * tint, c, lit) + base * emissive;
  c = mix(c, vec3(1.0), clamp(flash, 0.0, 1.0));
  frag_color = vec4(c, 1.0);
}
)";

// --- outline: ink hulls (v0.49) ---------------------------------------------------
// The mesh drawn again with front faces culled, every vertex pushed out in
// world space by the outline width: along the normal for smooth meshes, or
// for boxes (mode 1) along each of the box's axes by the sign of the vertex,
// so the hull is the box grown by the width on every side with closed
// corners (flat cube normals would split them).
// vs_params[0..3] = viewProj, [4..7] = model, [8] = width (x), mode (y)
// fs_params[0] = ink colour (rgb)
inline constexpr const char* kOutlineVs = KFM_GLSL_HEADER R"(
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
uniform vec4 vs_params[9];
void main() {
  mat4 viewProj = mat4(vs_params[0], vs_params[1], vs_params[2], vs_params[3]);
  mat4 model = mat4(vs_params[4], vs_params[5], vs_params[6], vs_params[7]);
  float width = vs_params[8].x;
  vec4 world = model * vec4(a_position, 1.0);
  vec3 dir;
  if (vs_params[8].y > 0.5) {
    mat3 m = mat3(model);
    dir = normalize(m[0]) * sign(a_position.x) + normalize(m[1]) * sign(a_position.y) +
          normalize(m[2]) * sign(a_position.z);
  } else {
    dir = normalize(transpose(inverse(mat3(model))) * a_normal);
  }
  world.xyz += dir * width;
  gl_Position = viewProj * world;
}
)";

inline constexpr const char* kOutlineFs = KFM_GLSL_HEADER R"(
uniform vec4 fs_params[1];
out vec4 frag_color;
void main() {
  frag_color = vec4(fs_params[0].rgb, 1.0);
}
)";

// --- skinned: GPU-skinned characters with the lit program's cel shading -------
// Vertex: position, normal, four joint indices (floats) and weights (sum 1).
// The skin matrix is the weighted sum of the joint matrices (linear blend
// skinning); normals go through its upper 3×3 (joint transforms are rigid),
// then through the model's normal matrix (the model matrix carries the
// per-player scale, squash and stretch). The fragment stage is kLitFs with
// mode 0, so colour, flash and emissive work exactly as for the capsule.
// vs_params[0..3] = mvp, [4..7] = model, [8 + 4j .. 11 + 4j] = joint j's skin
// matrix (global × inverse bind).
inline constexpr const char* kSkinnedVs = KFM_GLSL_HEADER R"(
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_joints;
layout(location = 3) in vec4 a_weights;
uniform vec4 vs_params[72];
out vec3 v_normal;
out vec2 v_uv;
out vec3 v_world;
mat4 jointMatrix(float joint) {
  int i = 8 + 4 * int(joint + 0.5);
  return mat4(vs_params[i], vs_params[i + 1], vs_params[i + 2], vs_params[i + 3]);
}
void main() {
  mat4 mvp = mat4(vs_params[0], vs_params[1], vs_params[2], vs_params[3]);
  mat4 model = mat4(vs_params[4], vs_params[5], vs_params[6], vs_params[7]);
  mat4 skin = a_weights.x * jointMatrix(a_joints.x) + a_weights.y * jointMatrix(a_joints.y) +
              a_weights.z * jointMatrix(a_joints.z) + a_weights.w * jointMatrix(a_joints.w);
  vec4 skinned = skin * vec4(a_position, 1.0);
  vec3 n = mat3(skin) * a_normal;
  v_normal = transpose(inverse(mat3(model))) * n;
  v_uv = vec2(0.0);
  v_world = (model * skinned).xyz;
  gl_Position = mvp * skinned;
}
)";

// The skinned character's ink hull: kSkinnedVs's skinning, then the smooth
// outline push of kOutlineVs. vs_params[0..3] = viewProj, [4..7] = model,
// [8 .. 71] = the joints, [72] = width (x).
inline constexpr const char* kSkinnedOutlineVs = KFM_GLSL_HEADER R"(
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_joints;
layout(location = 3) in vec4 a_weights;
uniform vec4 vs_params[73];
mat4 jointMatrix(float joint) {
  int i = 8 + 4 * int(joint + 0.5);
  return mat4(vs_params[i], vs_params[i + 1], vs_params[i + 2], vs_params[i + 3]);
}
void main() {
  mat4 viewProj = mat4(vs_params[0], vs_params[1], vs_params[2], vs_params[3]);
  mat4 model = mat4(vs_params[4], vs_params[5], vs_params[6], vs_params[7]);
  mat4 skin = a_weights.x * jointMatrix(a_joints.x) + a_weights.y * jointMatrix(a_joints.y) +
              a_weights.z * jointMatrix(a_joints.z) + a_weights.w * jointMatrix(a_joints.w);
  vec4 world = model * (skin * vec4(a_position, 1.0));
  vec3 n = normalize(transpose(inverse(mat3(model))) * (mat3(skin) * a_normal));
  world.xyz += n * vs_params[72].x;
  gl_Position = viewProj * world;
}
)";

// --- decal: unlit discs (blob shadows, rings, bursts) ----------------------------
// The disc mesh has uv (0.5, 0.5) at the centre and its rim at distance 0.5,
// so r = 2 |uv - 0.5| is 0 at the centre and 1 on the rim.
// fs_params[0] = colour (rgb) and opacity (a)
// fs_params[1] = shape: mode (0 blob shadow, 1 ring, 2 burst, 3 rect), inner
//                radius (ring: where the band starts; burst: where the fade
//                starts; rect: where the rim band starts), and for a rect the
//                fill's opacity relative to the rim's
// A rect (v0.33, zones) is drawn on the quad mesh, whose uv is centred on 0
// with the edges at ±0.5: d = 2 max(|uv|) is 1 on the edge.
inline constexpr const char* kDecalVs = KFM_GLSL_HEADER R"(
layout(location = 0) in vec3 a_position;
layout(location = 2) in vec2 a_uv;
uniform vec4 vs_params[4];
out vec2 v_uv;
void main() {
  mat4 mvp = mat4(vs_params[0], vs_params[1], vs_params[2], vs_params[3]);
  v_uv = a_uv;
  gl_Position = mvp * vec4(a_position, 1.0);
}
)";

inline constexpr const char* kDecalFs = KFM_GLSL_HEADER R"(
in vec2 v_uv;
uniform vec4 fs_params[2];
out vec4 frag_color;
void main() {
  vec4 color = fs_params[0];
  float mode = fs_params[1].x;
  float inner = fs_params[1].y;
  if (mode > 2.5) {
    vec2 q = abs(v_uv) * 2.0;
    float d = max(q.x, q.y);
    float aaR = max(fwidth(d), 1e-4) * 1.5;
    float edge = 1.0 - smoothstep(1.0 - aaR, 1.0, d);
    float ar = edge * mix(fs_params[1].z, 1.0, smoothstep(inner - aaR, inner, d));
    frag_color = vec4(color.rgb, color.a * ar);
    return;
  }
  float r = 2.0 * length(v_uv - vec2(0.5));
  float aa = max(fwidth(r), 1e-4) * 1.5;
  float outer = 1.0 - smoothstep(1.0 - aa, 1.0, r);
  float a;
  if (mode < 0.5) {
    // Blob shadow: solid with a darker rim, so the hitbox edge reads clearly.
    a = outer * mix(0.72, 1.0, smoothstep(0.62, 0.9, r));
  } else if (mode < 1.5) {
    a = outer * smoothstep(inner - aa, inner, r);
  } else {
    a = outer * (1.0 - smoothstep(inner, 1.0, r));
  }
  frag_color = vec4(color.rgb, color.a * a);
}
)";

#undef KFM_GLSL_HEADER

}  // namespace kfm::presentation::shaders
