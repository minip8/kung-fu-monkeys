// Circles against arena statics (systems/statics.hpp, architecture §5.10):
// the per-axis slide with rounded corners, the swept bounce, the outer box
// kept bit-identical to M1, hazard entry time and the slow multiplier.
#include "systems/statics.hpp"

#include "kfm/sim/content.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>

using namespace kfm;
using namespace kfm::sim;

namespace {

Fixed fx(int32_t num, int32_t den = 1) {
  return Fixed::fromRatio(num, den);
}
Vec2 v(Fixed x, Fixed y) {
  return Vec2{x, y};
}

// A 20 × 10 arena with one 2 × 2 block centred at (3, 0).
ArenaDef blockArena() {
  ArenaDef a;
  a.name = ContentName::of("block");
  a.halfExtents = v(fx(10), fx(5));
  a.walls[0] = Wall{v(fx(2), fx(-1)), v(fx(4), fx(1))};
  a.wallCount = 1;
  return a;
}

constexpr statics::MoveParams kSlide{.radius = Fixed::half(), .response = statics::Response::Slide};
constexpr statics::MoveParams kBounce{
    .radius = Fixed::half(), .response = statics::Response::Bounce, .restitution = Fixed::one()};

bool near(Fixed a, Fixed b, int32_t rawTol = 8) {
  return std::abs(a.raw - b.raw) <= rawTol;
}

}  // namespace

TEST_CASE("a circle stops at a wall face and keeps sliding along it", "[sim][statics]") {
  const ArenaDef a = blockArena();
  // Moving +x and +y into the west face: x stops at min.x - r, y keeps going.
  Vec2 vel = v(fx(3, 5), fx(1, 10));
  const statics::MoveResult r = statics::moveCircle(a, v(fx(1), Fixed::zero()), vel, vel, kSlide);
  CHECK(r.hitWall);
  CHECK_FALSE(r.hitOuter);
  CHECK(r.pos.x == fx(3, 2));
  CHECK(r.pos.y == fx(1, 10));
  CHECK(vel.x == Fixed::zero());
  CHECK(vel.y == fx(1, 10));

  // Already touching the face: pushing into it does not move x at all.
  Vec2 vel2 = v(fx(1, 2), Fixed::zero());
  const statics::MoveResult r2 = statics::moveCircle(a, r.pos, vel2, vel2, kSlide);
  CHECK(r2.pos.x == fx(3, 2));
}

TEST_CASE("no speed tunnels through a thin wall", "[sim][statics]") {
  ArenaDef a = blockArena();
  a.walls[0] = Wall{v(fx(3), fx(-4)), v(fx(3) + fx(1, 20), fx(4))};  // 0.05 thick
  for (const int32_t speed : {1, 3, 7, 12}) {
    INFO("speed " << speed);
    Vec2 vel = v(fx(speed), Fixed::zero());
    const statics::MoveResult r = statics::moveCircle(a, v(fx(2), fx(1)), vel, vel, kSlide);
    CHECK(r.pos.x == fx(3) - Fixed::half());
    Vec2 velB = v(fx(speed), Fixed::zero());
    const statics::MoveResult rb = statics::moveCircle(a, v(fx(2), fx(1)), velB, velB, kBounce);
    CHECK(rb.pos.x <= fx(3) - Fixed::half());
    CHECK(velB.x == -fx(speed));
  }
}

TEST_CASE("corners are round: a circle slides around a block's corner", "[sim][statics]") {
  const ArenaDef a = blockArena();
  // Centre just below the south-west corner's band (y < -1), moving +x into
  // the corner zone: pushed out to exactly one radius from the corner.
  const Vec2 from = v(fx(15, 10), fx(-13, 10));
  Vec2 vel = v(fx(3, 10), Fixed::zero());
  const statics::MoveResult r = statics::moveCircle(a, from, vel, vel, kSlide);
  CHECK(r.hitWall);
  const Vec2 corner = v(fx(2), fx(-1));
  CHECK(near(length(r.pos - corner), Fixed::half()));
  CHECK(r.pos.y < fx(-1));  // deflected around, not stopped flat
  CHECK(r.pos.x > from.x);
  // Never inside the block itself.
  CHECK_FALSE(boxContains(a.walls[0].min, a.walls[0].max, r.pos));
}

