// Procedural placeholder meshes (presentation/meshes.hpp) and the player
// palette: valid indices, unit normals, no degenerate triangles, consistent
// counter-clockwise winding, deterministic output.
#include "kfm/presentation/meshes.hpp"

#include "kfm/presentation/math.hpp"
#include "kfm/presentation/palette.hpp"
#include "kfm/presentation/tuning.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace kfm;
using namespace kfm::presentation;
using Catch::Approx;

namespace {

Vec3 posOf(const MeshVertex& v) {
  return {v.px, v.py, v.pz};
}
Vec3 normalOf(const MeshVertex& v) {
  return {v.nx, v.ny, v.nz};
}

struct Bounds {
  Vec3 lo{1e9f, 1e9f, 1e9f};
  Vec3 hi{-1e9f, -1e9f, -1e9f};
};

Bounds boundsOf(const MeshData& m) {
  Bounds b;
  for (const MeshVertex& v : m.vertices) {
    b.lo = Vec3{std::fmin(b.lo.x, v.px), std::fmin(b.lo.y, v.py), std::fmin(b.lo.z, v.pz)};
    b.hi = Vec3{std::fmax(b.hi.x, v.px), std::fmax(b.hi.y, v.py), std::fmax(b.hi.z, v.pz)};
  }
  return b;
}

// Every property the renderer relies on.
void checkValid(const MeshData& m, const std::string& name) {
  INFO(name);
  REQUIRE_FALSE(m.vertices.empty());
  REQUIRE(m.vertices.size() <= 65536);
  REQUIRE(m.indices.size() % 3 == 0);
  REQUIRE_FALSE(m.indices.empty());
  for (const MeshVertex& v : m.vertices) {
    CHECK(length(normalOf(v)) == Approx(1.0f).margin(1e-4));
    CHECK(std::isfinite(v.px));
    CHECK(std::isfinite(v.py));
    CHECK(std::isfinite(v.pz));
  }
  for (const uint16_t i : m.indices)
    REQUIRE(i < m.vertices.size());
  int badWinding = 0;
  int degenerate = 0;
  for (std::size_t t = 0; t < m.indices.size(); t += 3) {
    const MeshVertex& a = m.vertices[m.indices[t]];
    const MeshVertex& b = m.vertices[m.indices[t + 1]];
    const MeshVertex& c = m.vertices[m.indices[t + 2]];
    const Vec3 faceNormal = cross(posOf(b) - posOf(a), posOf(c) - posOf(a));
    const float twiceArea = length(faceNormal);
    if (twiceArea <= 1e-9f) {
      ++degenerate;
      continue;
    }
    // Counter-clockwise seen from where the vertex normals point.
    const Vec3 avgNormal = normalOf(a) + normalOf(b) + normalOf(c);
    if (dot(faceNormal, avgNormal) <= 0.0f) ++badWinding;
  }
  CHECK(degenerate == 0);
  CHECK(badWinding == 0);
}

bool sameBytes(const MeshData& a, const MeshData& b) {
  return a.vertices.size() == b.vertices.size() && a.indices == b.indices &&
         std::memcmp(a.vertices.data(), b.vertices.data(),
                     a.vertices.size() * sizeof(MeshVertex)) == 0;
}

}  // namespace

TEST_CASE("capsule mesh is valid and sized from PlayerDef", "[presentation][meshes]") {
  const sim::PlayerDef& player = sim::defaultContent().player;
  const MeshData m = makeMonkeyCapsule(player);
  checkValid(m, "monkey capsule");
  const Bounds b = boundsOf(m);
  const float r = player.radius.toFloat();
  CHECK(b.lo.y == Approx(0.0f).margin(1e-5));
  CHECK(b.hi.y == Approx(r * kMonkeyHeightOverRadius));
  CHECK(b.hi.x == Approx(r));
  CHECK(b.lo.x == Approx(-r));
  CHECK(b.hi.z == Approx(r).margin(1e-5));
  CHECK(b.lo.z == Approx(-r).margin(1e-5));
  for (const uint32_t seg : {3u, 7u, 32u}) {
    for (const uint32_t rings : {1u, 2u, 6u})
      checkValid(makeCapsule(0.4f, 1.5f, seg, rings), "capsule variant");
  }
}

TEST_CASE("banana mesh is valid, flat and spans its hitbox", "[presentation][meshes]") {
  const sim::BoomerangDef& def = sim::defaultContent().boomerang;
  const MeshData m = makeBananaMesh(def);
  checkValid(m, "banana");
  const Bounds b = boundsOf(m);
  const float r = def.radius.toFloat();
  // Tip-to-tip chord along z spans the hitbox diameter; it lies in the xz plane.
  CHECK(b.hi.z - b.lo.z == Approx(2.0f * r).margin(1e-4));
  CHECK(b.hi.y < 0.5f * r);
  CHECK(b.lo.y > -0.5f * r);
  // Bounding box roughly centred on x (the bulge points to +x).
  CHECK(std::fabs(b.hi.x + b.lo.x) < 0.25f * r);
  for (const uint32_t along : {2u, 3u, 16u}) {
    for (const uint32_t around : {3u, 5u, 12u})
      checkValid(makeBanana(1.0f, 0.12f, along, around), "banana variant");
  }
}

