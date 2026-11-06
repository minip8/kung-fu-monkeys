// The fixed arena camera (presentation/camera.hpp): framing at several aspect
// ratios, the pinned screen conventions, the screen/ground round trip, and the
// pixel scale that ends a screen shake.
#include "kfm/presentation/camera.hpp"

#include "kfm/presentation/tuning.hpp"
#include "kfm/presentation/world_basis.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>

using namespace kfm;
using namespace kfm::presentation;
using Catch::Approx;

namespace {

struct Viewport {
  float w;
  float h;
  const char* name;
};

constexpr std::array<Viewport, 5> kViewports = {{
    {1920.0f, 1080.0f, "16:9"},
    {1024.0f, 768.0f, "4:3"},
    {2560.0f, 1080.0f, "21:9"},
    {800.0f, 1200.0f, "2:3 portrait"},
    {1000.0f, 1000.0f, "1:1"},
}};

const sim::ArenaDef& arena() {
  return sim::defaultContent().arenas[0];
}

std::array<Vec3, 4> groundCorners() {
  const float hx = arena().halfExtents.x.toFloat();
  const float hy = arena().halfExtents.y.toFloat();
  return {simToWorld(Vec2f{-hx, -hy}), simToWorld(Vec2f{hx, -hy}), simToWorld(Vec2f{hx, hy}),
          simToWorld(Vec2f{-hx, hy})};
}

}  // namespace

TEST_CASE("the arena camera frames every corner with margin", "[presentation][camera]") {
  for (const Viewport& vp : kViewports) {
    INFO(vp.name);
    const Camera cam = makeArenaCamera(arena(), vp.w, vp.h);
    const float margin = cam.params.margin * 0.5f;  // NDC margin -> fraction of the viewport
    float minX = 1e9f;
    float maxX = -1e9f;
    float minY = 1e9f;
    float maxY = -1e9f;
    for (const Vec3& c : groundCorners()) {
      for (const float h : {0.0f, cam.params.framingHeight}) {
        const ScreenPoint sp = worldToScreen(cam, Vec3{c.x, h, c.z});
        REQUIRE(sp.inFront);
        CHECK(sp.x >= vp.w * margin * 0.99f);
        CHECK(sp.x <= vp.w * (1.0f - (margin * 0.99f)));
        CHECK(sp.y >= vp.h * margin * 0.99f);
        CHECK(sp.y <= vp.h * (1.0f - (margin * 0.99f)));
        CHECK(sp.depth > -1.0f);
        CHECK(sp.depth < 1.0f);
        minX = std::min(minX, sp.x);
        maxX = std::max(maxX, sp.x);
        minY = std::min(minY, sp.y);
        maxY = std::max(maxY, sp.y);
      }
    }
    // Tight on the limiting axis: the arena fills (1 - margin) of it.
    const float fillX = (maxX - minX) / vp.w;
    const float fillY = (maxY - minY) / vp.h;
    CHECK(std::max(fillX, fillY) == Approx(1.0f - cam.params.margin).margin(2e-3));
    // Centred by the lens shift.
    CHECK((minX + maxX) * 0.5f == Approx(vp.w * 0.5f).margin(vp.w * 1e-3f));
    CHECK((minY + maxY) * 0.5f == Approx(vp.h * 0.5f).margin(vp.h * 1e-3f));
  }
}

TEST_CASE("the arena box is width-limited on narrow and height-limited on wide viewports",
          "[presentation][camera]") {
  const auto fill = [](const Camera& cam, float w, float h) {
    const auto c = groundCorners();
    const ScreenPoint nearLeft = worldToScreen(cam, c[0]);
    const ScreenPoint nearRight = worldToScreen(cam, c[1]);
    const ScreenPoint farLeft = worldToScreen(cam, Vec3{c[3].x, cam.params.framingHeight, c[3].z});
    return std::array<float, 2>{(nearRight.x - nearLeft.x) / w, (nearLeft.y - farLeft.y) / h};
  };
  const Camera portrait = makeArenaCamera(arena(), 800.0f, 1200.0f);
  const auto p = fill(portrait, 800.0f, 1200.0f);
  CHECK(p[0] > p[1]);
  const Camera ultra = makeArenaCamera(arena(), 2560.0f, 1080.0f);
  const auto u = fill(ultra, 2560.0f, 1080.0f);
  CHECK(u[1] > u[0]);
}

TEST_CASE("camera numbers stay in the long-lens tilted range", "[presentation][camera]") {
  const Camera cam = makeArenaCamera(arena(), 1920.0f, 1080.0f);
  CHECK(cam.fovY == Approx(degreesToRadians(30.0f)));
  // Pitched down: the eye is above the ground on the sim -y (world +z) side.
  CHECK(cam.eye.y > 0.0f);
  CHECK(cam.eye.z > 0.0f);
  CHECK(cam.eye.x == 0.0f);
  const Vec3 forward = normalize(cam.target - cam.eye);
  CHECK(std::asin(-forward.y) == Approx(degreesToRadians(55.0f)).margin(1e-4));
  CHECK(cam.zNear > 0.0f);
  CHECK(cam.zNear < cam.distance - 20.0f);
  CHECK(cam.zFar > cam.distance + 20.0f);
}

