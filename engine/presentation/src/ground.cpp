#include "kfm/presentation/ground.hpp"

#include "kfm/presentation/tuning.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace kfm::presentation {
namespace {

// Corners and edges closer than this touch.
constexpr float kTouch = 1e-3f;
// How far a bridge reaches into each of the two boxes it joins, at most
// (deeper than any ink line, cliff or shallows band drawn from the distance).
constexpr float kBridgeReach = 1.5f;
// Without the land's boxes (landFromPits), a pit against the arena's edge is
// carried on this far past it, so the land's distance just outside the edge
// sees the pit, not a sliver of land.
constexpr float kPitOverhang = 3.0f;
// The land's cells: the arena's edges and every pit's, along each axis.
constexpr std::size_t kMaxCuts = 2 + (2 * sim::kMaxArenaHazards);

// The corner signs in GroundBox::radii order.
constexpr std::array<Vec2f, 4> kCornerSigns = {Vec2f{1.0f, 1.0f}, Vec2f{1.0f, -1.0f},
                                               Vec2f{-1.0f, 1.0f}, Vec2f{-1.0f, -1.0f}};

[[nodiscard]] GroundBox boxOf(float lox, float loy, float hix, float hiy) {
  return GroundBox{.center = {0.5f * (lox + hix), 0.5f * (loy + hiy)},
                   .half = {0.5f * (hix - lox), 0.5f * (hiy - loy)},
                   .radii = {},
                   .group = 0};
}

[[nodiscard]] bool contains(const GroundBox& b, Vec2f p) {
  return std::fabs(p.x - b.center.x) <= b.half.x + kTouch &&
         std::fabs(p.y - b.center.y) <= b.half.y + kTouch;
}

// Overlapping, or sharing a stretch of edge (not just a corner).
[[nodiscard]] bool touches(const GroundBox& a, const GroundBox& b) {
  const float ox = std::min(a.center.x + a.half.x, b.center.x + b.half.x) -
                   std::max(a.center.x - a.half.x, b.center.x - b.half.x);
  const float oy = std::min(a.center.y + a.half.y, b.center.y + b.half.y) -
                   std::max(a.center.y - a.half.y, b.center.y - b.half.y);
  return ox >= -kTouch && oy >= -kTouch && (ox > kTouch || oy > kTouch);
}

// Rounds the free corners of every box by `radius` (capped at half its
// shorter side): a corner inside another box stays square, and so does one
// on the arena's edge when `squareOnEdge`.
void roundFreeCorners(std::span<GroundBox> boxes, Vec2f arenaHalf, float radius,
                      bool squareOnEdge) {
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    GroundBox& b = boxes[i];
    const float r = std::max(0.0f, std::min({radius, b.half.x, b.half.y}));
    for (std::size_t k = 0; k < kCornerSigns.size(); ++k) {
      const Vec2f corner{b.center.x + (kCornerSigns[k].x * b.half.x),
                         b.center.y + (kCornerSigns[k].y * b.half.y)};
      bool free = !squareOnEdge || (std::fabs(corner.x) < arenaHalf.x - kTouch &&
                                    std::fabs(corner.y) < arenaHalf.y - kTouch);
      for (std::size_t j = 0; free && j < boxes.size(); ++j) {
        if (j != i && contains(boxes[j], corner)) free = false;
      }
      b.radii[k] = free ? r : 0.0f;
    }
  }
}

// Numbers the groups of touching boxes (in order of their first box) and
// sorts the boxes by group, keeping content order within one.
void groupBoxes(std::span<GroundBox> boxes) {
  std::array<std::size_t, kMaxGroundBoxes> parent{};
  const auto root = [&](std::size_t i) {
    while (parent[i] != i)
      i = parent[i];
    return i;
  };
  for (std::size_t i = 0; i < boxes.size(); ++i)
    parent[i] = i;
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    for (std::size_t j = i + 1; j < boxes.size(); ++j) {
      if (touches(boxes[i], boxes[j])) parent[root(j)] = root(i);
    }
  }
  std::array<std::size_t, kMaxGroundBoxes> idOfRoot{};
  idOfRoot.fill(kMaxGroundBoxes);
  uint8_t next = 0;
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    const std::size_t r = root(i);
    if (idOfRoot[r] == kMaxGroundBoxes) idOfRoot[r] = next++;
    boxes[i].group = static_cast<uint8_t>(idOfRoot[r]);
  }
  std::ranges::stable_sort(
      boxes, [](const GroundBox& a, const GroundBox& b) { return a.group < b.group; });
}

