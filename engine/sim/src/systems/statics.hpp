// Circles against the arena's statics (architecture §5.10): the outer box and
// the interior walls, for players (slide) and bananas (bounce) through one
// code path, plus the hazard queries. Header-only so unit tests can check the
// geometry directly; not part of the public sim interface.
//
// moveCircle() moves a circle of radius r by `delta` from `from`:
//   1. x pass: x moves by delta.x; the nearest wall face it crosses while its
//      y lies within the wall's face band [min.y, max.y] stops it (Slide: at
//      the face, velocity x zeroed) or mirrors it (Bounce: overshoot
//      reflected, velocity x = -x * restitution). Swept: a face is crossed
//      when the start is on or outside it and the end past it, so no speed
//      tunnels through a wall.
//   2. rounded corners: every wall the circle still overlaps (it can only
//      have entered a corner zone, never the box itself, which needs the
//      band) pushes it out along the normal from the closest box point
//      (Slide: the inward velocity component is removed; Bounce: it is
//      reflected with restitution).
//   3. y pass and corners again, with the y displacement unchanged.
//   4. the outer box: Slide clamps per axis (zeroing that velocity
//      component), Bounce mirrors the overshoot per axis, exactly the M1
//      wall code, so an arena without walls moves bit-identically to M1.
// Walls are visited in content order and each pass takes the nearest face,
// so the result never depends on iteration order within a pass. No spatial
// structure: at most kMaxArenaWalls boxes, each rejected by a band test.
//
// Coordinates stay within ±1024 units (the loader's arena limit), so every
// int64 product below is exact.
#pragma once

#include "kfm/core/fixed.hpp"
#include "kfm/core/vec2.hpp"
#include "kfm/sim/content.hpp"
#include "systems/collision_internal.hpp"

#include <cstdint>

