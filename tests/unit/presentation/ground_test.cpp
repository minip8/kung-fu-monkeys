// The ground program's layout (architecture v0.54, §7 "Look";
// presentation/ground.hpp): the land cut into boxes that cover exactly the
// arena minus its pits, corners rounded only where they are free, touching
// boxes grouped and their seams bridged inside the union.
#include "kfm/presentation/ground.hpp"

#include "kfm/presentation/builder.hpp"
#include "kfm/presentation/theme.hpp"
#include "kfm/presentation/tuning.hpp"
#include "kfm/presentation/world_basis.hpp"
#include "kfm/sim/content.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <span>

using namespace kfm;
using namespace kfm::presentation;
using namespace kfm::sim;
using Catch::Approx;

namespace {

[[nodiscard]] Vec2 at(float x, float y) {
  return Vec2{Fixed::fromRaw(static_cast<int32_t>(x * 65536.0f)),
              Fixed::fromRaw(static_cast<int32_t>(y * 65536.0f))};
}

[[nodiscard]] bool inside(const GroundBox& b, Vec2f p) {
  return std::fabs(p.x - b.center.x) < b.half.x && std::fabs(p.y - b.center.y) < b.half.y;
}

[[nodiscard]] bool insideClosed(const GroundBox& b, Vec2f p) {
  return std::fabs(p.x - b.center.x) <= b.half.x + 1e-4f &&
         std::fabs(p.y - b.center.y) <= b.half.y + 1e-4f;
}

struct Built {
  std::unique_ptr<RenderState> state = std::make_unique<RenderState>();
  GroundLayout layout;
};

[[nodiscard]] Built layoutOf(const ArenaDef& arena, const ArenaLook& look = kJungleLook) {
  Built b;
  buildArenaPieces(arena, *b.state);
  b.state->arenaHalfExtents = toFloat(arena.halfExtents);
  b.layout = buildGroundLayout(*b.state, look);
  return b;
}

[[nodiscard]] std::span<const GroundBox> landBoxes(const GroundLayout& g) {
  return std::span(g.boxes).first(g.landCount - g.landBridges);
}
[[nodiscard]] std::span<const GroundBox> landBridges(const GroundLayout& g) {
  return std::span(g.boxes).subspan(g.landCount - g.landBridges, g.landBridges);
}
[[nodiscard]] std::span<const GroundBox> slowBoxes(const GroundLayout& g) {
  return std::span(g.boxes).subspan(g.landCount, g.slowCount - g.slowBridges);
}

[[nodiscard]] ArenaDef islandArena() {
  ArenaDef a = defaultContent().arenas[0];
  a.halfExtents = at(16, 9);
  a.hazards[0] = Hazard{at(-16, 6), at(16, 9), HazardKind::Pit, Fixed::one()};
  a.hazards[1] = Hazard{at(-16, -9), at(16, -6), HazardKind::Pit, Fixed::one()};
  a.hazards[2] = Hazard{at(-16, -6), at(-11, 6), HazardKind::Pit, Fixed::one()};
  a.hazards[3] = Hazard{at(11, -6), at(16, 6), HazardKind::Pit, Fixed::one()};
  a.hazards[4] = Hazard{at(-7, 2.5f), at(-4, 5), HazardKind::Slow, Fixed::half()};
  a.hazards[5] = Hazard{at(4, -5), at(7, -2.5f), HazardKind::Slow, Fixed::half()};
  a.hazardCount = 6;
  a.walls[0] = Wall{at(-1.5f, -1), at(1.5f, 1)};
  a.wallCount = 1;
  return a;
}

// Every sample point of the arena is land exactly when no pit holds it.
void checkCoversLand(const ArenaDef& arena, const GroundLayout& g) {
  const Vec2f half = toFloat(arena.halfExtents);
  int wrong = 0;
  const int rows = static_cast<int>((2.0f * half.y) / 0.37f);
  const int cols = static_cast<int>((2.0f * half.x) / 0.41f);
  for (int r = 0; r < rows; ++r) {
    const float y = -half.y + 0.13f + (0.37f * static_cast<float>(r));
    for (int q = 0; q < cols; ++q) {
      const float x = -half.x + 0.11f + (0.41f * static_cast<float>(q));
      bool pit = false;
      for (uint8_t i = 0; i < arena.hazardCount; ++i) {
        const Hazard& h = arena.hazards[i];
        if (h.kind != HazardKind::Pit) continue;
        const Vec2f lo = toFloat(h.min);
        const Vec2f hi = toFloat(h.max);
        pit = pit || (x > lo.x && x < hi.x && y > lo.y && y < hi.y);
      }
      int holders = 0;
      for (const GroundBox& b : landBoxes(g))
        holders += inside(b, {x, y}) ? 1 : 0;
      if (holders != (pit ? 0 : 1)) ++wrong;
    }
  }
  CHECK(wrong == 0);
}

// A bridge lies inside the union of the boxes it joins.
void checkBridgesInside(std::span<const GroundBox> boxes, std::span<const GroundBox> bridges) {
  for (const GroundBox& br : bridges) {
    for (int iu = -4; iu <= 4; ++iu) {
      for (int iv = -4; iv <= 4; ++iv) {
        const float u = 0.25f * static_cast<float>(iu);
        const float v = 0.25f * static_cast<float>(iv);
        const Vec2f p{br.center.x + (u * br.half.x), br.center.y + (v * br.half.y)};
        bool held = false;
        for (const GroundBox& b : boxes)
          held = held || insideClosed(b, p);
        CHECK(held);
      }
    }
  }
}

}  // namespace

