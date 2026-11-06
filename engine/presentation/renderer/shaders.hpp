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
inline constexpr int kLitFsVec4s = 8;      // color, light, params, box, shade, alt, third, fourth
inline constexpr int kDecalVsVec4s = 4;    // mvp
inline constexpr int kDecalFsVec4s = 2;    // color, shape
inline constexpr int kOutlineVsVec4s = 9;  // viewProj, model, outline
inline constexpr int kOutlineFsVec4s = 1;  // ink
inline constexpr int kGroundVsVec4s = 4;   // mvp
// The ground program (v0.54): 22 vec4s of look. The land, the slow patches
// and the walls are distance fields in a half-float texture (ground_field,
// ground.hpp GroundField) built once per arena and look: a filtered fetch per
// lookup instead of a loop over boxes per pixel.
inline constexpr int kGroundFsVec4s = 22;
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
// fs_params[2] = surface (palette.hpp SurfaceKind: 0 toon, 4 rock, 5 banana,
//                9 planks, 10 lacquer; floors, pits and slow patches are the
//                ground program's since v0.54), flash (0..1 toward white),
//                emissive (adds base colour), unused
// fs_params[3] = walls only: the block's centre (xy = world x, z) and half
//                size (zw), so a wall's pattern follows its own box
// fs_params[4] = shade tint (rgb), cel band edge on n.l (w)
// fs_params[5] = the surface's second colour (rgb: moss, roof, straps,
//                banana tips; toon: a colour to fade toward), walls: the
//                block's height (w), toon: how far to fade (w; an eliminated
//                monkey greys out)
// fs_params[6] = walls: the third colour (rock and crate sides, lacquer posts)
// fs_params[7] = walls: the fourth colour (the medallion, the latch)
// The vertex colour (v_color, v0.49) is the skinned model's COLOR_0 and white
// for static meshes: the base is mix(v_color.rgb, fs_params[0].rgb, v_color.a),
// so a mesh part keeps its own colour where its alpha is 0 and wears the given
// colour (a player's) where it is 1.
inline constexpr const char* kLitVs = KFM_GLSL_HEADER R"(
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
uniform vec4 vs_params[8];
out vec3 v_normal;
out vec2 v_uv;
out vec3 v_world;
out vec4 v_color;
void main() {
  mat4 mvp = mat4(vs_params[0], vs_params[1], vs_params[2], vs_params[3]);
  mat4 model = mat4(vs_params[4], vs_params[5], vs_params[6], vs_params[7]);
  v_normal = transpose(inverse(mat3(model))) * a_normal;
  v_uv = a_uv;
  v_world = (model * vec4(a_position, 1.0)).xyz;
  v_color = vec4(1.0);
  gl_Position = mvp * vec4(a_position, 1.0);
}
)";

