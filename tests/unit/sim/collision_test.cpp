// CollisionSystem (contract step 6, architecture §5.3, §5.11): grid layout,
// swept circle narrowphase against a double reference, grid broadphase
// against brute force on random worlds, contact contents and determinism.
#include "../../../engine/sim/src/systems/collision_internal.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/sim/systems.hpp"
#include "sim_test_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;
using collision::sweptCircleHit;

namespace {

constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};

Vec2 v(double x, double y) {
  return Vec2{Fixed::fromDouble(x), Fixed::fromDouble(y)};
}

Fixed fx(double d) {
  return Fixed::fromDouble(d);
}

// tHit tolerance: the integer solver forms the discriminant exactly and
// rounds tHit down, so it is within about one Q16.16 ulp of the true root.
constexpr double kToiTolerance = 2.0 / 65536.0;

struct RefHit {
  bool hit{false};
  double t{0};
  bool nearBoundary{false};  // grazing, touching at start, or touching at t = 1
};

// Double-precision reference on the same Fixed inputs.
RefHit referenceToi(Vec2 prevA, Vec2 posA, Vec2 prevB, Vec2 posB, Fixed radiusSum) {
  const double d0x = prevB.x.toDouble() - prevA.x.toDouble();
  const double d0y = prevB.y.toDouble() - prevA.y.toDouble();
  const double dvx =
      (posB.x.toDouble() - prevB.x.toDouble()) - (posA.x.toDouble() - prevA.x.toDouble());
  const double dvy =
      (posB.y.toDouble() - prevB.y.toDouble()) - (posA.y.toDouble() - prevA.y.toDouble());
  const double r = radiusSum.toDouble();
  const double eps = 1e-3;
  RefHit out;
  const double dist0 = std::hypot(d0x, d0y);
  if (dist0 < r) {
    out.hit = true;
    out.nearBoundary = dist0 > r - eps;
    return out;
  }
  const double a = (dvx * dvx) + (dvy * dvy);
  const double b = (d0x * dvx) + (d0y * dvy);
  if (a == 0 || b >= 0) {
    out.nearBoundary = dist0 < r + eps;
    return out;
  }
  const double len = std::sqrt(a);
  const double h = std::abs((d0x * dvy) - (d0y * dvx)) / len;
  if (h >= r) {
    out.nearBoundary = h < r + eps;
    return out;
  }
  const double c = (dist0 * dist0) - (r * r);
  const double t = c / (-b + std::sqrt((b * b) - (a * c)));  // cancellation-free first root
  out.hit = t <= 1.0;
  out.t = std::max(t, 0.0);
  out.nearBoundary = h > r - eps || std::abs(t - 1.0) * len < eps || dist0 < r + eps;
  return out;
}

// A player in a hand-built world (no rules involved), for broadphase tests.
void putPlayer(WorldState& s, uint16_t slot, Vec2 prev, Vec2 pos) {
  const EntityId id = s.players.spawnAt(slot);
  REQUIRE(id.isValid());
  Player& p = s.players.items[slot];
  p.id = PlayerId{static_cast<uint8_t>(slot)};
  p.transform.prevPos = prev;
  p.transform.pos = pos;
}

bool samePairs(const CandidatePairs& a, const CandidatePairs& b) {
  return a.count == b.count && std::equal(a.begin(), a.end(), b.begin());
}

bool sameContacts(const Contacts& a, const Contacts& b) {
  if (a.count != b.count || a.dropped != b.dropped) return false;
  for (uint16_t i = 0; i < a.count; ++i) {
    const Contact& x = a.items[i];
    const Contact& y = b.items[i];
    if (x.kindA != y.kindA || x.a != y.a || x.kindB != y.kindB || x.b != y.b || x.tHit != y.tHit ||
        x.point != y.point || x.normal != y.normal) {
      return false;
    }
  }
  return true;
}

}  // namespace

// --- grid layout --------------------------------------------------------------