TEST_CASE("the island's land is one rounded box, its mud rounded pills", "[presentation][ground]") {
  const ArenaDef arena = islandArena();
  const Built b = layoutOf(arena);
  const GroundLayout& g = b.layout;
  CHECK_FALSE(g.landFromPits);
  REQUIRE(g.landCount == 1);
  CHECK(g.landBridges == 0);
  const GroundBox& land = g.boxes[0];
  CHECK(land.center.x == Approx(0.0f).margin(1e-4));
  CHECK(land.center.y == Approx(0.0f).margin(1e-4));
  CHECK(land.half.x == Approx(11.0f));
  CHECK(land.half.y == Approx(6.0f));
  for (const float r : land.radii)
    CHECK(r == Approx(kJungleLook.cornerRadius));
  checkCoversLand(arena, g);

  REQUIRE(g.slowCount == 2);
  CHECK(g.slowBridges == 0);
  for (const GroundBox& m : slowBoxes(g)) {
    for (const float r : m.radii)
      CHECK(r == Approx(std::fmin(kJungleLook.slowRadius, 1.25f)));
  }
  // Two separate patches: two groups, joined with a plain minimum.
  CHECK(g.boxes[1].group != g.boxes[2].group);

  REQUIRE(g.wallCount == 1);
  CHECK(g.walls[0].half.x == Approx(1.5f));
  CHECK(g.walls[0].half.y == Approx(1.0f));
  CHECK(g.walls[0].radii[0] == Approx(wallCornerRadius(kJungleLook, {1.5f, 1.0f})));
}

TEST_CASE("a notch in the edge cuts the land into grouped, bridged boxes",
          "[presentation][ground]") {
  // The courtyard's shape: a 2 x 1 pit against the north edge.
  ArenaDef a = defaultContent().arenas[0];
  a.halfExtents = at(12, 7);
  a.hazards[0] = Hazard{at(-1, 6), at(1, 7), HazardKind::Pit, Fixed::one()};
  a.hazards[1] = Hazard{at(-1, -7), at(1, -6), HazardKind::Pit, Fixed::one()};
  a.hazardCount = 2;
  a.wallCount = 0;
  const Built b = layoutOf(a, kTempleLook);
  const GroundLayout& g = b.layout;
  CHECK_FALSE(g.landFromPits);
  REQUIRE(g.landCount > g.landBridges);
  CHECK(g.landBridges > 0);
  checkCoversLand(a, g);
  checkBridgesInside(landBoxes(g), landBridges(g));
  // One piece of land: one group.
  for (const GroundBox& box : landBoxes(g))
    CHECK(box.group == g.boxes[0].group);
  // The land's convex corners are rounded (the arena's corners and where the
  // notch meets the edge); a corner inside another box stays square.
  const auto radiusAt = [&](Vec2f corner) {
    for (const GroundBox& box : landBoxes(g)) {
      for (std::size_t k = 0; k < 4; ++k) {
        const float sx = k < 2 ? 1.0f : -1.0f;
        const float sy = (k % 2 == 0) ? 1.0f : -1.0f;
        const Vec2f c{box.center.x + (sx * box.half.x), box.center.y + (sy * box.half.y)};
        bool inOther = false;
        for (const GroundBox& o : landBoxes(g))
          inOther = inOther || (&o != &box && insideClosed(o, c));
        if (std::fabs(c.x - corner.x) < 1e-4f && std::fabs(c.y - corner.y) < 1e-4f && !inOther)
          return box.radii[k];
      }
    }
    return -1.0f;
  };
  CHECK(radiusAt({-12.0f, 7.0f}) == Approx(kTempleLook.cornerRadius));
  CHECK(radiusAt({12.0f, -7.0f}) == Approx(kTempleLook.cornerRadius));
  CHECK(radiusAt({-1.0f, 7.0f}) == Approx(kTempleLook.cornerRadius));
  CHECK(radiusAt({1.0f, -7.0f}) == Approx(kTempleLook.cornerRadius));
}