TEST_CASE("a bouncing circle mirrors its overshoot off a face", "[sim][statics]") {
  const ArenaDef a = blockArena();
  // 0.25 short of the west face (x = 1.5), moving 0.5 per tick: 0.25 past it.
  Vec2 vel = v(fx(1, 2), fx(1, 8));
  const statics::MoveResult r =
      statics::moveCircle(a, v(fx(5, 4), Fixed::zero()), vel, vel, kBounce);
  CHECK(r.hitWall);
  CHECK(r.pos.x == fx(5, 4));  // 1.5 - 0.25
  CHECK(r.pos.y == fx(1, 8));
  CHECK(vel == v(-fx(1, 2), fx(1, 8)));

  // Restitution scales the reflected component only.
  statics::MoveParams half = kBounce;
  half.restitution = Fixed::half();
  Vec2 vel2 = v(fx(1, 2), fx(1, 8));
  (void)statics::moveCircle(a, v(fx(5, 4), Fixed::zero()), vel2, vel2, half);
  CHECK(vel2 == v(-fx(1, 4), fx(1, 8)));
}

TEST_CASE("a bounce off a corner reflects about the corner normal", "[sim][statics]") {
  const ArenaDef a = blockArena();
  // Heading diagonally (+x, +y) at the south-west corner (2, -1).
  Vec2 vel = v(fx(3, 10), fx(3, 10));
  const statics::MoveResult r =
      statics::moveCircle(a, v(fx(14, 10), fx(-16, 10)), vel, vel, kBounce);
  CHECK(r.hitWall);
  // Straight into the corner: the velocity comes straight back.
  CHECK(near(vel.x, -fx(3, 10), 64));
  CHECK(near(vel.y, -fx(3, 10), 64));
  CHECK(near(length(r.pos - v(fx(2), fx(-1))), Fixed::half()));
}

TEST_CASE("without walls the move is the M1 integrate-and-clamp / reflect", "[sim][statics]") {
  ArenaDef a = blockArena();
  a.wallCount = 0;
  // Slide: clamp to half extents minus radius, zero the clamped component.
  Vec2 vel = v(fx(1), fx(1, 3));
  statics::MoveResult r = statics::moveCircle(a, v(fx(9), fx(1)), vel, vel, kSlide);
  CHECK(r.hitOuter);
  CHECK(r.pos == v(fx(19, 2), fx(1) + fx(1, 3)));
  CHECK(vel == v(Fixed::zero(), fx(1, 3)));
  // Bounce: mirror about the limit, reflect.
  Vec2 velB = v(fx(1), Fixed::zero());
  r = statics::moveCircle(a, v(fx(9), Fixed::zero()), velB, velB, kBounce);
  CHECK(r.pos.x == fx(9));  // 9.5 * 2 - 10
  CHECK(velB.x == -fx(1));
  // Interior only (a throw's spawn path): the outer box is left alone.
  statics::MoveParams inner = kBounce;
  inner.outerBox = false;
  Vec2 velI = v(fx(1), Fixed::zero());
  r = statics::moveCircle(a, v(fx(9), Fixed::zero()), velI, velI, inner);
  CHECK(r.pos.x == fx(10));
  CHECK_FALSE(r.hitOuter);
}

TEST_CASE("a circle starting inside a block leaves through the nearest face", "[sim][statics]") {
  const ArenaDef a = blockArena();
  Vec2 vel{};
  const statics::MoveResult r = statics::moveCircle(a, v(fx(37, 10), fx(1, 5)), vel, vel, kSlide);
  CHECK(r.pos == v(fx(9, 2), fx(1, 5)));  // east face 4 + 0.5
}