inline constexpr const char* kLitFs = KFM_GLSL_HEADER R"(
in vec3 v_normal;
in vec2 v_uv;
in vec3 v_world;
in vec4 v_color;
uniform vec4 fs_params[8];
out vec4 frag_color;
// A hard-edged step, anti-aliased over one pixel of `x`.
float edge(float threshold, float x) {
  float w = max(fwidth(x), 1e-4);
  return smoothstep(threshold - w, threshold + w, x);
}
float sdBox(vec2 p, vec2 b) {
  vec2 q = abs(p) - b;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0));
}
void main() {
  vec3 base = mix(v_color.rgb, fs_params[0].rgb, v_color.a);
  vec3 toLight = fs_params[1].xyz;
  float shade = fs_params[1].w;
  float surface = fs_params[2].x;
  float flash = fs_params[2].y;
  float emissive = fs_params[2].z;
  vec3 tint = fs_params[4].rgb;
  float bandEdge = fs_params[4].w;
  vec3 alt = fs_params[5].rgb;
  vec3 third = fs_params[6].rgb;
  vec3 fourth = fs_params[7].rgb;
  vec3 n = normalize(v_normal);
  vec3 c = base;
  vec2 q = v_world.xz - fs_params[3].xy;  // walls: from the block's centre
  vec2 hb = fs_params[3].zw;
  float height = fs_params[5].w;
  float top = edge(0.62, n.y);  // walls: the top face and the upper bevel
  if (surface < 0.5) {
    // Toon: the base colour, faded toward the second colour by its w.
    c = mix(base, alt, clamp(fs_params[5].w, 0.0, 1.0));
  } else if (surface < 4.5) {
    // Rock (v0.54): a boulder, the lighter top over darker sides, and a
    // moss cap with a soft wavy rim inset from the top's edge.
    float wave = 0.05 * sin(q.x * 3.3 + 1.7 * sin(q.y * 2.1)) + 0.04 * sin(q.y * 4.1 + q.x);
    float cap = sdBox(q - vec2(0.0, 0.06), max(hb - 0.3, vec2(0.05))) + wave - 0.08;
    c = mix(third, base, top);
    c = mix(c, alt, top * (1.0 - edge(0.0, cap)));
  } else if (surface < 5.5) {
    // Banana: brown tips (the mesh's u runs from one tip, 0, to the other, 1).
    c = mix(base, alt, edge(0.84, abs(v_uv.x * 2.0 - 1.0)));
  } else if (surface < 9.5) {
    // Planks (v0.54): crates along the block's long side, each strapped
    // with an X across its top, seams between them down the sides, and a
    // latch near the top of each crate's face toward the camera.
    bool alongX = hb.x >= hb.y;
    float len = alongX ? hb.x : hb.y;
    float wid = alongX ? hb.y : hb.x;
    float along = alongX ? q.x : q.y;
    float across = alongX ? q.y : q.x;
    float crates = max(1.0, floor(2.0 * len / max(2.0 * wid, 1.6) + 0.5));
    float seg = 2.0 * len / crates;
    float u = (fract((along + len) / seg) - 0.5) * seg;
    vec2 hc = max(vec2(0.5 * seg, wid) - 0.16, vec2(0.05));
    vec2 lq = vec2(u, across);
    float d1 = abs(dot(lq, normalize(vec2(hc.y, -hc.x))));
    float d2 = abs(dot(lq, normalize(vec2(hc.y, hc.x))));
    float inside = 1.0 - edge(0.0, sdBox(lq, hc));
    float strap = inside * (1.0 - edge(0.055, min(d1, d2)));
    vec3 topC = mix(base, alt, strap);
    float seam = 1.0 - edge(0.03, abs(abs(u) - 0.5 * seg));
    vec3 sideC = mix(third, third * 0.8, seam);
    float face = edge(0.5, n.z);
    float faceU = alongX ? u + 0.5 * seg : q.x + hb.x;
    vec2 lp = vec2(faceU - 0.26, v_world.y - (height - 0.3));
    float latch = face * (1.0 - edge(0.0, sdBox(lp, vec2(0.07, 0.08))));
    sideC = mix(sideC, fourth, latch);
    c = mix(sideC, topC, top);
  } else if (surface < 10.5) {
    // Lacquer (v0.54): a red shrine, dark posts near the ends of every
    // face, a gold medallion on the faces toward and away from the camera,
    // under a flat jade roof on top.
    bool alongX = abs(n.z) > abs(n.x);
    float s = alongX ? q.x : q.y;
    float hl = alongX ? hb.x : hb.y;
    float inset = min(0.3, 0.3 * hl);
    float post = 1.0 - edge(0.06, abs(abs(s) - (hl - inset)));
    vec3 side = mix(base, third, post);
    float face = edge(0.5, abs(n.z));
    float md = length(vec2(s, v_world.y - 0.55 * height)) - min(0.2, 0.3 * hl);
    side = mix(side, fourth, face * (1.0 - edge(0.0, md)));
    side = mix(side, third, face * (1.0 - edge(0.035, abs(md))));
    c = mix(side, alt, top);
  }
  float lit = edge(bandEdge, dot(n, toLight));
  c = mix(c * shade * tint, c, lit) + base * emissive;
  c = mix(c, vec3(1.0), clamp(flash, 0.0, 1.0));
  frag_color = vec4(c, 1.0);
}
)";