TEST_CASE("disc, floor quad and cube are valid", "[presentation][meshes]") {
  const MeshData disc = makeDisc(0.5f, 24);
  checkValid(disc, "disc");
  for (const MeshVertex& v : disc.vertices) {
    CHECK(v.py == 0.0f);
    CHECK(v.ny == 1.0f);
  }
  checkValid(makeDisc(1.0f, 3), "triangle disc");

  const MeshData floor = makeFloorQuad(16.0f, 9.0f);
  checkValid(floor, "floor");
  CHECK(floor.vertices.size() == 4);
  for (const MeshVertex& v : floor.vertices) {
    CHECK(v.u == v.px);  // uv is the sim-plane position: a one-unit grid
    CHECK(v.v == -v.pz);
  }

  const MeshData cube = makeUnitCube();
  checkValid(cube, "cube");
  CHECK(cube.vertices.size() == 24);
  CHECK(cube.indices.size() == 36);
  const Bounds b = boundsOf(cube);
  CHECK(b.lo == Vec3{-0.5f, -0.5f, -0.5f});
  CHECK(b.hi == Vec3{0.5f, 0.5f, 0.5f});
  // Face normals point away from the centre.
  for (const MeshVertex& v : cube.vertices)
    CHECK(dot(posOf(v), normalOf(v)) == Approx(0.5f));
}

TEST_CASE("a wall block is valid, rounded and stands on its footprint", "[presentation][meshes]") {
  const MeshData m = makeRoundedBlock(1.5f, 1.0f, kArenaWallHeight, 0.4f, 0.3f);
  checkValid(m, "rounded block");
  const Bounds b = boundsOf(m);
  CHECK(b.lo.y == Approx(0.0f).margin(1e-5));
  CHECK(b.hi.y == Approx(kArenaWallHeight));
  CHECK(b.lo.x == Approx(-1.5f));
  CHECK(b.hi.x == Approx(1.5f));
  CHECK(b.lo.z == Approx(-1.0f));
  CHECK(b.hi.z == Approx(1.0f));
  // The rounded corner keeps the footprint inside the box with the corner
  // radius cut off: no vertex lies in the square corner beyond the arc.
  for (const MeshVertex& v : m.vertices) {
    const float dx = std::fmax(std::fabs(v.px) - (1.5f - 0.4f), 0.0f);
    const float dz = std::fmax(std::fabs(v.pz) - (1.0f - 0.4f), 0.0f);
    CHECK(std::sqrt((dx * dx) + (dz * dz)) <= 0.4f + 1e-4f);
  }
  // Thin, nearly sharp and flat-topped variants (the three themes' shapes).
  checkValid(makeRoundedBlock(5.5f, 0.3f, 1.2f, 0.13f, 0.1f), "thin block");
  checkValid(makeRoundedBlock(1.0f, 1.0f, 1.2f, 0.04f, 0.04f), "sharp block");
  checkValid(makeRoundedBlock(1.0f, 2.0f, 1.2f, 0.3f, 0.0f, 2), "unbevelled block");

  // appendMesh moves a part and its indices.
  MeshData all;
  CHECK(appendMesh(all, m, Vec3{2.0f, 0.0f, -3.0f}) == 0);
  const auto second = appendMesh(all, m, Vec3{-4.0f, 0.0f, 1.0f});
  CHECK(second == m.indices.size());
  REQUIRE(all.vertices.size() == 2 * m.vertices.size());
  CHECK(all.vertices[0].px == Approx(m.vertices[0].px + 2.0f));
  CHECK(all.vertices[0].v == Approx(m.vertices[0].v + 3.0f));
  CHECK(all.indices[second] == m.indices[0] + m.vertices.size());
  checkValid(all, "two blocks");
}

TEST_CASE("mesh generation is deterministic", "[presentation][meshes]") {
  const sim::ContentDB& c = sim::defaultContent();
  CHECK(sameBytes(makeMonkeyCapsule(c.player), makeMonkeyCapsule(c.player)));
  CHECK(sameBytes(makeBananaMesh(c.boomerang), makeBananaMesh(c.boomerang)));
  CHECK(sameBytes(makeDisc(0.5f), makeDisc(0.5f)));
  CHECK(sameBytes(makeUnitCube(), makeUnitCube()));
}

TEST_CASE("the player palette has ten distinct colours", "[presentation][palette]") {
  REQUIRE(kPlayerColors.size() == sim::kMaxPlayers);
  for (std::size_t i = 0; i < kPlayerColors.size(); ++i) {
    const Color a = kPlayerColors[i];
    CHECK(a.r >= 0.0f);
    CHECK(a.r <= 1.0f);
    CHECK(a.g >= 0.0f);
    CHECK(a.g <= 1.0f);
    CHECK(a.b >= 0.0f);
    CHECK(a.b <= 1.0f);
    for (std::size_t j = i + 1; j < kPlayerColors.size(); ++j) {
      const Color b = kPlayerColors[j];
      const float d = std::sqrt(((a.r - b.r) * (a.r - b.r)) + ((a.g - b.g) * (a.g - b.g)) +
                                ((a.b - b.b) * (a.b - b.b)));
      INFO("slots " << i << " and " << j);
      CHECK(d > 0.25f);
    }
  }
  CHECK(playerColor(12).r == kPlayerColors[2].r);
}