TEST_CASE("adjoining slow patches join as one group with square shared corners",
          "[presentation][ground]") {
  ArenaDef a = defaultContent().arenas[0];
  // An L of mud, as a tilemap merges it: a 2 x 2 box and a 2 x 1 box under it.
  a.hazards[0] = Hazard{at(2, 2), at(4, 4), HazardKind::Slow, Fixed::half()};
  a.hazards[1] = Hazard{at(2, 1), at(6, 2), HazardKind::Slow, Fixed::half()};
  // And one apart from it.
  a.hazards[2] = Hazard{at(-8, -3), at(-6, -1), HazardKind::Slow, Fixed::half()};
  a.hazardCount = 3;
  a.wallCount = 0;
  const Built b = layoutOf(a);
  const GroundLayout& g = b.layout;
  CHECK(g.landCount == 1);  // no pits: the land is the arena
  REQUIRE(g.slowCount - g.slowBridges == 3);
  CHECK(g.slowBridges == 1);
  const std::span<const GroundBox> mud = slowBoxes(g);
  // Sorted by group: the L's two boxes first, then the lone patch.
  CHECK(mud[0].group == mud[1].group);
  CHECK(mud[2].group != mud[0].group);
  // The 2 x 2 box's bottom corners lie on the other box: square.
  const GroundBox& square = mud[0].half.y > 0.75f ? mud[0] : mud[1];
  CHECK(square.radii[1] == 0.0f);  // (+x, -y)
  CHECK(square.radii[3] == 0.0f);  // (-x, -y)
  CHECK(square.radii[0] > 0.0f);
  CHECK(square.radii[2] > 0.0f);
  checkBridgesInside(mud.first(2), std::span(g.boxes).subspan(g.landCount + 3, 1));
}

TEST_CASE("land that does not fit falls back to the pits", "[presentation][ground]") {
  ArenaDef a = defaultContent().arenas[0];
  a.halfExtents = at(16, 9);
  // 16 small pits on a diagonal, no two sharing a row or a column of the
  // cut grid: the land cuts into far more boxes than fit.
  uint8_t n = 0;
  for (int k = 0; k < 16; ++k) {
    const float x = -15.0f + (1.9f * static_cast<float>(k));
    const float y = -8.5f + (1.05f * static_cast<float>(k));
    a.hazards[n++] = Hazard{at(x, y), at(x + 0.5f, y + 0.5f), HazardKind::Pit, Fixed::one()};
  }
  a.hazardCount = n;
  a.wallCount = 0;
  const Built b = layoutOf(a);
  const GroundLayout& g = b.layout;
  CHECK(g.landFromPits);
  CHECK(g.landCount - g.landBridges == 16);
  CHECK(g.slowCount == 0);
}

TEST_CASE("every theme's walls are rounded within their box", "[presentation][ground]") {
  for (const ArenaTheme t : {ArenaTheme::Jungle, ArenaTheme::Temple, ArenaTheme::Beach}) {
    const ArenaLook& look = arenaLook(t);
    CHECK(look.wallBevel <= look.wallRadius);
    CHECK(wallCornerRadius(look, {0.3f, 5.5f}) <= kWallRadiusShare * 0.3f + 1e-6f);
    CHECK(wallCornerRadius(look, {2.0f, 2.0f}) == Approx(look.wallRadius));
  }
}