// --- ground: the arena plane in one pass (v0.54, ground.hpp) ----------------------
// One quad at y = 0 reaching kGroundMargin past the arena box; its uv is the
// sim position. Everything is a signed distance in sim units, so every edge
// is anti-aliased over one pixel (`px`, the pixel's footprint on the
// ground) and nothing depends on the camera: the backdrop beyond the
// arena's edge, the pits inside it, the land (the arena minus the pits,
// with a cliff hanging below every edge that faces the camera), the slow
// patches on the land, the walls' soft shadows and the ink lines.
// fs_params[0] = arena half size (xy), corner radius (z), smooth-union k (w)
// fs_params[1] = the field texture's sim origin (xy) and extent (zw)
// fs_params[2] = floor, pit, slow patch style (SurfaceKind), backdrop (BackdropKind)
// fs_params[3] = ink (rgb), ink width (w; 0: no ground ink)
// fs_params[4] = cliff depth, unused, wall shadow spread, opacity
// fs_params[5] = grid cell (0: none), grid opacity, unused, edge opacity
// fs_params[6] = backdrop (rgb), shallows width (w)
// fs_params[7] = backdrop alt (rgb), ripple opacity (w)
// fs_params[8] = decor (rgb), floor scale (w)
// fs_params[9] = decor alt (rgb), floor detail cell (w)
// fs_params[10] = floor (rgb), floor detail density (w)
// fs_params[11] = floor alt (rgb), ripple sway (w: radians per second)
// fs_params[12] = floor detail (rgb), grid line width (w)
// fs_params[13] = cliff (rgb), wall shadow softness (w)
// fs_params[14] = pit (rgb)
// fs_params[15] = pit alt (rgb), mud bubble rate (w: Hz)
// fs_params[16] = shallows (rgb), edge line width (w)
// fs_params[17] = mud (rgb)
// fs_params[18] = mud alt (rgb)
// fs_params[19] = mud detail (rgb)
// fs_params[20] = wall shadow (rgb)
// fs_params[21] = edge line (rgb), time (w: seconds)
// ground_field = the land (r), the slow patches (g), the walls (b) and the
//                walls moved toward the camera (a, their shadow), signed
//                distances in sim units, bilinear, clamped at its edge
inline constexpr const char* kGroundVs = KFM_GLSL_HEADER R"(
layout(location = 0) in vec3 a_position;
layout(location = 2) in vec2 a_uv;
uniform vec4 vs_params[4];
out vec2 v_p;
void main() {
  mat4 mvp = mat4(vs_params[0], vs_params[1], vs_params[2], vs_params[3]);
  v_p = a_uv;
  gl_Position = mvp * vec4(a_position, 1.0);
}
)";

