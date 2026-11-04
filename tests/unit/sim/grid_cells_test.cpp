// The grid's cell mapping and the eight-neighbour quantisation (architecture
// §5.4, §5.12, v0.34): kfm/sim/spaces/grid_cells.hpp.
#include "kfm/sim/spaces/grid_cells.hpp"

#include "systems/angle_of.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::grid;

namespace {

// An arena of cols × rows cells of `cell` (raw Q16.16), centred on the
// origin like a tilemap.
ArenaDef cellArena(int32_t cols, int32_t rows, int32_t cellRaw) {
  ArenaDef a;
  a.cellSize = Fixed::fromRaw(cellRaw);
  a.halfExtents = Vec2{Fixed::fromRaw(cols * cellRaw / 2), Fixed::fromRaw(rows * cellRaw / 2)};
  return a;
}

// The octant nearest a direction of `steps` / 256 of a turn, ties to the
// counter-clockwise octant, by integer arithmetic on 256ths: octant k sits
// at 32 k.
uint8_t nearestOctant(uint32_t steps) {
  uint8_t best = 0;
  int32_t bestDist = 1 << 20;
  for (uint8_t k = 0; k < 8; ++k) {
    int32_t d = static_cast<int32_t>(steps) - (32 * k);
    d = ((d % 256) + 256) % 256;
    if (d > 128) d -= 256;  // signed distance in (-128, 128]
    const int32_t dist = std::abs(d);
    // A tie (dist 16) lies half-way: the octant counter-clockwise of the
    // direction wins, i.e. the one the direction sits below (d < 0).
    if (dist < bestDist || (dist == bestDist && d < 0)) {
      best = k;
      bestDist = dist;
    }
  }
  return best;
}

}  // namespace

TEST_CASE("cellOf and cellCentre round-trip on every cell", "[sim][grid]") {
  for (const ArenaDef& a :
       {cellArena(32, 18, Fixed::kOneRaw), cellArena(7, 5, Fixed::kOneRaw),
        cellArena(9, 4, Fixed::kOneRaw / 2), cellArena(6, 6, 3 * Fixed::kOneRaw)}) {
    REQUIRE(hasCells(a));
    const int32_t cols = columns(a);
    const int32_t rws = rows(a);
    for (int32_t r = 0; r < rws; ++r) {
      for (int32_t c = 0; c < cols; ++c) {
        const Cell k{c, r};
        REQUIRE(inside(a, k));
        const Vec2 centre = cellCentre(a, k);
        CHECK(cellOf(a, centre) == k);
        // The cell's corners: the north-west one is its own, the others
        // belong to the neighbours east and south.
        const Fixed half = a.cellSize / 2;
        const Vec2 nw{centre.x - half, centre.y + half};
        CHECK(cellOf(a, nw) == k);
        CHECK(cellOf(a, Vec2{centre.x + half, centre.y}) == Cell{c + 1, r});
        CHECK(cellOf(a, Vec2{centre.x, centre.y - half}) == Cell{c, r + 1});
      }
    }
    CHECK_FALSE(inside(a, Cell{-1, 0}));
    CHECK_FALSE(inside(a, Cell{0, -1}));
    CHECK_FALSE(inside(a, Cell{cols, 0}));
    CHECK_FALSE(inside(a, Cell{0, rws}));
  }
}

TEST_CASE("cells read like the tilemap: column 0 west, row 0 north", "[sim][grid]") {
  const ArenaDef a = cellArena(32, 18, Fixed::kOneRaw);  // hilltop's 32 x 18
  CHECK(columns(a) == 32);
  CHECK(rows(a) == 18);
  CHECK(cellCentre(a, Cell{0, 0}) == Vec2{Fixed::fromRatio(-31, 2), Fixed::fromRatio(17, 2)});
  CHECK(cellCentre(a, Cell{31, 17}) == Vec2{Fixed::fromRatio(31, 2), Fixed::fromRatio(-17, 2)});
  CHECK(cellOf(a, Vec2::zero()) == Cell{16, 9});  // edges belong east and south
  CHECK(cellOf(a, Vec2{-Fixed::epsilon(), Fixed::epsilon()}) == Cell{15, 8});
  // An odd count puts a centre on the origin.
  const ArenaDef odd = cellArena(7, 5, Fixed::kOneRaw);
  CHECK(cellCentre(odd, Cell{3, 2}) == Vec2::zero());
}

TEST_CASE("an arena without cells maps everything to cell 0", "[sim][grid]") {
  ArenaDef a;
  a.halfExtents = Vec2{Fixed::fromInt(16), Fixed::fromInt(9)};
  CHECK_FALSE(hasCells(a));
  CHECK(columns(a) == 0);
  CHECK(rows(a) == 0);
  CHECK(cellOf(a, Vec2{Fixed::fromInt(5), Fixed::fromInt(-3)}) == Cell{});
  CHECK(cellCentre(a, Cell{4, 2}) == Vec2::zero());
  CHECK_FALSE(inside(a, Cell{}));
}

TEST_CASE("every moveDir quantises to its nearest octant", "[sim][grid]") {
  for (uint32_t d = 0; d < 256; ++d) {
    INFO("moveDir " << d);
    const auto dir = static_cast<uint8_t>(d);
    const uint8_t o = octantOf(dir);
    CHECK(o == nearestOctant(d));
    // The same octant from the angle InputSystem decodes, and from the
    // direction a dash recovers from its unit vector.
    CHECK(octantOfAngle(Angle::fromInput8(dir)) == o);
    CHECK(octantOfAngle(angleOf(Vec2::fromAngle(Angle::fromInput8(dir)))) == o);
  }
  CHECK(octantOf(0) == 0);
  CHECK(octantOf(15) == 0);
  CHECK(octantOf(16) == 1);  // half-way: counter-clockwise
  CHECK(octantOf(64) == 2);
  CHECK(octantOf(240) == 0);
  CHECK(octantOf(239) == 7);
}

TEST_CASE("neighbours step one cell in each octant", "[sim][grid]") {
  const Cell k{5, 5};
  CHECK(neighbour(k, 0) == Cell{6, 5});  // east
  CHECK(neighbour(k, 1) == Cell{6, 4});  // north-east: row - 1 is +y
  CHECK(neighbour(k, 2) == Cell{5, 4});  // north
  CHECK(neighbour(k, 3) == Cell{4, 4});
  CHECK(neighbour(k, 4) == Cell{4, 5});  // west
  CHECK(neighbour(k, 5) == Cell{4, 6});
  CHECK(neighbour(k, 6) == Cell{5, 6});  // south
  CHECK(neighbour(k, 7) == Cell{6, 6});
  for (uint8_t o = 0; o < kOctants; ++o) {
    CHECK(diagonal(o) == (o % 2 == 1));
    // A neighbour's centre lies in the octant's direction.
    const ArenaDef a = cellArena(11, 11, Fixed::kOneRaw);
    const Vec2 delta = cellCentre(a, neighbour(k, o)) - cellCentre(a, k);
    CHECK(octantOfAngle(angleOf(delta)) == o);
  }
}