// Writes a bridge for every stretch of edge two of `boxes` share to `out`, as
// far as it has room; returns how many.
std::size_t addBridges(std::span<const GroundBox> boxes, std::span<GroundBox> out) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    for (std::size_t j = i + 1; j < boxes.size(); ++j) {
      const GroundBox& a = boxes[i];
      const GroundBox& b = boxes[j];
      for (int axis = 0; axis < 2; ++axis) {
        // The seam lies across `axis`: a's edge on it meets b's opposite edge.
        const float ac = axis == 0 ? a.center.x : a.center.y;
        const float bc = axis == 0 ? b.center.x : b.center.y;
        const float ah = axis == 0 ? a.half.x : a.half.y;
        const float bh = axis == 0 ? b.half.x : b.half.y;
        const float gap = std::fabs(ac - bc) - (ah + bh);
        if (std::fabs(gap) > kTouch) continue;
        const float seam = ac < bc ? ac + ah : ac - ah;
        // The shared stretch along the other axis.
        const float ao = axis == 0 ? a.center.y : a.center.x;
        const float bo = axis == 0 ? b.center.y : b.center.x;
        const float aoh = axis == 0 ? a.half.y : a.half.x;
        const float boh = axis == 0 ? b.half.y : b.half.x;
        const float lo = std::max(ao - aoh, bo - boh);
        const float hi = std::min(ao + aoh, bo + boh);
        if (hi - lo <= kTouch || n >= out.size()) continue;
        const float reach = std::min({kBridgeReach, 2.0f * ah, 2.0f * bh});
        out[n++] = axis == 0 ? boxOf(seam - reach, lo, seam + reach, hi)
                             : boxOf(lo, seam - reach, hi, seam + reach);
      }
    }
  }
  return n;
}

// Rounds, groups and bridges `real` boxes of one kind into `out`: the boxes
// sorted by group, then their bridges as far as `out` has room. Returns the
// count written and the bridges among them.
struct Written {
  std::size_t count{0};
  std::size_t bridges{0};
};
Written finishKind(std::span<GroundBox> real, std::span<GroundBox> out, Vec2f arenaHalf,
                   float radius, bool squareOnEdge) {
  roundFreeCorners(real, arenaHalf, radius, squareOnEdge);
  groupBoxes(real);
  std::ranges::copy(real, out.begin());
  const std::size_t bridges = addBridges(real, out.subspan(real.size()));
  return Written{.count = real.size() + bridges, .bridges = bridges};
}