inline constexpr const char* kGroundFs = KFM_GLSL_HEADER R"(
in vec2 v_p;
uniform vec4 fs_params[22];
uniform highp sampler2D ground_field;
out vec4 frag_color;
float px;  // a pixel's footprint on the ground, sim units (set first in main)
// No derivative is taken after px, so the stages below may branch freely.
// An arithmetic hash (no sin: the same pattern on every GPU).
float hash1(vec2 q) {
  vec3 p3 = fract(vec3(q.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
vec2 hash2(vec2 q) {
  return vec2(hash1(q + 3.1), hash1(q + 7.7));
}
// Coverage of a shape at signed distance d (1 inside), over one pixel.
float fill(float d) {
  return clamp(0.5 - d / px, 0.0, 1.0);
}
// Coverage of a line of width w along the zero contour of d.
float stroke(float d, float w) {
  return fill(abs(d) - 0.5 * w);
}
float sdRoundBox(vec2 p, vec2 b, vec4 r) {
  r.xy = (p.x > 0.0) ? r.xy : r.zw;
  r.x = (p.y > 0.0) ? r.x : r.y;
  vec2 q = abs(p) - b + r.x;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r.x;
}
float sdSegment(vec2 p, vec2 a, vec2 b) {
  vec2 pa = p - a;
  vec2 ba = b - a;
  float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
  return length(pa - ba * h);
}
// The distance to an arc bulging toward +y, its chord c on the x axis
// centred on the origin, its sagitta s.
float sdArcUp(vec2 q, float c, float s) {
  float r = (0.25 * c * c + s * s) / (2.0 * s);
  vec2 o = q - vec2(0.0, s - r);
  vec2 sc = vec2(0.5 * c / r, (r - s) / r);
  o.x = abs(o.x);
  return (sc.y * o.x > sc.x * o.y) ? length(o - sc * r) : abs(length(o) - r);
}
// The distance to one period of a sine wave of length len and amplitude
// amp along x, centred on the origin, rising first.
float sdWave(vec2 q, float len, float amp) {
  float k = 6.2831853 / len;
  float x = clamp(q.x, -0.5 * len, 0.5 * len);
  float y = amp * sin(k * (x + 0.5 * len));
  float slope = amp * k * cos(k * (x + 0.5 * len));
  float dy = (q.y - y) / sqrt(1.0 + slope * slope);
  return length(vec2(q.x - x, dy));
}
// A leaf along its local y: a vesica 2b long and 2(r - d) wide.
float sdLeaf(vec2 p, float r, float d) {
  p = abs(p);
  float b = sqrt(r * r - d * d);
  return ((p.y - b) * d > p.x * b) ? length(p - vec2(0.0, b)) : length(p - vec2(-d, 0.0)) - r;
}
float arenaBox(vec2 p) {
  return sdRoundBox(p, fs_params[0].xy, vec4(fs_params[0].z));
}
// The distance fields at p (textureLod: safe in any branch).
vec4 field(vec2 p) {
  return textureLod(ground_field, (p - fs_params[1].xy) / fs_params[1].zw, 0.0);
}
// The land (negative inside).
float land(vec2 p) {
  return field(p).x;
}
float slows(vec2 p) {
  return field(p).y;
}
// One decoration per cell of `cell`: the cell's id (xy) and the offset of p
// from its jittered centre (zw), jittered by up to `room` either way.
vec4 cellAt(vec2 p, vec2 cell, vec2 room, float salt) {
  vec2 id = floor(p / cell) + salt;
  vec2 centre = (floor(p / cell) + 0.5) * cell + (hash2(id) - 0.5) * 2.0 * room;
  return vec4(id, p - centre);
}
// The pit and backdrop decorations' coverage (ripples, cloud arcs, wave
// squiggles), kept clear of the land and, for a pit style, of the backdrop.
float waterMarks(vec2 p, float style, bool backdrop, float time) {
  vec2 cell = style > 10.5 ? vec2(5.0, 3.6) : (style > 7.5 ? vec2(7.0, 4.5) : vec2(3.4, 2.2));
  float reach = style > 10.5 ? 1.95 : (style > 7.5 ? 2.9 : 1.15);
  vec4 c = cellAt(p, cell, vec2(0.5 * cell.x - reach, min(0.5 * cell.y - 0.4, 0.5)), 17.0);
  float h = hash1(c.xy);
  if (h > (style > 7.5 && style < 8.5 ? 0.7 : 0.45)) return 0.0;
  float sway = (style > 7.5 && style < 8.5 ? 0.4 : 0.22) * sin(time * fs_params[11].w + 6.2831853 * h);
  vec2 q = c.zw - vec2(sway, 0.0);
  // Far from the mark: nothing to test (the tests below are the costly part).
  if (abs(q.x) > reach + 0.3 || abs(q.y) > 0.75) return 0.0;
  // Both ends clear of the land and its cliff, and inside the arena unless
  // the backdrop is the same water.
  vec2 e1 = p - c.zw - vec2(reach, 0.0);
  vec2 e2 = p - c.zw + vec2(reach, 0.0);
  vec2 cl = vec2(0.0, fs_params[4].x);
  float clear = min(min(land(e1), land(e2)), min(land(e1 + cl), land(e2 + cl)));
  float keep = step(0.35, clear);
  if (!backdrop) keep *= step(max(arenaBox(e1), arenaBox(e2)), -0.4);
  float d;
  float w;
  if (style > 10.5) {
    d = sdWave(q, 3.2, 0.2);
    w = 0.16;
  } else if (style > 7.5) {
    d = min(sdArcUp(q + vec2(1.2, 0.0), 2.4, 0.42), sdArcUp(q - vec2(1.2, 0.0), 2.4, 0.42));
    w = 0.2;
  } else {
    d = sdArcUp(q, 1.6, 0.18);
    w = 0.16;
  }
  return keep * stroke(d, w);
}
vec3 seaAt(vec2 p) {
  float t = clamp((p.y + fs_params[0].y + 6.0) / (2.0 * fs_params[0].y + 12.0), 0.0, 1.0);
  return mix(fs_params[6].rgb, fs_params[7].rgb, t);
}
void main() {
  vec2 p = v_p;
  px = max(max(length(dFdx(p)), length(dFdy(p))), 1e-4);
  vec2 hs = fs_params[0].xy;
  float time = fs_params[21].w;
  float floorStyle = fs_params[2].x;
  float pitStyle = fs_params[2].y;
  float slowStyle = fs_params[2].z;
  float backStyle = fs_params[2].w;
  vec3 ink = fs_params[3].rgb;
  float inkW = fs_params[3].w;
  float hasInk = step(0.001, inkW);
  float cliffDepth = fs_params[4].x;

  float arena = arenaBox(p);
  vec4 here = field(p);
  float dl = here.x;
  // Only what a pixel can show is worked out: the cliff off the land, the
  // backdrop outside the arena, the water off the land, a floor's and a slow
  // patch's details on them.
  // The land's distance is 1-Lipschitz: past cliffDepth (and an ink line or
  // the shallows) from the land, the land moved up by cliffDepth is clear of p.
  float cliffReach = cliffDepth + max(inkW, fs_params[6].w) + px;
  float dlc = (dl > -px && dl < cliffReach) ? land(p + vec2(0.0, cliffDepth)) : 1e4;
  if (dl <= -px) dlc = -1.0;
  float dm = dl < px ? max(here.y, dl) : 1e4;
  vec2 dw = here.zw;
  bool seaBack = backStyle > 1.5;

  // --- the backdrop ---------------------------------------------------------------
  vec3 c = fs_params[6].rgb;
  float outside = 1.0 - fill(arena);
  if (arena > -px && (backStyle < 0.5 || seaBack)) {
    if (seaBack) {
      c = seaAt(p);
      c = mix(c, fs_params[15].rgb, fs_params[7].w * waterMarks(p, 11.0, true, time));
    }
    // Leaves at the corners, under the arena's edge: two at every corner
    // (canopy), one at two corners (sea).
    float leaves = 0.0;
    float leafInk = 0.0;
    vec3 leafC = fs_params[8].rgb;
    for (int k = 0; k < 4; ++k) {
      vec2 sgn = vec2(k == 0 || k == 3 ? 1.0 : -1.0, k < 2 ? 1.0 : -1.0);
      if (seaBack && k != 0 && k != 2) continue;
      vec2 corner = sgn * hs;
      for (int j = 0; j < 2; ++j) {
        if (seaBack && j == 0) continue;
        // A smaller leaf turned 0.8 rad toward the corner's side edge, then
        // the big one along the corner's diagonal, over it.
        vec2 dir = 0.70710678 * sgn;
        if (j == 0) {
          float turn = 0.7173561 * sgn.x * sgn.y;  // sin(0.8), signed
          dir = vec2(0.6967067 * dir.x - turn * dir.y, turn * dir.x + 0.6967067 * dir.y);
        }
        // Over the open sea a leaf stays clear of the arena (its edge there
        // is only a faint line, so a leaf cut by it would show the cut).
        vec2 at = corner + dir * (j == 0 ? 3.4 : (seaBack ? 6.8 : 4.2));
        if (length(p - at) > 4.5) continue;  // past either leaf's tip
        vec2 lq = vec2(dot(p - at, vec2(-dir.y, dir.x)), dot(p - at, dir));
        float d = j == 0 ? sdLeaf(lq, 4.36, 2.96) : sdLeaf(lq, 5.41, 3.41);
        float cov = fill(d);
        leafC = mix(leafC, j == 0 ? fs_params[9].rgb : fs_params[8].rgb, cov);
        leaves = max(leaves, cov);
        // A leaf's ink: its own outline, over the leaf under it.
        leafInk = max(leafInk * (1.0 - cov), stroke(d, inkW));
      }
    }
    c = mix(c, leafC, leaves);
    c = mix(c, ink, hasInk * leafInk);
  } else if (arena > -px) {
    // A mountain range beyond the far edge, its feet under the arena.
    float x = p.x;
    float ridge = max(3.4 * (1.0 - 2.0 * abs(fract(x / 7.0 + 0.3) - 0.5)),
                      2.2 * (1.0 - 2.0 * abs(fract(x / 4.6 + 0.65) - 0.5)));
    float base = 0.4 * hs.y;
    float m = fill(p.y - (hs.y + 0.6 + ridge)) * fill(base - p.y);
    c = mix(c, mix(c, fs_params[7].rgb, 0.7), m);
  }

  // --- the pits -----------------------------------------------------------------
  if (arena < px && dl > -px) {
    vec3 water = pitStyle > 10.5 ? seaAt(p) : fs_params[14].rgb;
    water = mix(water, fs_params[15].rgb,
                fs_params[7].w * waterMarks(p, pitStyle, seaBack && pitStyle > 10.5, time));
    c = mix(c, water, 1.0 - outside);
  }
  // Shallow water around the land and its cliff.
  float landAndCliff = min(dl, dlc);
  c = mix(c, fs_params[16].rgb, 0.7 * step(0.001, fs_params[6].w) * fill(landAndCliff - fs_params[6].w));

  // --- the cliff under every edge of land that faces the camera --------------------
  c = mix(c, fs_params[13].rgb, fill(dlc));
  c = mix(c, ink, hasInk * stroke(dlc, inkW));

  // --- the land -----------------------------------------------------------------
  vec3 g = fs_params[10].rgb;
  if (dl > px) {
    // Off the land: nothing of it shows.
  } else if (floorStyle < 1.5) {
    // Grass: a few big soft lighter patches and sparse small tufts.
    vec2 cell = vec2(fs_params[8].w, 0.75 * fs_params[8].w);
    vec2 rad = vec2(2.6, 1.0);
    vec4 pc = cellAt(p, cell, max(0.5 * cell - rad - vec2(0.8, 0.3), vec2(0.0)), 5.0);
    vec2 r = rad + vec2(0.8, 0.3) * hash2(pc.xy + 1.3);
    float e = (length(pc.zw / r) - 1.0) * min(r.x, r.y);
    if (e < px && hash1(pc.xy) < 0.5 && land(p - pc.zw) < -(r.y + 0.3))
      g = mix(g, fs_params[11].rgb, fill(e));
    vec2 tcell = vec2(fs_params[9].w);
    vec4 tc = cellAt(p, tcell, 0.5 * tcell - 0.6, 9.0);
    float th = hash1(tc.xy);
    vec2 t = tc.zw;
    float d = min(sdSegment(t, vec2(-0.16, -0.18), vec2(0.0, 0.2)),
                  sdSegment(t, vec2(0.0, 0.2), vec2(0.16, -0.18)));
    if (th < 0.5 * fs_params[10].w) {
      t.x -= 0.32;
      d = min(d, min(sdSegment(t, vec2(-0.14, -0.18), vec2(0.0, 0.26)),
                     sdSegment(t, vec2(0.0, 0.26), vec2(0.14, -0.18))));
    }
    if (d < 0.2 && th < fs_params[10].w && land(p - tc.zw) < -0.6)
      g = mix(g, fs_params[12].rgb, stroke(d, 0.11));
  } else if (floorStyle < 6.5) {
    // Paving: large flagstones (the grid's cells under a grid mode) with
    // grout lines, and the odd crack.
    float gridCell = fs_params[5].x;
    vec2 tiles = max(floor(2.0 * hs / fs_params[8].w + 0.5), vec2(1.0));
    vec2 tile = gridCell > 0.0 ? vec2(gridCell) : 2.0 * hs / tiles;
    vec2 gq = abs(fract((p + hs) / tile + 0.5) - 0.5) * tile;
    g = mix(g, fs_params[11].rgb, stroke(min(gq.x, gq.y), min(0.12, 0.07 * min(tile.x, tile.y))));
    vec2 ccell = vec2(fs_params[9].w);
    vec4 cc = cellAt(p, ccell, 0.5 * ccell - 0.6, 3.0);
    float a = 6.2831853 * hash1(cc.xy + 2.0);
    vec2 dir = 0.26 * vec2(cos(a), sin(a));
    float crack = step(hash1(cc.xy), fs_params[10].w) * stroke(sdSegment(cc.zw, -dir, dir), 0.09);
    g = mix(g, fs_params[12].rgb, 0.6 * crack);
  } else {
    // Sand: bleached toward the middle, the odd shell or pebble.
    float t = clamp(length((p - vec2(0.0, 0.15 * hs.y)) / (1.1 * hs)), 0.0, 1.0);
    g = mix(fs_params[11].rgb, fs_params[10].rgb, smoothstep(0.0, 1.0, t));
    vec2 scell = vec2(fs_params[9].w);
    vec4 sc = cellAt(p, scell, 0.5 * scell - 0.4, 7.0);
    float sh = hash1(sc.xy);
    float dot1 = fill(length(sc.zw) - (sh < 0.5 * fs_params[10].w ? 0.16 : 0.12));
    vec3 dotC = sh < 0.5 * fs_params[10].w ? fs_params[12].rgb : fs_params[15].rgb;
    g = mix(g, dotC, step(sh, fs_params[10].w) * dot1 * (sh < 0.5 * fs_params[10].w ? 1.0 : 0.8));
  }
  float gridCell = fs_params[5].x;
  if (gridCell > 0.0 && (floorStyle < 5.5 || floorStyle > 6.5)) {
    // Grid-space modes: a faint line on every cell edge.
    vec2 gq = abs(fract((p + hs) / gridCell + 0.5) - 0.5) * gridCell;
    float w = max(fs_params[12].w, px);
    g = mix(g, ink, fs_params[5].y * (fs_params[12].w / w) * stroke(min(gq.x, gq.y), w));
  }
  c = mix(c, g, fill(dl));

  // --- the walls' soft shadows -----------------------------------------------------
  float soft = fs_params[13].w;
  float shadow = 1.0 - smoothstep(-soft, soft, dw.y - fs_params[4].z);
  c = mix(c, fs_params[20].rgb, fs_params[4].w * shadow * (1.0 - outside));

  // --- the slow patches ----------------------------------------------------------
  vec3 s = fs_params[17].rgb;
  if (dm > px) {
    // Off the slow patches.
  } else if (slowStyle < 3.5) {
    // Mud: bubble rings that swell and settle.
    vec4 rc = cellAt(p, vec2(1.1), vec2(0.18), 11.0);
    float h = hash1(rc.xy);
    float rr = mix(0.15, 0.27, hash1(rc.xy + 5.0)) *
               (1.0 + 0.12 * sin(6.2831853 * (time * fs_params[15].w + h)));
    float ring = abs(length(rc.zw) - rr);
    if (ring < 0.15 && h < 0.55 && slows(p - rc.zw) < -(rr + 0.2))
      s = mix(s, fs_params[18].rgb, stroke(ring, 0.11));
  } else if (slowStyle < 12.5) {
    // Paddy: rice shoots and the odd ripple.
    vec4 rc = cellAt(p, vec2(0.7, 1.0), vec2(0.15, 0.1), 13.0);
    float h = hash1(rc.xy);
    float lean = 0.08 * (hash1(rc.xy + 4.0) - 0.5);
    float shoot = sdSegment(rc.zw, vec2(-lean, -0.28), vec2(lean, 0.28));
    if (shoot < 0.15 && h < 0.35 && slows(p - rc.zw) < -0.45)
      s = mix(s, fs_params[19].rgb, stroke(shoot, 0.11));
    vec4 ac = cellAt(p, vec2(1.6), vec2(0.3), 19.0);
    float arc = sdArcUp(ac.zw, 0.64, 0.16);
    if (arc < 0.15 && hash1(ac.xy) < 0.3 && slows(p - ac.zw) < -0.6)
      s = mix(s, fs_params[18].rgb, stroke(arc, 0.11));
  } else {
    // Tide pool: shallow water in a sand rim, the odd ripple.
    s = mix(fs_params[18].rgb, s, fill(dm + 0.32));
    vec4 ac = cellAt(p, vec2(1.6), vec2(0.3), 23.0);
    float arc = sdArcUp(ac.zw, 0.6, 0.12);
    if (arc < 0.15 && hash1(ac.xy) < 0.35 && slows(p - ac.zw) < -0.7)
      s = mix(s, fs_params[19].rgb, stroke(arc, 0.1));
  }
  c = mix(c, s, fill(dm));
  c = mix(c, ink, hasInk * stroke(dm, inkW));

  // --- ink: the land's edge, the walls' feet, the arena's edge ---------------------
  c = mix(c, ink, hasInk * stroke(dl, inkW));
  c = mix(c, ink, hasInk * fill(dw.x - 0.5 * inkW));
  c = mix(c, fs_params[21].rgb, fs_params[5].w * stroke(arena, fs_params[16].w));
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
// Vertex: position, normal, four joint indices (floats), weights (sum 1) and
// the colour (rgb, player-colour mask; see kLitFs).
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
layout(location = 4) in vec4 a_color;
uniform vec4 vs_params[72];
out vec3 v_normal;
out vec2 v_uv;
out vec3 v_world;
out vec4 v_color;
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
  v_color = a_color;
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