TEST_CASE("hazard entry time and the slow multiplier", "[sim][statics]") {
  const Vec2 min = v(fx(0), fx(0));
  const Vec2 max = v(fx(2), fx(2));
  CHECK(statics::entryTime(v(fx(1), fx(1)), v(fx(1), fx(1)), min, max) == Fixed::zero());
  CHECK(statics::entryTime(v(fx(-1), fx(1)), v(fx(1), fx(1)), min, max) == Fixed::half());
  CHECK(statics::entryTime(v(fx(-1), fx(-3)), v(fx(1), fx(1)), min, max) == fx(3, 4));

  ArenaDef a = blockArena();
  a.hazards[0] = Hazard{v(fx(-4), fx(-4)), v(fx(0), fx(0)), HazardKind::Slow, Fixed::half()};
  a.hazards[1] = Hazard{v(fx(-2), fx(-2)), v(fx(2), fx(2)), HazardKind::Slow, Fixed::half()};
  a.hazards[2] = Hazard{v(fx(-9), fx(-4)), v(fx(9), fx(4)), HazardKind::Pit, Fixed::one()};
  a.hazardCount = 3;
  CHECK(statics::speedMulAt(a, v(fx(-3), fx(-3))) == Fixed::half());
  CHECK(statics::speedMulAt(a, v(fx(-1), fx(-1))) == fx(1, 4));  // both patches
  CHECK(statics::speedMulAt(a, v(fx(1), fx(1))) == Fixed::half());
  CHECK(statics::speedMulAt(a, v(fx(5), fx(1))) == Fixed::one());  // a pit is no slow
}

TEST_CASE("discOverPits: the whole disc inside the union of the pit boxes (v0.57)",
          "[sim][statics][pit]") {
  ArenaDef a;
  a.halfExtents = v(fx(10), fx(10));
  // An L of two pit boxes ([0, 4] x [0, 2] and [0, 2] x [2, 4]) and a slow
  // patch that is no pit.
  a.hazards[0] = Hazard{v(fx(0), fx(0)), v(fx(4), fx(2)), HazardKind::Pit, Fixed::one()};
  a.hazards[1] = Hazard{v(fx(0), fx(2)), v(fx(2), fx(4)), HazardKind::Pit, Fixed::one()};
  a.hazards[2] = Hazard{v(fx(-6), fx(-6)), v(fx(-2), fx(-2)), HazardKind::Slow, Fixed::half()};
  a.hazardCount = 3;
  const Fixed r = Fixed::half();
  // Inside one box, touching its edges from inside: in.
  CHECK(discOverPits(a, v(fx(1, 2), fx(1, 2)), r));
  CHECK(discOverPits(a, v(fx(7, 2), fx(1)), r));
  // Straddling the seam y = 2 of the two boxes: in (a per-box test says no).
  CHECK(discOverPits(a, v(fx(1), fx(2)), r));
  // Into the L's notch (x > 2, y > 2 is floor): out, by an exact margin.
  CHECK_FALSE(discOverPits(a, v(fx(2), fx(2)), fx(1, 10)));
  CHECK(discOverPits(a, v(fx(3, 2), fx(3, 2)), r));  // diagonal to the notch: clear of it
  // The notch's corner (2, 2) seen diagonally: (1.6, 1.6) is 0.4 * sqrt(2)
  // ~ 0.566 from it, so a radius of 0.5 clears it and 0.6 does not (the
  // disc, not its bounding square).
  CHECK(discOverPits(a, v(fx(8, 5), fx(8, 5)), r));
  CHECK_FALSE(discOverPits(a, v(fx(8, 5), fx(8, 5)), fx(3, 5)));
  // Hanging over the outer edge by an epsilon: out.
  CHECK_FALSE(discOverPits(a, v(fx(1, 2) - Fixed::epsilon(), fx(1)), r));
  // Wider than the pit: never in, wherever the centre.
  CHECK_FALSE(discOverPits(a, v(fx(1), fx(1)), fx(3, 2)));
  // A slow patch is no pit; radius 0 is the centre point.
  CHECK_FALSE(discOverPits(a, v(fx(-4), fx(-4)), r));
  CHECK(discOverPits(a, v(fx(0), fx(0)), Fixed::zero()));
  CHECK_FALSE(discOverPits(a, v(fx(3), fx(3)), Fixed::zero()));
}