TEST_CASE("the default arena gets a 2-unit grid within the cell budget", "[sim][collision]") {
  constexpr collision::GridLayout kM1Grid = collision::gridLayout(Vec2{16_fx, 9_fx});
  static_assert(kM1Grid.nx == 16 && kM1Grid.ny == 9 && kM1Grid.cellRaw == collision::kBaseCellRaw);

  const collision::GridLayout g = collision::gridLayout(defaultContent().arena.halfExtents);
  CHECK(g.cellRaw == collision::kBaseCellRaw);
  CHECK(g.nx == 16);
  CHECK(g.ny == 9);
  CHECK(g.cellX(fx(-16)) == 0);
  CHECK(g.cellX(fx(-14.01)) == 0);
  CHECK(g.cellX(fx(-14)) == 1);
  CHECK(g.cellX(fx(15.99)) == 15);
  CHECK(g.cellY(fx(8.99)) == 8);
  // Outside the box clamps into the edge cells.
  CHECK(g.cellX(fx(-40)) == 0);
  CHECK(g.cellX(fx(17)) == 15);
  CHECK(g.cellY(fx(-9.5)) == 0);
  CHECK(g.cellY(fx(30)) == 8);
}

TEST_CASE("a large arena scales the cell size to fit the grid", "[sim][collision]") {
  const collision::GridLayout g = collision::gridLayout(Vec2{300_fx, 20_fx});
  CHECK(g.cellRaw > collision::kBaseCellRaw);
  CHECK(g.nx <= collision::kMaxGridAxisCells);
  CHECK(g.ny >= 1);
  CHECK(static_cast<int64_t>(g.nx) * g.cellRaw >= int64_t{600} * Fixed::kOneRaw);

  const collision::GridLayout empty = collision::gridLayout(Vec2{});
  CHECK(empty.nx == 1);
  CHECK(empty.ny == 1);
  CHECK(empty.cellX(fx(5)) == 0);
}

// --- swept narrowphase ----------------------------------------------------------

TEST_CASE("swept circles: head-on time of impact", "[sim][collision]") {
  // B travels from x = 5 to x = -5 toward a static A at the origin; R = 1, so
  // they touch when B reaches x = 1: 4 units into a 10-unit move.
  const auto hit = sweptCircleHit(v(0, 0), v(0, 0), v(5, 0), v(-5, 0), 1_fx);
  REQUIRE(hit.hit);
  CHECK(std::abs(hit.tHit.toDouble() - 0.4) <= 2.0 / 65536.0);

  // Both moving: centres 4 apart closing at 6 per tick, touch after 3 -> t = 0.5.
  const auto both = sweptCircleHit(v(-2, 0), v(1, 0), v(2, 0), v(-1, 0), 1_fx);
  REQUIRE(both.hit);
  CHECK(std::abs(both.tHit.toDouble() - 0.5) <= 2.0 / 65536.0);
}

TEST_CASE("swept circles: misses", "[sim][collision]") {
  // Passes beside A with clearance.
  CHECK_FALSE(sweptCircleHit(v(0, 0), v(0, 0), v(5, 2), v(-5, 2), 1_fx).hit);
  // Moving away.
  CHECK_FALSE(sweptCircleHit(v(0, 0), v(0, 0), v(2, 0), v(5, 0), 1_fx).hit);
  // Approaching, but first touch is after this tick (B stops at x = 2).
  CHECK_FALSE(sweptCircleHit(v(0, 0), v(0, 0), v(5, 0), v(2, 0), 1_fx).hit);
  // No relative motion and apart.
  CHECK_FALSE(sweptCircleHit(v(0, 0), v(1, 1), v(3, 0), v(4, 1), 1_fx).hit);
  // Exactly grazing (closest approach == R) is not a hit.
  CHECK_FALSE(sweptCircleHit(v(0, 0), v(0, 0), v(5, 1), v(-5, 1), 1_fx).hit);
}

TEST_CASE("swept circles: a fast banana cannot tunnel through a player", "[sim][collision]") {
  // Moves 6 units in one tick straight through a player of combined radius 0.85;
  // neither endpoint overlaps.
  const Fixed r = defaultContent().player.radius + defaultContent().boomerang.radius;
  const auto hit = sweptCircleHit(v(0, 0), v(0, 0), v(-3, 0), v(3, 0), r);
  REQUIRE(hit.hit);
  CHECK(std::abs(hit.tHit.toDouble() - ((3.0 - r.toDouble()) / 6.0)) <= 1e-4);
}