TEST_CASE("screen conventions: sim +x is right, sim +y is up", "[presentation][camera]") {
  const Camera cam = makeArenaCamera(arena(), 1920.0f, 1080.0f);
  for (const float y : {-6.0f, 0.0f, 6.0f}) {
    const ScreenPoint left = worldToScreen(cam, simToWorld(Vec2f{-5.0f, y}));
    const ScreenPoint right = worldToScreen(cam, simToWorld(Vec2f{5.0f, y}));
    CHECK(right.x > left.x);
  }
  for (const float x : {-12.0f, 0.0f, 12.0f}) {
    const ScreenPoint low = worldToScreen(cam, simToWorld(Vec2f{x, -4.0f}));
    const ScreenPoint high = worldToScreen(cam, simToWorld(Vec2f{x, 4.0f}));
    CHECK(high.y < low.y);  // pixel y grows downward
  }
  // Height lifts a point up the screen.
  CHECK(worldToScreen(cam, simToWorld(Vec2f{}, 1.0f)).y < worldToScreen(cam, Vec3{}).y);
  // Angles turn counter-clockwise on screen: +x, then +y is a left turn.
  const ScreenPoint o = worldToScreen(cam, Vec3{});
  const ScreenPoint ex = worldToScreen(cam, simToWorld(Vec2f{1.0f, 0.0f}));
  const ScreenPoint ey = worldToScreen(cam, simToWorld(Vec2f{0.0f, 1.0f}));
  // Cross product in a y-up screen frame (flip pixel y).
  const float crossZ = ((ex.x - o.x) * -(ey.y - o.y)) - (-(ex.y - o.y) * (ey.x - o.x));
  CHECK(crossZ > 0.0f);
}

TEST_CASE("screenToGround inverts worldToScreen on the ground plane", "[presentation][camera]") {
  for (const ClipDepth depth : {ClipDepth::NegativeOneToOne, ClipDepth::ZeroToOne}) {
    CameraParams params;
    params.clipDepth = depth;
    const Camera cam = makeArenaCamera(arena(), 1280.0f, 720.0f, params);
    for (const Vec2f p : {Vec2f{0.0f, 0.0f}, Vec2f{-15.5f, 8.5f}, Vec2f{15.5f, -8.5f},
                          Vec2f{3.25f, -1.75f}, Vec2f{-7.0f, 2.0f}}) {
      const ScreenPoint sp = worldToScreen(cam, simToWorld(p));
      REQUIRE(sp.inFront);
      const auto hit = screenToGround(cam, sp.x, sp.y);
      REQUIRE(hit.has_value());
      const Vec3 h = hit.value_or(Vec3{});
      CHECK(h.y == Approx(0.0f).margin(1e-4));
      const Vec2f back = worldToSim(h);
      CHECK(back.x == Approx(p.x).margin(2e-3));
      CHECK(back.y == Approx(p.y).margin(2e-3));
    }
    // A raised plane (aim at hand height) round-trips too.
    const Vec3 raised = simToWorld(Vec2f{4.0f, 3.0f}, 0.6f);
    const ScreenPoint sp = worldToScreen(cam, raised);
    const auto hit = screenToGround(cam, sp.x, sp.y, 0.6f);
    REQUIRE(hit.has_value());
    const Vec3 h = hit.value_or(Vec3{});
    CHECK(h.x == Approx(raised.x).margin(2e-3));
    CHECK(h.z == Approx(raised.z).margin(2e-3));
  }
}

TEST_CASE("screenToGround misses above the horizon", "[presentation][camera]") {
  // Far above the viewport the ray through the pixel points up into the sky.
  const Camera cam = makeArenaCamera(arena(), 1280.0f, 720.0f);
  CHECK_FALSE(screenToGround(cam, 640.0f, -72000.0f).has_value());
  CHECK(screenToGround(cam, 640.0f, 0.0f).has_value());  // 55 degrees down: no sky in view
  CHECK(screenToGround(cam, 640.0f, 719.0f).has_value());
}

TEST_CASE("pixelsPerUnitAtTarget scales with the viewport", "[presentation][camera]") {
  const Camera small = makeArenaCamera(arena(), 1280.0f, 720.0f);
  const Camera large = makeArenaCamera(arena(), 2560.0f, 1440.0f);
  const float ppu = pixelsPerUnitAtTarget(small);
  // The 32-unit dojo spans most of a 1280-pixel width.
  CHECK(ppu > 1280.0f / 64.0f);
  CHECK(ppu < 1280.0f / 32.0f);
  CHECK(pixelsPerUnitAtTarget(large) == Approx(2.0f * ppu).epsilon(1e-3));
  // One world unit right of the target projects that many pixels away.
  const ScreenPoint a = worldToScreen(small, small.target);
  const ScreenPoint b = worldToScreen(small, small.target + Vec3{1.0f, 0.0f, 0.0f});
  CHECK(b.x - a.x == Approx(ppu).epsilon(1e-4));
}

TEST_CASE("a shake below the pixel threshold is not drawn", "[presentation][camera]") {
  const Camera cam = makeArenaCamera(arena(), 1280.0f, 720.0f);
  const float ppu = pixelsPerUnitAtTarget(cam);
  const Vec3 offset{0.3f, -0.2f, 0.0f};
  const float visible = 1.01f * kShakeMinPixels / ppu;
  const float subPixel = 0.99f * kShakeMinPixels / ppu;
  const Vec3 drawn = visibleShakeOffset(cam, visible, offset);
  CHECK(drawn.x == offset.x);
  CHECK(drawn.y == offset.y);
  const Vec3 dropped = visibleShakeOffset(cam, subPixel, offset);
  CHECK(dropped.x == 0.0f);
  CHECK(dropped.y == 0.0f);
  CHECK(dropped.z == 0.0f);
  // The same amplitude is visible on a framebuffer twice the size.
  const Camera large = makeArenaCamera(arena(), 2560.0f, 1440.0f);
  CHECK(visibleShakeOffset(large, subPixel, offset).x == offset.x);
}