// Cuts the arena minus the pits into rectangles: the grid of every pit edge,
// its free cells merged into runs along each row, then runs with the same
// columns on consecutive rows into one (the tilemap loader's merge). Returns
// the count, or out.size() + 1 when they do not fit.
std::size_t landBoxes(std::span<const GroundBox> pits, Vec2f half, std::span<GroundBox> out) {
  std::array<float, kMaxCuts> xs{};
  std::array<float, kMaxCuts> ys{};
  std::size_t nx = 0;
  std::size_t ny = 0;
  xs[nx++] = -half.x;
  xs[nx++] = half.x;
  ys[ny++] = -half.y;
  ys[ny++] = half.y;
  for (const GroundBox& p : pits) {
    for (const float x : {p.center.x - p.half.x, p.center.x + p.half.x})
      if (x > -half.x + kTouch && x < half.x - kTouch) xs[nx++] = x;
    for (const float y : {p.center.y - p.half.y, p.center.y + p.half.y})
      if (y > -half.y + kTouch && y < half.y - kTouch) ys[ny++] = y;
  }
  const auto unique = [](std::span<float> v) {
    std::ranges::sort(v);
    std::size_t n = 0;
    for (const float f : v)
      if (n == 0 || f - v[n - 1] > kTouch) v[n++] = f;
    return n;
  };
  nx = unique(std::span(xs).first(nx));
  ny = unique(std::span(ys).first(ny));
  const auto free = [&](std::size_t i, std::size_t j) {
    const Vec2f c{0.5f * (xs[i] + xs[i + 1]), 0.5f * (ys[j] + ys[j + 1])};
    return std::ranges::none_of(pits, [&](const GroundBox& p) {
      return std::fabs(c.x - p.center.x) < p.half.x && std::fabs(c.y - p.center.y) < p.half.y;
    });
  };
  // Open rectangles: columns [i0, i1), rows [j0, j1).
  struct Cells {
    std::size_t i0, i1, j0, j1;
  };
  std::array<Cells, kMaxGroundBoxes> rects{};
  std::size_t n = 0;
  for (std::size_t j = 0; j + 1 < ny; ++j) {
    std::size_t i = 0;
    while (i + 1 < nx) {
      if (!free(i, j)) {
        ++i;
        continue;
      }
      const std::size_t i0 = i;
      while (i + 1 < nx && free(i, j))
        ++i;
      bool extended = false;
      for (std::size_t r = 0; r < n && !extended; ++r) {
        if (rects[r].i0 == i0 && rects[r].i1 == i && rects[r].j1 == j) {
          rects[r].j1 = j + 1;
          extended = true;
        }
      }
      if (extended) continue;
      if (n >= out.size() || n >= rects.size()) return out.size() + 1;
      rects[n++] = Cells{.i0 = i0, .i1 = i, .j0 = j, .j1 = j + 1};
    }
  }
  for (std::size_t r = 0; r < n; ++r)
    out[r] = boxOf(xs[rects[r].i0], ys[rects[r].j0], xs[rects[r].i1], ys[rects[r].j1]);
  return n;
}

// Carries every edge of `b` that lies on the arena's edge kPitOverhang past it.
void overhang(GroundBox& b, Vec2f arenaHalf) {
  float lox = b.center.x - b.half.x;
  float hix = b.center.x + b.half.x;
  float loy = b.center.y - b.half.y;
  float hiy = b.center.y + b.half.y;
  if (lox <= -arenaHalf.x + kTouch) lox -= kPitOverhang;
  if (hix >= arenaHalf.x - kTouch) hix += kPitOverhang;
  if (loy <= -arenaHalf.y + kTouch) loy -= kPitOverhang;
  if (hiy >= arenaHalf.y - kTouch) hiy += kPitOverhang;
  b = GroundBox{.center = boxOf(lox, loy, hix, hiy).center,
                .half = boxOf(lox, loy, hix, hiy).half,
                .radii = b.radii,
                .group = b.group};
}

// The polynomial smooth minimum rounds a right-angled corner to about this
// share of its k: k = radius / this.
constexpr float kSminRadiusPerK = 0.853f;

[[nodiscard]] float sdRoundBox(Vec2f p, Vec2f b, const std::array<float, 4>& radii) {
  // radii: (+x +y), (+x -y), (-x +y), (-x -y).
  const std::size_t corner = (p.x > 0.0f ? 0U : 2U) + (p.y > 0.0f ? 0U : 1U);
  const float r = radii[corner];
  const float qx = std::fabs(p.x) - b.x + r;
  const float qy = std::fabs(p.y) - b.y + r;
  const float ox = std::max(qx, 0.0f);
  const float oy = std::max(qy, 0.0f);
  return std::min(std::max(qx, qy), 0.0f) + std::sqrt((ox * ox) + (oy * oy)) - r;
}

[[nodiscard]] float smin(float a, float b, float k) {
  const float h = std::max(k - std::fabs(a - b), 0.0f) / k;
  return std::min(a, b) - (h * h * k * 0.25f);
}

// The union of `boxes`: the boxes of a group (in a row) smoothly with k, the
// groups and the last `bridges` boxes with a plain minimum.
[[nodiscard]] float unionOf(std::span<const GroundBox> boxes, std::size_t bridges, Vec2f p,
                            float k) {
  float done = 1e4f;
  float group = 1e4f;
  int current = -1;
  float bridged = 1e4f;
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    const GroundBox& b = boxes[i];
    const float e = sdRoundBox({p.x - b.center.x, p.y - b.center.y}, b.half, b.radii);
    if (i + bridges < boxes.size()) {
      if (std::cmp_not_equal(b.group, current)) {
        done = std::min(done, group);
        group = 1e4f;
        current = b.group;
      }
      group = smin(group, e, k);
    } else {
      bridged = std::min(bridged, e);
    }
  }
  return std::min({done, group, bridged});
}

}  // namespace