TEST_CASE("swept circles: overlapping at tick start hits at t = 0", "[sim][collision]") {
  const auto still = sweptCircleHit(v(0, 0), v(0, 0), v(0.5, 0), v(0.5, 0), 1_fx);
  CHECK(still.hit);
  CHECK(still.tHit == Fixed::zero());
  // Even when separating.
  const auto leaving = sweptCircleHit(v(0, 0), v(0, 0), v(0.5, 0), v(3, 0), 1_fx);
  CHECK(leaving.hit);
  CHECK(leaving.tHit == Fixed::zero());
}

TEST_CASE("swept circles agree with a double reference on random inputs", "[sim][collision]") {
  Rng rng = Rng::seeded(0x5EE9);
  int hits = 0;
  int checked = 0;
  for (int i = 0; i < 20000; ++i) {
    const Fixed span = rng.below(4) == 0 ? 15_fx : 2_fx;  // some very fast movers
    const auto pt = [&](Fixed lo, Fixed hi) {
      return Vec2{rng.between(lo, hi), rng.between(lo, hi)};
    };
    const Vec2 prevA = pt(-20_fx, 20_fx);
    const Vec2 posA = prevA + pt(-span / 4, span / 4);
    const Vec2 prevB = prevA + pt(-span, span);
    const Vec2 posB = prevB + pt(-span, span);
    const Fixed r = rng.between(Fixed::fromRatio(1, 10), 2_fx);

    const auto got = sweptCircleHit(prevA, posA, prevB, posB, r);
    const RefHit want = referenceToi(prevA, posA, prevB, posB, r);
    const Vec2 dv = (posB - prevB) - (posA - prevA);
    const double speed = std::hypot(dv.x.toDouble(), dv.y.toDouble());
    INFO("case " << i << " r " << r.toDouble() << " speed " << speed);
    if (!want.nearBoundary) {
      REQUIRE(got.hit == want.hit);
      ++checked;
    }
    if (got.hit && want.hit) {
      ++hits;
      CHECK(got.tHit >= Fixed::zero());
      CHECK(got.tHit <= Fixed::one());
      CHECK(std::abs(got.tHit.toDouble() - want.t) <= kToiTolerance);
    }
  }
  CHECK(checked > 19000);
  CHECK(hits > 1000);
}

// --- collisionSystem ----------------------------------------------------------

TEST_CASE("collisionSystem reports a banana hitting a player with its contact data",
          "[sim][collision]") {
  const ContentDB& c = defaultContent();
  WorldState s = makePlayingWorld(1, 0b11);
  const Player& p = placePlayer(s, kP1, v(2, 1));
  const EntityId b = spawnBanana(s, kP0, v(2, 1), v(0, 0));
  Boomerang& bb = s.boomerangs.items[b.index];
  bb.transform.prevPos = v(-1, 1);  // moved 3 units right this tick, through the player
  placePlayer(s, kP0, v(-8, -5));

  Contacts out;
  collisionSystem(s, c, out);
  REQUIRE(out.count == 1);
  const Contact& k = out.items[0];
  CHECK(k.kindA == EntityKind::Player);
  CHECK(k.a == s.players.idAt(1));
  CHECK(k.kindB == EntityKind::Boomerang);
  CHECK(k.b == b);
  const double r = (c.player.radius + c.boomerang.radius).toDouble();
  const double wantT = (3.0 - r) / 3.0;
  CHECK(std::abs(k.tHit.toDouble() - wantT) <= 1e-4);
  // point = banana position at tHit, normal from the player toward it.
  CHECK(std::abs(k.point.x.toDouble() - (2.0 - r)) <= 1e-3);
  CHECK(std::abs(k.point.y.toDouble() - 1.0) <= 1e-6);
  CHECK(std::abs(k.normal.x.toDouble() + 1.0) <= 1e-3);
  CHECK(std::abs(k.normal.y.toDouble()) <= 1e-3);
  (void)p;
}

TEST_CASE("collisionSystem: a player walking onto a static pickup", "[sim][collision]") {
  const ContentDB& c = defaultContent();
  WorldState s = makePlayingWorld(1, 0b1);
  Player& p = placePlayer(s, kP0, v(1.5, 0));
  p.transform.prevPos = v(-1, 0);
  const EntityId k = spawnPickup(s, v(2, 0));
  spawnPickup(s, v(-10, 5));  // far away, no contact

  Contacts out;
  collisionSystem(s, c, out);
  REQUIRE(out.count == 1);
  CHECK(out.items[0].kindB == EntityKind::Pickup);
  CHECK(out.items[0].b == k);
  CHECK(out.items[0].point == v(2, 0));
  CHECK(out.items[0].normal == Vec2{Fixed::one(), Fixed::zero()});
  // The player covers 2.5 units and first touches the pickup at x = 2 - r.
  const double r = (c.player.radius + c.boomerang.radius).toDouble();
  CHECK(std::abs(out.items[0].tHit.toDouble() - ((3.0 - r) / 2.5)) <= 1e-4);
}

