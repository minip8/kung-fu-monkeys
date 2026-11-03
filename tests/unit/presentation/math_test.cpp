// Float math (presentation/math.hpp): identities, hand-computed projection
// and view matrices, angle helpers and the world basis.
#include "kfm/presentation/math.hpp"

#include "kfm/presentation/world_basis.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace kfm;
using namespace kfm::presentation;
using Catch::Approx;

namespace {

bool near(float a, float b, float eps = 1e-5f) {
  return std::fabs(a - b) <= eps;
}
bool near(Vec3 a, Vec3 b, float eps = 1e-5f) {
  return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
bool near(const Mat4& a, const Mat4& b, float eps = 1e-5f) {
  for (std::size_t i = 0; i < 16; ++i) {
    if (!near(a.m[i], b.m[i], eps)) return false;
  }
  return true;
}

}  // namespace

TEST_CASE("Mat4 is column-major", "[presentation][math]") {
  const Mat4 t = translate(Vec3{1.0f, 2.0f, 3.0f});
  // A GL uniform upload reads the translation from elements 12..14.
  CHECK(t.data()[12] == 1.0f);
  CHECK(t.data()[13] == 2.0f);
  CHECK(t.data()[14] == 3.0f);
  CHECK(t.at(0, 3) == 1.0f);
  CHECK(transformPoint(t, Vec3{1.0f, 1.0f, 1.0f}) == Vec3{2.0f, 3.0f, 4.0f});
  CHECK(transformDirection(t, Vec3{1.0f, 1.0f, 1.0f}) == Vec3{1.0f, 1.0f, 1.0f});
}

TEST_CASE("matrix identities", "[presentation][math]") {
  const Mat4 a = translate(Vec3{1.0f, -2.0f, 0.5f}) * rotateY(0.7f) * scale(Vec3{2.0f, 3.0f, 4.0f});
  CHECK(a * Mat4::identity() == a);
  CHECK(Mat4::identity() * a == a);
  CHECK(transpose(transpose(a)) == a);
  CHECK(near(a * inverse(a), Mat4::identity()));
  CHECK(near(inverse(a) * a, Mat4::identity()));
  // Composition applies right to left.
  const Vec3 p{1.0f, 1.0f, 1.0f};
  CHECK(near(transformPoint(a, p),
             transformPoint(translate(Vec3{1.0f, -2.0f, 0.5f}),
                            transformPoint(rotateY(0.7f), Vec3{2.0f, 3.0f, 4.0f}))));
  // A singular matrix inverts to the identity rather than to NaNs.
  CHECK(inverse(scale(Vec3{0.0f, 1.0f, 1.0f})) == Mat4::identity());
}

TEST_CASE("rotateY turns +x toward -z, matching sim angles", "[presentation][math]") {
  CHECK(near(transformDirection(rotateY(0.5f * kPi), Vec3{1.0f, 0.0f, 0.0f}),
             Vec3{0.0f, 0.0f, -1.0f}));
  // Sim angle theta in world: rotateY(theta) applied to +x lands on the sim
  // direction (cos theta, sin theta) mapped to the world.
  for (const int deg : {0, 30, 90, 135, 200, 300}) {
    const Angle a = Angle::fromDegrees(deg);
    const float yaw = simAngleToYaw(a);
    const Vec3 dir = transformDirection(rotateY(yaw), Vec3{1.0f, 0.0f, 0.0f});
    const Vec3 expected = simToWorld(Vec2f{std::cos(yaw), std::sin(yaw)});
    CHECK(near(dir, expected, 1e-4f));
  }
}

TEST_CASE("perspective matches hand-computed values", "[presentation][math]") {
  // fovY 90 degrees: f = 1. aspect 2, near 1, far 3.
  const Mat4 p = perspective(0.5f * kPi, 2.0f, 1.0f, 3.0f);
  CHECK(p.at(0, 0) == Approx(0.5f));
  CHECK(p.at(1, 1) == Approx(1.0f));
  CHECK(p.at(2, 2) == Approx(-2.0f));  // (f + n) / (n - f) = 4 / -2
  CHECK(p.at(2, 3) == Approx(-3.0f));  // 2fn / (n - f) = 6 / -2
  CHECK(p.at(3, 2) == -1.0f);
  CHECK(p.at(3, 3) == 0.0f);
  // Near and far planes map to NDC -1 and +1, or 0 and 1.
  CHECK(projectPoint(p, Vec3{0.0f, 0.0f, -1.0f}).z == Approx(-1.0f));
  CHECK(projectPoint(p, Vec3{0.0f, 0.0f, -3.0f}).z == Approx(1.0f));
  const Mat4 p01 = perspective(0.5f * kPi, 2.0f, 1.0f, 3.0f, ClipDepth::ZeroToOne);
  CHECK(projectPoint(p01, Vec3{0.0f, 0.0f, -1.0f}).z == Approx(0.0f).margin(1e-6));
  CHECK(projectPoint(p01, Vec3{0.0f, 0.0f, -3.0f}).z == Approx(1.0f));
  // The frustum edge at depth 2: y = 2 (45 degrees) maps to NDC y = 1, x = 4 to x = 1.
  const Vec3 edge = projectPoint(p, Vec3{4.0f, 2.0f, -2.0f});
  CHECK(edge.x == Approx(1.0f));
  CHECK(edge.y == Approx(1.0f));
}

TEST_CASE("orthographic matches hand-computed values", "[presentation][math]") {
  const Mat4 o = orthographic(-2.0f, 2.0f, -1.0f, 3.0f, 1.0f, 5.0f);
  CHECK(o.at(0, 0) == Approx(0.5f));
  CHECK(o.at(1, 1) == Approx(0.5f));
  CHECK(o.at(1, 3) == Approx(-0.5f));
  CHECK(o.at(2, 2) == Approx(-0.5f));
  CHECK(o.at(2, 3) == Approx(-1.5f));
  CHECK(near(projectPoint(o, Vec3{2.0f, 3.0f, -5.0f}), Vec3{1.0f, 1.0f, 1.0f}));
  CHECK(near(projectPoint(o, Vec3{-2.0f, -1.0f, -1.0f}), Vec3{-1.0f, -1.0f, -1.0f}));
  const Mat4 o01 = orthographic(-2.0f, 2.0f, -1.0f, 3.0f, 1.0f, 5.0f, ClipDepth::ZeroToOne);
  CHECK(projectPoint(o01, Vec3{0.0f, 0.0f, -1.0f}).z == Approx(0.0f).margin(1e-6));
  CHECK(projectPoint(o01, Vec3{0.0f, 0.0f, -5.0f}).z == Approx(1.0f));
}

TEST_CASE("lookAt matches hand-computed values", "[presentation][math]") {
  // Default GL camera moved back: eye (0, 0, 5) looking at the origin.
  const Mat4 v = lookAt(Vec3{0.0f, 0.0f, 5.0f}, Vec3{}, Vec3{0.0f, 1.0f, 0.0f});
  CHECK(near(v, translate(Vec3{0.0f, 0.0f, -5.0f})));

  // Eye on +x looking at the origin: view x (right) is world -z.
  const Mat4 w = lookAt(Vec3{3.0f, 0.0f, 0.0f}, Vec3{}, Vec3{0.0f, 1.0f, 0.0f});
  CHECK(near(transformPoint(w, Vec3{}), Vec3{0.0f, 0.0f, -3.0f}));
  CHECK(near(transformDirection(w, Vec3{0.0f, 0.0f, -1.0f}), Vec3{1.0f, 0.0f, 0.0f}));
  CHECK(near(transformDirection(w, Vec3{0.0f, 1.0f, 0.0f}), Vec3{0.0f, 1.0f, 0.0f}));

  // Tilted down 45 degrees from (0, 1, 1): the target is straight ahead.
  const Mat4 t = lookAt(Vec3{0.0f, 1.0f, 1.0f}, Vec3{}, Vec3{0.0f, 1.0f, 0.0f});
  CHECK(near(transformPoint(t, Vec3{}), Vec3{0.0f, 0.0f, -std::sqrt(2.0f)}));
  CHECK(near(transformDirection(t, Vec3{1.0f, 0.0f, 0.0f}), Vec3{1.0f, 0.0f, 0.0f}));
}

TEST_CASE("angle helpers", "[presentation][math]") {
  CHECK(angleToRadians(Angle{}) == 0.0f);
  CHECK(angleToRadians(Angle::fromBam(Angle::kQuarter)) == Approx(0.5f * kPi));
  CHECK(angleToRadians(Angle::fromBam(Angle::kHalf)) == Approx(kPi));
  CHECK(wrapRadians(3.0f * kPi) == Approx(-kPi));
  CHECK(wrapRadians(-0.5f * kPi) == Approx(-0.5f * kPi));
  CHECK(wrapRadians(2.5f * kPi) == Approx(0.5f * kPi));
  // Shortest arc across the +-pi seam.
  // (+pi and -pi are the same heading; compare through wrapRadians.)
  const auto isHalfTurn = [](float a) { return std::fabs(wrapRadians(a + kPi)) < 1e-5f; };
  CHECK(isHalfTurn(lerpAngle(0.9f * kPi, -0.9f * kPi, 0.5f)));
  CHECK(approachAngle(0.0f, 1.0f, 0.25f) == Approx(0.25f));
  CHECK(approachAngle(0.0f, -1.0f, 0.25f) == Approx(-0.25f));
  CHECK(approachAngle(0.0f, 0.1f, 0.25f) == Approx(0.1f));
  CHECK(isHalfTurn(approachAngle(0.9f * kPi, -0.9f * kPi, 0.1f * kPi)));
  // Across the seam it turns the short way (+), not the long way round.
  CHECK(approachAngle(0.9f * kPi, -0.9f * kPi, 0.05f * kPi) == Approx(0.95f * kPi));
  CHECK(lerp(2.0f, 4.0f, 0.25f) == 2.5f);
  CHECK(near(normalize(Vec3{3.0f, 0.0f, 4.0f}), Vec3{0.6f, 0.0f, 0.8f}));
  CHECK(normalize(Vec3{}) == Vec3{});
  CHECK(cross(Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}) == Vec3{0.0f, 0.0f, 1.0f});
}

TEST_CASE("world basis maps sim y to world -z", "[presentation][math]") {
  const Vec2 sim{Fixed::fromInt(3), Fixed::fromInt(-2)};
  CHECK(simToWorld(sim, 1.5f) == Vec3{3.0f, 1.5f, 2.0f});
  CHECK(worldToSim(simToWorld(sim)) == Vec2f{3.0f, -2.0f});
  // Right-handed: sim x × sim y (as world vectors) points up.
  CHECK(cross(simToWorld(Vec2f{1.0f, 0.0f}), simToWorld(Vec2f{0.0f, 1.0f})) ==
        Vec3{0.0f, 1.0f, 0.0f});
}