uint16_t toHalf(float f) {
  uint32_t bits = 0;
  std::memcpy(&bits, &f, sizeof(bits));
  const auto sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
  const int exponent = static_cast<int>((bits >> 23) & 0xFFu) - 127 + 15;
  uint32_t mantissa = bits & 0x7FFFFFu;
  if (exponent <= 0) {
    // Subnormal or zero: shift the implicit one in, round to nearest.
    if (exponent < -10) return sign;
    mantissa |= 0x800000u;
    const auto shift = static_cast<uint32_t>(14 - exponent);
    const uint32_t half = (mantissa + (1u << (shift - 1))) >> shift;
    return static_cast<uint16_t>(sign | half);
  }
  if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7C00u);  // infinity
  const uint32_t rounded = mantissa + 0x1000u;  // round the dropped 13 bits to nearest
  if ((rounded & 0x800000u) != 0) {
    // The mantissa overflowed into the exponent.
    if (exponent + 1 >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent + 1) << 10));
  }
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (rounded >> 13));
}

float fromHalf(uint16_t h) {
  const uint32_t sign = (static_cast<uint32_t>(h) & 0x8000u) << 16;
  const uint32_t exponent = (static_cast<uint32_t>(h) >> 10) & 0x1Fu;
  const uint32_t mantissa = static_cast<uint32_t>(h) & 0x3FFu;
  float f = 0.0f;
  if (exponent == 0) {
    f = std::ldexp(static_cast<float>(mantissa), -24);
  } else if (exponent == 31) {
    f = mantissa == 0 ? std::numeric_limits<float>::infinity()
                      : std::numeric_limits<float>::quiet_NaN();
  } else {
    f = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
  }
  return sign != 0 ? -f : f;
}

Vec4 groundDistances(const GroundLayout& layout, const ArenaLook& look, Vec2f p) {
  const GroundLayout& g = layout;
  // Concave corners (where boxes of a group meet) round by kGroundFillet
  // only: a fillet takes land from a pit, or slow patch from the floor, and
  // must not make either look smaller than it is.
  const float k = kGroundFillet / kSminRadiusPerK;
  const std::span<const GroundBox> all(g.boxes);
  const std::span<const GroundBox> land = all.first(g.landCount);
  const std::span<const GroundBox> slow = all.subspan(g.landCount, g.slowCount);
  float dl = unionOf(land, g.landBridges, p, k);
  if (g.landFromPits) {
    // The boxes are the pits: the land is the arena minus them, its
    // corners against the edge rounded by about the look's corner radius.
    const float r = look.cornerRadius;
    const float arena = sdRoundBox(p, g.half, {r, r, r, r});
    dl = -smin(-arena, dl, std::max(r, 0.05f) / kSminRadiusPerK);
  }
  const float ds = g.slowCount > 0 ? unionOf(slow, g.slowBridges, p, k) : 1e4f;
  float dw = 1e4f;
  float shadow = 1e4f;
  for (const GroundBox& w : std::span(g.walls).first(g.wallCount)) {
    dw = std::min(dw, sdRoundBox({p.x - w.center.x, p.y - w.center.y}, w.half, w.radii));
    shadow = std::min(shadow, sdRoundBox({p.x - w.center.x, p.y + kWallShadowOffset - w.center.y},
                                         w.half, w.radii));
  }
  return {dl, ds, dw, shadow};
}

GroundField buildGroundField(const GroundLayout& layout, const ArenaLook& look) {
  GroundField f;
  f.origin = {-layout.half.x - kGroundFieldMargin, -layout.half.y - kGroundFieldMargin};
  f.size = {2.0f * (layout.half.x + kGroundFieldMargin),
            2.0f * (layout.half.y + kGroundFieldMargin)};
  f.width = std::max(1, static_cast<int>(std::ceil(f.size.x * kGroundFieldTexelsPerUnit)));
  f.height = std::max(1, static_cast<int>(std::ceil(f.size.y * kGroundFieldTexelsPerUnit)));
  f.texels.resize(static_cast<std::size_t>(f.width) * static_cast<std::size_t>(f.height) * 4);
  const auto clampRange = [](float d) {
    return toHalf(std::clamp(d, -kGroundFieldRange, kGroundFieldRange));
  };
  std::size_t at = 0;
  for (int j = 0; j < f.height; ++j) {
    const float y =
        f.origin.y + ((static_cast<float>(j) + 0.5f) / static_cast<float>(f.height)) * f.size.y;
    for (int i = 0; i < f.width; ++i) {
      const float x =
          f.origin.x + ((static_cast<float>(i) + 0.5f) / static_cast<float>(f.width)) * f.size.x;
      const Vec4 d = groundDistances(layout, look, {x, y});
      f.texels[at++] = clampRange(d.x);
      f.texels[at++] = clampRange(d.y);
      f.texels[at++] = clampRange(d.z);
      f.texels[at++] = clampRange(d.w);
    }
  }
  return f;
}