TEST_CASE("collisionSystem: coincident centres give a fallback normal", "[sim][collision]") {
  WorldState s = makePlayingWorld(1, 0b1);
  placePlayer(s, kP0, v(3, 3));
  spawnBanana(s, kP0, v(3, 3), v(0, 0), BoomerangPhase::Grounded);
  Contacts out;
  collisionSystem(s, defaultContent(), out);
  REQUIRE(out.count == 1);
  CHECK(out.items[0].tHit == Fixed::zero());
  CHECK(out.items[0].normal == Vec2{Fixed::one(), Fixed::zero()});
}

TEST_CASE("collisionSystem orders contacts by (player, kind, index)", "[sim][collision]") {
  WorldState s = makePlayingWorld(1, 0b11);
  placePlayer(s, kP0, v(5, 0));
  placePlayer(s, kP1, v(-5, 0));
  spawnPickup(s, v(-5, 0.5));                 // pickup 0 touches P1
  spawnBanana(s, kP0, v(5, 0.2), v(0, 0));    // banana 0 touches P0
  spawnBanana(s, kP0, v(-5, -0.2), v(0, 0));  // banana 1 touches P1
  spawnPickup(s, v(5, -0.5));                 // pickup 1 touches P0
  Contacts out;
  collisionSystem(s, defaultContent(), out);
  REQUIRE(out.count == 4);
  CHECK(out.items[0].a.index == 0);
  CHECK(out.items[0].kindB == EntityKind::Boomerang);
  CHECK(out.items[1].a.index == 0);
  CHECK(out.items[1].kindB == EntityKind::Pickup);
  CHECK(out.items[2].a.index == 1);
  CHECK(out.items[2].kindB == EntityKind::Boomerang);
  CHECK(out.items[3].a.index == 1);
  CHECK(out.items[3].kindB == EntityKind::Pickup);
}

TEST_CASE("collisionSystem with no players or no targets reports nothing", "[sim][collision]") {
  WorldState empty{};
  spawnBanana(empty, kP0, v(0, 0), v(0, 0));
  Contacts out;
  collisionSystem(empty, defaultContent(), out);
  CHECK(out.count == 0);

  const WorldState alone = makePlayingWorld(1, 0b11);
  collisionSystem(alone, defaultContent(), out);
  CHECK(out.count == 0);
}

// --- broadphase property test ---------------------------------------------------