TEST_CASE("half floats round-trip the field's range", "[presentation][ground]") {
  for (const float f : {0.0f, 1.0f, -1.0f, 0.5f, 0.0625f, -0.37f, 7.99f, -8.0f, 1e-5f}) {
    INFO(f);
    CHECK(fromHalf(toHalf(f)) == Approx(f).margin(std::fabs(f) / 1024.0f + 1e-7f));
  }
  CHECK(toHalf(1.0f) == 0x3C00);
  CHECK(toHalf(-2.0f) == 0xC000);
  CHECK(toHalf(65536.0f) == 0x7C00);  // overflow: infinity
}

TEST_CASE("the distance fields are signed distances to the layout", "[presentation][ground]") {
  const ArenaDef arena = islandArena();
  const Built b = layoutOf(arena);
  const GroundLayout& g = b.layout;
  const ArenaLook& look = kJungleLook;
  // The land: the island box [-11, 11] x [-6, 6], its corners rounded.
  CHECK(groundDistances(g, look, {0.0f, 0.0f}).x == Approx(-6.0f).margin(1e-4));
  CHECK(groundDistances(g, look, {0.0f, 7.0f}).x == Approx(1.0f).margin(1e-4));
  CHECK(groundDistances(g, look, {-13.0f, 0.0f}).x == Approx(2.0f).margin(1e-4));
  const float r = look.cornerRadius;
  const float corner = groundDistances(g, look, {-11.0f, 6.0f}).x;
  CHECK(corner == Approx(r * (std::sqrt(2.0f) - 1.0f)).margin(1e-4));
  // The mud: [-7, -4] x [2.5, 5]; the wall: [-1.5, 1.5] x [-1, 1].
  CHECK(groundDistances(g, look, {-5.5f, 3.75f}).y == Approx(-1.25f).margin(1e-4));
  CHECK(groundDistances(g, look, {0.0f, 3.0f}).z == Approx(2.0f).margin(1e-4));
  // The shadow channel is the wall moved toward the camera (sim -y).
  CHECK(groundDistances(g, look, {0.0f, -1.0f - kWallShadowOffset}).w == Approx(0.0f).margin(1e-4));

  const GroundField f = buildGroundField(g, look);
  CHECK(f.width == static_cast<int>(std::ceil(f.size.x * kGroundFieldTexelsPerUnit)));
  CHECK(f.size.x == Approx(32.0f + 2.0f * kGroundFieldMargin));
  REQUIRE(f.texels.size() == static_cast<std::size_t>(f.width * f.height * 4));
  // Every texel holds its centre's distances, clamped to the range.
  for (const int j : {0, f.height / 2, f.height - 1}) {
    for (const int i : {0, f.width / 3, f.width - 1}) {
      const Vec2f p{
          f.origin.x + ((static_cast<float>(i) + 0.5f) / static_cast<float>(f.width)) * f.size.x,
          f.origin.y + ((static_cast<float>(j) + 0.5f) / static_cast<float>(f.height)) * f.size.y};
      const Vec4 d = groundDistances(g, look, p);
      const std::size_t at =
          4 * ((static_cast<std::size_t>(j) * static_cast<std::size_t>(f.width)) +
               static_cast<std::size_t>(i));
      const float want = std::fmin(std::fmax(d.x, -kGroundFieldRange), kGroundFieldRange);
      CHECK(fromHalf(f.texels[at]) == Approx(want).margin(0.01f));
    }
  }
}

TEST_CASE("a seam between land boxes is deep inside the land", "[presentation][ground]") {
  // The courtyard's notch splits the land into boxes that meet along y = 6:
  // the bridges keep the seam's distance well inside, so no ink line or
  // shallows band shows across it.
  ArenaDef a = defaultContent().arenas[0];
  a.halfExtents = at(12, 7);
  a.hazards[0] = Hazard{at(-1, 6), at(1, 7), HazardKind::Pit, Fixed::one()};
  a.hazardCount = 1;
  a.wallCount = 0;
  const Built b = layoutOf(a, kBeachLook);
  for (const float x : {-8.0f, -4.0f, 4.0f, 8.0f})
    CHECK(groundDistances(b.layout, kBeachLook, {x, 6.0f}).x < -0.9f);
  // And the notch is water: positive land distance in its middle.
  CHECK(groundDistances(b.layout, kBeachLook, {0.0f, 6.5f}).x > 0.4f);
}