float wallCornerRadius(const ArenaLook& look, Vec2f half) {
  return std::max(0.0f, std::min(look.wallRadius, kWallRadiusShare * std::min(half.x, half.y)));
}

GroundLayout buildGroundLayout(const RenderState& state, const ArenaLook& look) {
  GroundLayout g;
  g.half = state.arenaHalfExtents;
  std::array<GroundBox, sim::kMaxArenaHazards> pits{};
  std::array<GroundBox, sim::kMaxArenaHazards> slows{};
  std::size_t pitCount = 0;
  std::size_t slowCount = 0;
  for (uint16_t i = 0; i < state.arenaPieceCount; ++i) {
    const RenderArenaPiece& piece = state.arenaPieces[i];
    // World (x, h, z) is sim (x, -z); a box's world z extent is its sim y extent.
    GroundBox box{.center = {piece.center.x, -piece.center.z},
                  .half = {0.5f * piece.size.x, 0.5f * piece.size.z},
                  .radii = {},
                  .group = 0};
    switch (piece.kind) {
      case ArenaPieceKind::Wall:
        if (g.wallCount < g.walls.size()) {
          box.radii.fill(wallCornerRadius(look, box.half));
          g.walls[g.wallCount++] = box;
        }
        break;
      case ArenaPieceKind::Pit:
        if (pitCount < pits.size()) pits[pitCount++] = box;
        break;
      case ArenaPieceKind::Slow:
        if (slowCount < slows.size()) slows[slowCount++] = box;
        break;
      case ArenaPieceKind::PitRim:
        break;  // the ground program draws its own edges
    }
  }

  // The slow patches go last; the land takes what room they leave.
  std::array<GroundBox, kMaxGroundBoxes> slowOut{};
  const Written slow = finishKind(std::span(slows).first(slowCount), std::span(slowOut), g.half,
                                  look.slowRadius, true);
  const std::size_t landRoom = kMaxGroundBoxes - slow.count;
  std::array<GroundBox, kMaxGroundBoxes> land{};
  const std::size_t landCount =
      landBoxes(std::span(pits).first(pitCount), g.half, std::span(land).first(landRoom));
  const std::span<GroundBox> out(g.boxes);
  Written written;
  if (landCount <= landRoom) {
    written = finishKind(std::span(land).first(landCount), out.first(landRoom), g.half,
                         look.cornerRadius, false);
  } else {
    // Too many pieces of land: the pits instead (there are at most
    // kMaxArenaHazards, and slowCount + pitCount <= kMaxArenaHazards).
    g.landFromPits = true;
    const std::span<GroundBox> pitSpan = std::span(pits).first(pitCount);
    roundFreeCorners(pitSpan, g.half, look.pitRadius, true);
    for (GroundBox& b : pitSpan)
      overhang(b, g.half);
    groupBoxes(pitSpan);
    std::ranges::copy(pitSpan, out.begin());
    const std::size_t bridges = addBridges(pitSpan, out.subspan(pitCount, landRoom - pitCount));
    written = Written{.count = pitCount + bridges, .bridges = bridges};
  }
  g.landCount = static_cast<uint8_t>(written.count);
  g.landBridges = static_cast<uint8_t>(written.bridges);
  std::copy_n(slowOut.begin(), slow.count, out.subspan(written.count).begin());
  g.slowCount = static_cast<uint8_t>(slow.count);
  g.slowBridges = static_cast<uint8_t>(slow.bridges);
  return g;
}

}  // namespace kfm::presentation