namespace kfm::sim::statics {

enum class Response : uint8_t {
  Slide = 0,  // players: stop at the face, keep the tangential motion
  Bounce,     // bananas: reflect the normal component
};

struct MoveParams {
  Fixed radius{};
  Response response{Response::Slide};
  Fixed restitution{Fixed::one()};  // Bounce only
  bool outerBox{true};              // also keep the circle inside the arena box
};

struct MoveResult {
  Vec2 pos{};
  bool hitWall{false};   // an interior wall stopped, deflected or pushed the circle
  bool hitOuter{false};  // the outer box clamped or reflected it
};

// --- boxes --------------------------------------------------------------------

// (boxContains / circleOverlapsBox live with ArenaDef in content/arena.hpp:
// the loader validates spawn points with the same tests.)

// Whether a circle overlaps any interior wall of the arena.
[[nodiscard]] constexpr bool overlapsAnyWall(const ArenaDef& arena, Vec2 c, Fixed r) {
  for (uint8_t i = 0; i < arena.wallCount && i < kMaxArenaWalls; ++i) {
    if (circleOverlapsBox(arena.walls[i].min, arena.walls[i].max, c, r)) return true;
  }
  return false;
}

// --- one axis -----------------------------------------------------------------

// Coordinates of one axis (a) and the other (b) of a Vec2.
struct Axis {
  bool isX{true};
  [[nodiscard]] constexpr Fixed& a(Vec2& v) const { return isX ? v.x : v.y; }
  [[nodiscard]] constexpr Fixed a(const Vec2& v) const { return isX ? v.x : v.y; }
  [[nodiscard]] constexpr Fixed b(const Vec2& v) const { return isX ? v.y : v.x; }
};

// Moves pos along one axis by d against the wall faces. Returns true when a
// face stopped or reflected it.
constexpr bool moveAxis(const ArenaDef& arena, Axis ax, Vec2& pos, Vec2& vel, Fixed d,
                        const MoveParams& m) {
  const Fixed start = ax.a(pos);
  const Fixed end = start + d;
  Fixed face{};
  bool blocked = false;
  if (d != Fixed::zero()) {
    for (uint8_t i = 0; i < arena.wallCount && i < kMaxArenaWalls; ++i) {
      const Wall& w = arena.walls[i];
      const Fixed other = ax.b(pos);
      if (other < ax.b(w.min) || other > ax.b(w.max)) continue;  // outside the face band
      if (d > Fixed::zero()) {
        const Fixed f = ax.a(w.min) - m.radius;
        if (start <= f && end > f && (!blocked || f < face)) {
          face = f;
          blocked = true;
        }
      } else {
        const Fixed f = ax.a(w.max) + m.radius;
        if (start >= f && end < f && (!blocked || f > face)) {
          face = f;
          blocked = true;
        }
      }
    }
  }
  if (!blocked) {
    ax.a(pos) = end;
    return false;
  }
  if (m.response == Response::Slide) {
    ax.a(pos) = face;
    ax.a(vel) = Fixed::zero();
  } else {
    ax.a(pos) = (face * 2) - end;  // mirror the overshoot
    ax.a(vel) = -ax.a(vel) * m.restitution;
  }
  return true;
}

// Pushes a circle out of one wall it overlaps (corner zones, or a start that
// already overlapped). Returns true when it moved.
constexpr bool pushOut(const Wall& w, Vec2& pos, Vec2& vel, const MoveParams& m) {
  if (!circleOverlapsBox(w.min, w.max, pos, m.radius)) return false;
  const Vec2 closest{clamp(pos.x, w.min.x, w.max.x), clamp(pos.y, w.min.y, w.max.y)};
  Vec2 normal{};
  if (closest == pos) {
    // Centre inside the box: out through the nearest face (ties: -x, +x, -y, +y).
    const Fixed left = pos.x - w.min.x;
    const Fixed right = w.max.x - pos.x;
    const Fixed down = pos.y - w.min.y;
    const Fixed up = w.max.y - pos.y;
    const Fixed best = min(min(left, right), min(down, up));
    if (best == left) {
      pos.x = w.min.x - m.radius;
      normal = Vec2{-Fixed::one(), Fixed::zero()};
    } else if (best == right) {
      pos.x = w.max.x + m.radius;
      normal = Vec2{Fixed::one(), Fixed::zero()};
    } else if (best == down) {
      pos.y = w.min.y - m.radius;
      normal = Vec2{Fixed::zero(), -Fixed::one()};
    } else {
      pos.y = w.max.y + m.radius;
      normal = Vec2{Fixed::zero(), Fixed::one()};
    }
  } else {
    normal = normalized(pos - closest);
    if (normal == Vec2::zero()) return false;  // below Fixed resolution: leave it
    pos = closest + (normal * m.radius);
  }
  const Fixed vn = dot(vel, normal);
  if (vn < Fixed::zero()) {
    const Fixed scale = m.response == Response::Slide ? vn : vn * (Fixed::one() + m.restitution);
    vel -= normal * scale;
  }
  return true;
}

constexpr bool pushOutAll(const ArenaDef& arena, Vec2& pos, Vec2& vel, const MoveParams& m) {
  bool moved = false;
  for (uint8_t i = 0; i < arena.wallCount && i < kMaxArenaWalls; ++i) {
    moved = pushOut(arena.walls[i], pos, vel, m) || moved;
  }
  return moved;
}

// --- the outer box (the M1 wall code, unchanged) ------------------------------

// Clamps one axis to [-limit, limit]; zeroes the velocity component on contact.
constexpr bool clampAxis(Fixed& pos, Fixed& vel, Fixed limit) {
  if (pos > limit) {
    pos = limit;
    vel = Fixed::zero();
    return true;
  }
  if (pos < -limit) {
    pos = -limit;
    vel = Fixed::zero();
    return true;
  }
  return false;
}

// Reflects one axis off the walls at ±limit. Returns true on a bounce.
constexpr bool reflectAxis(Fixed& pos, Fixed& vel, Fixed limit, Fixed restitution) {
  bool bounced = false;
  if (pos > limit) {
    pos = (limit * 2) - pos;
    vel = -vel * restitution;
    bounced = true;
  } else if (pos < -limit) {
    pos = (-limit * 2) - pos;
    vel = -vel * restitution;
    bounced = true;
  }
  // An overshoot larger than the whole box would mirror past the far wall.
  pos = clamp(pos, -limit, limit);
  return bounced;
}

// --- the move -----------------------------------------------------------------

// Moves a circle by `delta` from `from` through the arena's statics (see the
// header comment). `vel` is updated by every stop, slide and bounce.
constexpr MoveResult moveCircle(const ArenaDef& arena, Vec2 from, Vec2 delta, Vec2& vel,
                                const MoveParams& m) {
  MoveResult r;
  Vec2 pos = from;
  if (arena.wallCount == 0) {
    pos = from + delta;  // the M1 integration, bit for bit
  } else {
    r.hitWall = moveAxis(arena, Axis{true}, pos, vel, delta.x, m) || r.hitWall;
    r.hitWall = pushOutAll(arena, pos, vel, m) || r.hitWall;
    r.hitWall = moveAxis(arena, Axis{false}, pos, vel, delta.y, m) || r.hitWall;
    r.hitWall = pushOutAll(arena, pos, vel, m) || r.hitWall;
  }
  if (m.outerBox) {
    if (m.response == Response::Slide) {
      const Vec2 limit = arena.halfExtents - Vec2{m.radius, m.radius};
      const bool cx = clampAxis(pos.x, vel.x, limit.x);
      const bool cy = clampAxis(pos.y, vel.y, limit.y);
      r.hitOuter = cx || cy;
    } else {
      const Vec2 limit = Vec2{max(arena.halfExtents.x - m.radius, Fixed::zero()),
                              max(arena.halfExtents.y - m.radius, Fixed::zero())};
      const bool bx = reflectAxis(pos.x, vel.x, limit.x, m.restitution);
      const bool by = reflectAxis(pos.y, vel.y, limit.y, m.restitution);
      r.hitOuter = bx || by;
    }
  }
  r.pos = pos;
  return r;
}

// --- hazards ------------------------------------------------------------------

// Fraction of the move from -> to at which a point entering the closed box
// first lies inside it: 0 when `from` is already inside. Precondition: `to`
// is inside the box.
[[nodiscard]] constexpr Fixed entryTime(Vec2 from, Vec2 to, Vec2 min, Vec2 max) {
  const auto axis = [](Fixed a0, Fixed a1, Fixed lo, Fixed hi) -> int32_t {
    if (a0 >= lo && a0 <= hi) return 0;
    const int64_t num = a0 < lo ? int64_t{lo.raw} - a0.raw : int64_t{a0.raw} - hi.raw;
    const int64_t den = a0 < lo ? int64_t{a1.raw} - a0.raw : int64_t{a0.raw} - a1.raw;
    if (den <= 0) return Fixed::kOneRaw;  // unreachable when `to` is inside
    return collision::fractionQ16(static_cast<uint64_t>(num), static_cast<uint64_t>(den));
  };
  const int32_t tx = axis(from.x, to.x, min.x, max.x);
  const int32_t ty = axis(from.y, to.y, min.y, max.y);
  return Fixed::fromRaw(tx > ty ? tx : ty);
}

// Move speed multiplier at a position: the product of the speedMul of every
// Slow hazard containing it, in content order; exactly one outside them.
[[nodiscard]] constexpr Fixed speedMulAt(const ArenaDef& arena, Vec2 p) {
  Fixed mul = Fixed::one();
  for (uint8_t i = 0; i < arena.hazardCount && i < kMaxArenaHazards; ++i) {
    const Hazard& h = arena.hazards[i];
    if (h.kind == HazardKind::Slow && boxContains(h.min, h.max, p)) mul = mul * h.speedMul;
  }
  return mul;
}

}  // namespace kfm::sim::statics
