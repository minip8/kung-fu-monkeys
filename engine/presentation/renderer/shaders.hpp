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
inline constexpr int kLitVsVec4s = 8;    // mvp, model
inline constexpr int kLitFsVec4s = 4;    // color, light, params, floor
inline constexpr int kDecalVsVec4s = 4;  // mvp
inline constexpr int kDecalFsVec4s = 2;  // color, shape

// --- lit: opaque meshes with a three-band toon ramp ------------------------------
// fs_params[0] = base colour (rgb)
// fs_params[1] = unit direction toward the light (xyz), shade band level (w)
// fs_params[2] = mode (0 toon mesh, 1 arena floor), flash (0..1 toward white),
//                emissive (adds base colour), unused
// fs_params[3] = floor only: arena half extents (xy) in sim units
inline constexpr const char* kLitVs = KFM_GLSL_HEADER R"(
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
uniform vec4 vs_params[8];
out vec3 v_normal;
out vec2 v_uv;
void main() {
  mat4 mvp = mat4(vs_params[0], vs_params[1], vs_params[2], vs_params[3]);
  mat4 model = mat4(vs_params[4], vs_params[5], vs_params[6], vs_params[7]);
  v_normal = transpose(inverse(mat3(model))) * a_normal;
  v_uv = a_uv;
  gl_Position = mvp * vec4(a_position, 1.0);
}
)";

inline constexpr const char* kLitFs = KFM_GLSL_HEADER R"(
in vec3 v_normal;
in vec2 v_uv;
uniform vec4 fs_params[4];
out vec4 frag_color;
void main() {
  vec3 base = fs_params[0].rgb;
  vec3 toLight = fs_params[1].xyz;
  float shade = fs_params[1].w;
  float mode = fs_params[2].x;
  float flash = fs_params[2].y;
  float emissive = fs_params[2].z;
  vec3 n = normalize(v_normal);
  float ndl = dot(n, toLight);
  // Three flat bands (lit, half-lit, shade): a toon look, no gradients.
  float band = ndl > 0.5 ? 1.0 : (ndl > 0.05 ? mix(shade, 1.0, 0.55) : shade);
  vec3 c = base;
  if (mode > 0.5) {
    // Arena floor: a subtle checker of one-unit cells, thin grid lines on the
    // integer sim coordinates (uv = sim x, y) and a darker band along the
    // walls.
    vec2 dist = abs(fract(v_uv + 0.5) - 0.5);
    vec2 w = max(fwidth(v_uv), vec2(1e-4));
    vec2 g = 1.0 - smoothstep(0.5 * w, 1.5 * w, dist);
    float line = max(g.x, g.y);
    vec2 cell = floor(v_uv);
    float checker = mod(cell.x + cell.y, 2.0);
    c = base * (0.97 + 0.05 * checker);
    c = mix(c, base * 0.8, 0.5 * line);
    vec2 edge = fs_params[3].xy - abs(v_uv);
    c *= mix(0.82, 1.0, clamp(min(edge.x, edge.y) / 1.25, 0.0, 1.0));
  }
  c = c * band + base * emissive;
  c = mix(c, vec3(1.0), clamp(flash, 0.0, 1.0));
  frag_color = vec4(c, 1.0);
}
)";

// --- decal: unlit discs (blob shadows, rings, bursts) ----------------------------
// The disc mesh has uv (0.5, 0.5) at the centre and its rim at distance 0.5,
// so r = 2 |uv - 0.5| is 0 at the centre and 1 on the rim.
// fs_params[0] = colour (rgb) and opacity (a)
// fs_params[1] = shape: mode (0 blob shadow, 1 ring, 2 burst), inner radius
//                (ring: where the band starts; burst: where the fade starts)
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