TEST_CASE("grid broadphase equals brute force on random worlds", "[sim][collision]") {
  ContentDB big = defaultContent();
  big.arena.halfExtents = Vec2{200_fx, 40_fx};  // forces a scaled cell size
  Rng rng = Rng::seeded(0xB20AD);
  int totalPairs = 0;
  for (int world = 0; world < 300; ++world) {
    const ContentDB& c = (world % 5 == 4) ? big : defaultContent();
    const Fixed hx = c.arena.halfExtents.x;
    const Fixed hy = c.arena.halfExtents.y;
    // Mostly inside the box, sometimes exactly on a wall or a little outside.
    const auto coord = [&](Fixed half) {
      switch (rng.below(8)) {
        case 0:
          return half;
        case 1:
          return -half;
        case 2:
          return half + rng.between(Fixed::zero(), 2_fx);
        case 3:
          return -half - rng.between(Fixed::zero(), 2_fx);
        default:
          return rng.between(-half, half);
      }
    };
    const auto point = [&] { return Vec2{coord(hx), coord(hy)}; };
    // Usually a short move; sometimes the previous position is far away.
    const auto prevOf = [&](Vec2 pos) {
      if (rng.below(6) == 0) return point();
      return pos + Vec2{rng.between(-1_fx, 1_fx), rng.between(-1_fx, 1_fx)};
    };

    WorldState s{};
    const uint32_t playerMask = rng.below(1u << kMaxPlayers);
    for (uint16_t slot = 0; slot < kMaxPlayers; ++slot) {
      if ((playerMask & (1u << slot)) == 0) continue;
      const Vec2 pos = point();
      putPlayer(s, slot, prevOf(pos), pos);
    }
    const uint32_t nb = rng.below(kMaxBoomerangs + 1);
    for (uint32_t i = 0; i < nb; ++i) {
      const Vec2 pos = point();
      const EntityId id = spawnBanana(s, kP0, pos, Vec2::zero());
      s.boomerangs.items[id.index].transform.prevPos = prevOf(pos);
    }
    const uint32_t nk = rng.below(kMaxPickups + 1);
    for (uint32_t i = 0; i < nk; ++i)
      spawnPickup(s, point());
    // Punch holes so pool indices are sparse.
    for (int i = 0; i < 6; ++i) {
      const auto bi = static_cast<uint16_t>(rng.below(kMaxBoomerangs));
      if (s.boomerangs.isAliveIndex(bi)) s.boomerangs.despawn(s.boomerangs.idAt(bi));
      const auto ki = static_cast<uint16_t>(rng.below(kMaxPickups));
      if (s.pickups.isAliveIndex(ki)) s.pickups.despawn(s.pickups.idAt(ki));
    }

    CandidatePairs grid;
    CandidatePairs brute;
    collisionBroadphaseGrid(s, c, grid);
    collisionBroadphaseBruteForce(s, c, brute);
    INFO("world " << world);
    REQUIRE(grid.dropped == 0);
    REQUIRE(brute.dropped == 0);
    REQUIRE(samePairs(grid, brute));
    REQUIRE(std::ranges::is_sorted(grid));
    REQUIRE(std::ranges::adjacent_find(grid) == grid.end());
    totalPairs += grid.count;
  }
  CHECK(totalPairs > 500);  // the test exercised real overlaps
}

TEST_CASE("broadphase pairs entities touching exactly at a box edge", "[sim][collision]") {
  const ContentDB& c = defaultContent();
  const Fixed r = c.player.radius + c.boomerang.radius;
  WorldState s{};
  putPlayer(s, 3, v(0, 0), v(0, 0));
  // Banana box min x == player box max x.
  spawnBanana(s, kP0, Vec2{r, Fixed::zero()}, Vec2::zero());
  CandidatePairs grid;
  CandidatePairs brute;
  collisionBroadphaseGrid(s, c, grid);
  collisionBroadphaseBruteForce(s, c, brute);
  REQUIRE(brute.count == 1);
  CHECK(samePairs(grid, brute));
  CHECK(grid.items[0] == CandidatePair{EntityKind::Player, 3, EntityKind::Boomerang, 0});
}

// --- determinism ----------------------------------------------------------------

TEST_CASE("collisionSystem is deterministic and reads state only", "[sim][collision]") {
  WorldState s = makePlayingWorld(3, 0b1111);
  Rng rng = Rng::seeded(17);
  for (uint8_t slot = 0; slot < 4; ++slot) {
    Player& p =
        placePlayer(s, PlayerId{slot}, Vec2{rng.between(-3_fx, 3_fx), rng.between(-3_fx, 3_fx)});
    p.transform.prevPos =
        p.transform.pos - Vec2{rng.between(-1_fx, 1_fx), rng.between(-1_fx, 1_fx)};
  }
  for (int i = 0; i < 30; ++i) {
    const Vec2 pos{rng.between(-4_fx, 4_fx), rng.between(-4_fx, 4_fx)};
    const EntityId id = spawnBanana(s, kP0, pos, Vec2::zero());
    s.boomerangs.items[id.index].transform.prevPos =
        pos - Vec2{rng.between(-2_fx, 2_fx), rng.between(-2_fx, 2_fx)};
  }
  for (int i = 0; i < 10; ++i)
    spawnPickup(s, Vec2{rng.between(-4_fx, 4_fx), rng.between(-4_fx, 4_fx)});

  const WorldState copy = s;
  const uint32_t before = checksum(s);
  Contacts first;
  Contacts second;
  collisionSystem(s, defaultContent(), first);
  collisionSystem(copy, defaultContent(), second);
  CHECK(first.count > 5);
  CHECK(first.dropped == 0);
  CHECK(sameContacts(first, second));
  CHECK(checksum(s) == before);
  CHECK(checksum(s) == checksum(copy));
}
