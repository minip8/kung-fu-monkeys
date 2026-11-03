// ProjectileSystem (contract step 5, architecture §5.7): phase timeline,
// return steering, wall bounces, grounding, counters, and the internal
// integer atan2 it steers with.
#include "../../../engine/sim/src/systems/angle_of.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/sim/step.hpp"
#include "kfm/sim/systems.hpp"
#include "sim_test_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};

EventSink tickProjectiles(WorldState& s, const ContentDB& content = defaultContent()) {
  EventSink sink;
  projectileSystem(s, content, sink);
  REQUIRE(sink.dropped == 0);
  return sink;
}

Boomerang& banana(WorldState& s, EntityId id) {
  Boomerang* b = s.boomerangs.get(id);
  REQUIRE(b != nullptr);
  return *b;
}

Vec2 v(double x, double y) {
  return Vec2{Fixed::fromDouble(x), Fixed::fromDouble(y)};
}

// Signed BAM distance between two angles.
int bamError(Angle got, Angle want) {
  return got.signedDeltaFrom(want);
}

Angle referenceAngle(Vec2 vec) {
  const double rad = std::atan2(vec.y.toDouble(), vec.x.toDouble());
  const double bam = rad / (2.0 * std::numbers::pi) * 65536.0;
  return Angle::fromBam(static_cast<uint16_t>(static_cast<int32_t>(std::lround(bam)) & 0xFFFF));
}

}  // namespace

// --- angleOf ------------------------------------------------------------------

TEST_CASE("angleOf matches the cardinal and diagonal directions", "[sim][projectile]") {
  CHECK(angleOf(Vec2::zero()).bam == 0);
  CHECK(angleOf(v(1, 0)).bam == 0);
  CHECK(angleOf(v(0, 1)).bam == Angle::kQuarter);
  CHECK(angleOf(v(-1, 0)).bam == Angle::kHalf);
  CHECK(angleOf(v(0, -1)).bam == 3 * Angle::kQuarter);
  CHECK(std::abs(bamError(angleOf(v(1, 1)), Angle::fromBam(0x2000))) <= 1);
  CHECK(std::abs(bamError(angleOf(v(-1, 1)), Angle::fromBam(0x6000))) <= 1);
  CHECK(std::abs(bamError(angleOf(v(-1, -1)), Angle::fromBam(0xA000))) <= 1);
  CHECK(std::abs(bamError(angleOf(v(1, -1)), Angle::fromBam(0xE000))) <= 1);
  // Smallest representable vectors.
  CHECK(angleOf(Vec2{Fixed::fromRaw(1), Fixed::zero()}).bam == 0);
  CHECK(angleOf(Vec2{Fixed::zero(), Fixed::fromRaw(-1)}).bam == 3 * Angle::kQuarter);
  static_assert(angleOf(Vec2{Fixed::zero(), Fixed::one()}).bam == Angle::kQuarter);
}

TEST_CASE("angleOf agrees with std::atan2 within a few BAM", "[sim][projectile]") {
  Rng rng = Rng::seeded(0xA7A2);
  int worst = 0;
  for (int i = 0; i < 20000; ++i) {
    // Magnitudes from 1/256 unit to ~1000 units, any direction.
    const int32_t scaleShift = rng.range(8, 26);
    const auto comp = [&] {
      const auto mag = static_cast<int32_t>(rng.below(uint32_t{1} << scaleShift));
      return Fixed::fromRaw(rng.below(2) == 0 ? mag : -mag);
    };
    const Vec2 vec{comp(), comp()};
    if (lengthSqRaw(vec) < (uint64_t{256} * 256)) continue;  // too short to have a precise angle
    const int err = std::abs(bamError(angleOf(vec), referenceAngle(vec)));
    worst = std::max(worst, err);
  }
  CHECK(worst <= 3);
}

// --- phases -------------------------------------------------------------------

TEST_CASE("outbound flies straight, then returns after outboundTicks", "[sim][projectile]") {
  const ContentDB& c = defaultContent();
  WorldState s = makePlayingWorld(1, 0b11);
  placePlayer(s, kP0, v(-10, 0));
  placePlayer(s, kP1, v(10, 5));
  const Vec2 vel{c.boomerang.speed, Fixed::zero()};
  const EntityId id = spawnBanana(s, kP0, v(-9, 0), vel);

  Vec2 expected = v(-9, 0);
  for (uint16_t t = 1; t < c.boomerang.outboundTicks; ++t) {
    const Vec2 before = banana(s, id).transform.pos;
    const EventSink sink = tickProjectiles(s);
    const Boomerang& b = banana(s, id);
    expected += vel;
    REQUIRE(b.phase == BoomerangPhase::Outbound);
    CHECK(b.transform.prevPos == before);
    CHECK(b.transform.pos == expected);
    CHECK(b.transform.vel == vel);
    CHECK(b.transform.facing.bam == 0);
    CHECK(b.ticksAlive == t);
    CHECK(b.ticksInPhase == t);
    CHECK(sink.count == 0);
  }
  tickProjectiles(s);
  const Boomerang& b = banana(s, id);
  CHECK(b.phase == BoomerangPhase::Returning);
  CHECK(b.ticksInPhase == 0);
  CHECK(b.ticksAlive == c.boomerang.outboundTicks);
  // The owner is straight behind: the first returning tick turns by exactly the rate.
  CHECK(b.transform.facing.signedDeltaFrom(Angle{}) ==
        -static_cast<int32_t>(c.boomerang.turnRateBam));
}

TEST_CASE("outbound facing follows the velocity heading", "[sim][projectile]") {
  WorldState s = makePlayingWorld(1, 0b1);
  const Vec2 vel = Vec2::fromAngle(Angle::fromDegrees(135), defaultContent().boomerang.speed);
  const EntityId id = spawnBanana(s, kP0, v(0, 0), vel);
  tickProjectiles(s);
  CHECK(std::abs(bamError(banana(s, id).transform.facing, Angle::fromDegrees(135))) <= 3);
}

TEST_CASE("zero outboundTicks returns on the first tick", "[sim][projectile]") {
  ContentDB c = defaultContent();
  c.boomerang.outboundTicks = 0;
  WorldState s = makePlayingWorld(1, 0b1);
  placePlayer(s, kP0, v(0, 0));
  const EntityId id = spawnBanana(s, kP0, v(3, 0), Vec2{c.boomerang.speed, Fixed::zero()});
  tickProjectiles(s, c);
  CHECK(banana(s, id).phase == BoomerangPhase::Returning);
}

TEST_CASE("returning steers toward the owner at most turnRate per tick and reaches them",
          "[sim][projectile]") {
  const ContentDB& c = defaultContent();
  const BoomerangDef& def = c.boomerang;
  for (const int deg : {0, 37, 90, 180, 251, 300}) {
    WorldState s = makePlayingWorld(1, 0b11);
    const Vec2 owner = v(-2, 1);
    placePlayer(s, kP0, owner);
    placePlayer(s, kP1, v(12, -7));
    const Vec2 vel = Vec2::fromAngle(Angle::fromDegrees(deg), def.speed);
    const EntityId id = spawnBanana(s, kP0, owner + Vec2::fromAngle(Angle::fromDegrees(deg)), vel);

    const double reach = (c.player.radius + def.radius).toDouble();
    bool reached = false;
    for (int t = 0; t < 400 && !reached; ++t) {
      const Angle before = banana(s, id).transform.facing;
      const EventSink sink = tickProjectiles(s);
      const Boomerang& b = banana(s, id);
      REQUIRE(b.phase != BoomerangPhase::Grounded);
      // A wall bounce reflects the heading; the turn limit applies to steering.
      if (b.phase == BoomerangPhase::Returning && sink.count == 0) {
        CHECK(std::abs(b.transform.facing.signedDeltaFrom(before)) <= def.turnRateBam);
        CHECK(std::abs(length(b.transform.vel).toDouble() - def.speed.toDouble()) < 1e-3);
      }
      const Vec2 d = b.transform.pos - owner;
      reached = std::hypot(d.x.toDouble(), d.y.toDouble()) <= reach;
    }
    INFO("throw angle " << deg);
    CHECK(reached);
  }
}

TEST_CASE("returning with the owner gone grounds the banana in place", "[sim][projectile]") {
  WorldState s = makePlayingWorld(1, 0b11);
  const Vec2 pos = v(3, 2);
  const EntityId id = spawnBanana(s, kP1, pos, v(0.2, 0), BoomerangPhase::Returning);
  s.players.despawn(s.players.idAt(1));
  tickProjectiles(s);
  const Boomerang& b = banana(s, id);
  CHECK(b.phase == BoomerangPhase::Grounded);
  CHECK(b.ticksInPhase == 0);
  CHECK(b.transform.pos == pos);
  CHECK(b.transform.vel == Vec2::zero());

  // An outbound banana whose owner is gone keeps flying until it would return.
  WorldState s2 = makePlayingWorld(1, 0b11);
  const EntityId id2 = spawnBanana(s2, kP1, pos, v(0.2, 0));
  s2.players.despawn(s2.players.idAt(1));
  tickProjectiles(s2);
  CHECK(banana(s2, id2).phase == BoomerangPhase::Outbound);
  CHECK(banana(s2, id2).transform.pos == pos + v(0.2, 0));

  // An invalid owner id is "not alive" too.
  WorldState s3 = makePlayingWorld(1, 0b1);
  const EntityId id3 =
      spawnBanana(s3, PlayerId::invalid(), pos, v(0.2, 0), BoomerangPhase::Returning);
  tickProjectiles(s3);
  CHECK(banana(s3, id3).phase == BoomerangPhase::Grounded);
}

TEST_CASE("returning longer than maxReturnTicks grounds the banana", "[sim][projectile]") {
  ContentDB c = defaultContent();
  c.boomerang.maxReturnTicks = 5;
  c.boomerang.turnRateBam = 0;  // never turns, so it never reaches the owner
  WorldState s = makePlayingWorld(1, 0b1);
  placePlayer(s, kP0, v(-12, 0));
  const EntityId id = spawnBanana(s, kP0, v(-5, 0), Vec2{c.boomerang.speed, Fixed::zero()},
                                  BoomerangPhase::Returning, c);
  for (int t = 1; t <= 5; ++t) {
    tickProjectiles(s, c);
    REQUIRE(banana(s, id).phase == BoomerangPhase::Returning);
  }
  const Vec2 pos = banana(s, id).transform.pos;
  tickProjectiles(s, c);
  CHECK(banana(s, id).phase == BoomerangPhase::Grounded);
  CHECK(banana(s, id).transform.pos == pos);
}

TEST_CASE("grounded bananas stay put but still update prevPos and counters", "[sim][projectile]") {
  WorldState s = makePlayingWorld(1, 0b1);
  const EntityId id = spawnBanana(s, kP0, v(1, 1), v(0.3, 0.3), BoomerangPhase::Grounded);
  banana(s, id).transform.prevPos = v(0, 0);
  const Angle spin = banana(s, id).spin;
  const EventSink sink = tickProjectiles(s);
  const Boomerang& b = banana(s, id);
  CHECK(b.phase == BoomerangPhase::Grounded);
  CHECK(b.transform.pos == v(1, 1));
  CHECK(b.transform.prevPos == v(1, 1));
  CHECK(b.spin == spin);
  CHECK(b.ticksAlive == 1);
  CHECK(b.ticksInPhase == 1);
  CHECK(sink.count == 0);
}

TEST_CASE("tick counters saturate instead of wrapping", "[sim][projectile]") {
  WorldState s = makePlayingWorld(1, 0b1);
  const EntityId id = spawnBanana(s, kP0, v(1, 1), v(0, 0), BoomerangPhase::Grounded);
  banana(s, id).ticksAlive = UINT16_MAX;
  banana(s, id).ticksInPhase = UINT16_MAX - 1;
  tickProjectiles(s);
  tickProjectiles(s);
  CHECK(banana(s, id).ticksAlive == UINT16_MAX);
  CHECK(banana(s, id).ticksInPhase == UINT16_MAX);
}

TEST_CASE("spin advances while in flight", "[sim][projectile]") {
  WorldState s = makePlayingWorld(1, 0b1);
  const EntityId id = spawnBanana(s, kP0, v(0, 0), v(0.1, 0));
  tickProjectiles(s);
  const Angle first = banana(s, id).spin;
  tickProjectiles(s);
  CHECK(first.bam != 0);
  CHECK(banana(s, id).spin.signedDeltaFrom(first) == first.bam);
}

// --- walls --------------------------------------------------------------------

TEST_CASE("bananas bounce off each wall with restitution and an event", "[sim][projectile]") {
  ContentDB c = defaultContent();
  c.boomerang.restitution = Fixed::half();
  const Fixed limX = c.arena.halfExtents.x - c.boomerang.radius;
  const Fixed limY = c.arena.halfExtents.y - c.boomerang.radius;
  const Fixed step = Fixed::fromRatio(3, 10);
  const Fixed gap = Fixed::fromRatio(1, 10);

  struct Case {
    const char* name;
    Vec2 start;
    Vec2 vel;
    Vec2 wantPos;
    Vec2 wantVel;
  };
  const Case cases[] = {
      {"+x",
       {limX - gap, Fixed::one()},
       {step, Fixed::zero()},
       {(limX * 2) - (limX - gap + step), Fixed::one()},
       {-step * Fixed::half(), Fixed::zero()}},
      {"-x",
       {-limX + gap, Fixed::one()},
       {-step, Fixed::zero()},
       {(-limX * 2) - (-limX + gap - step), Fixed::one()},
       {step * Fixed::half(), Fixed::zero()}},
      {"+y",
       {Fixed::one(), limY - gap},
       {Fixed::zero(), step},
       {Fixed::one(), (limY * 2) - (limY - gap + step)},
       {Fixed::zero(), -step * Fixed::half()}},
      {"-y",
       {Fixed::one(), -limY + gap},
       {Fixed::zero(), -step},
       {Fixed::one(), (-limY * 2) - (-limY + gap - step)},
       {Fixed::zero(), step * Fixed::half()}},
  };
  for (const Case& k : cases) {
    INFO("wall " << k.name);
    WorldState s = makePlayingWorld(1, 0b1);
    const EntityId id = spawnBanana(s, kP0, k.start, k.vel, BoomerangPhase::Outbound, c);
    const EventSink sink = tickProjectiles(s, c);
    const Boomerang& b = banana(s, id);
    CHECK(b.transform.pos == k.wantPos);
    CHECK(b.transform.vel == k.wantVel);
    CHECK(std::abs(bamError(b.transform.facing, angleOf(k.wantVel))) == 0);
    REQUIRE(sink.count == 1);
    CHECK(sink.events[0].kind == EventKind::BoomerangBounced);
    CHECK(sink.events[0].entity == id);
    CHECK(sink.events[0].tick == s.tick);
  }
}

TEST_CASE("a corner bounce reflects both axes and emits one event", "[sim][projectile]") {
  const ContentDB& c = defaultContent();
  const Fixed limX = c.arena.halfExtents.x - c.boomerang.radius;
  const Fixed limY = c.arena.halfExtents.y - c.boomerang.radius;
  WorldState s = makePlayingWorld(1, 0b1);
  const Vec2 vel = v(0.2, 0.2);
  const EntityId id = spawnBanana(s, kP0, Vec2{limX, limY} - v(0.1, 0.1), vel);
  const EventSink sink = tickProjectiles(s);
  CHECK(banana(s, id).transform.vel == -vel);
  CHECK(banana(s, id).transform.pos.x < limX);
  CHECK(banana(s, id).transform.pos.y < limY);
  CHECK(countEvents(sink, EventKind::BoomerangBounced) == 1);
}

TEST_CASE("a huge overshoot is clamped inside the arena", "[sim][projectile]") {
  const ContentDB& c = defaultContent();
  const Fixed limX = c.arena.halfExtents.x - c.boomerang.radius;
  const Fixed limY = c.arena.halfExtents.y - c.boomerang.radius;
  WorldState s = makePlayingWorld(1, 0b1);
  const EntityId id = spawnBanana(s, kP0, v(0, 0), v(100, -100));
  tickProjectiles(s);
  const Vec2 pos = banana(s, id).transform.pos;
  CHECK(pos.x >= -limX);
  CHECK(pos.x <= limX);
  CHECK(pos.y >= -limY);
  CHECK(pos.y <= limY);
}

TEST_CASE("a banana resting exactly on the wall line does not bounce", "[sim][projectile]") {
  const ContentDB& c = defaultContent();
  const Fixed limX = c.arena.halfExtents.x - c.boomerang.radius;
  WorldState s = makePlayingWorld(1, 0b1);
  const EntityId id = spawnBanana(s, kP0, Vec2{limX - Fixed::fromRatio(1, 10), Fixed::zero()},
                                  Vec2{Fixed::fromRatio(1, 10), Fixed::zero()});
  const EventSink sink = tickProjectiles(s);
  CHECK(banana(s, id).transform.pos.x == limX);
  CHECK(sink.count == 0);
}

// --- determinism ----------------------------------------------------------------

TEST_CASE("projectile flight is deterministic", "[sim][projectile]") {
  const auto run = [] {
    WorldState s = makePlayingWorld(7, 0b111);
    placePlayer(s, kP0, v(-4, 2));
    placePlayer(s, kP1, v(6, -3));
    Rng rng = Rng::seeded(99);
    for (int i = 0; i < 20; ++i) {
      const Vec2 vel = Vec2::fromAngle(rng.angle(), defaultContent().boomerang.speed);
      spawnBanana(s, PlayerId{static_cast<uint8_t>(i % 3)}, v(0, 0), vel);
    }
    for (int t = 0; t < 400; ++t)
      tickProjectiles(s);
    return s;
  };
  const WorldState a = run();
  const WorldState b = run();
  CHECK(checksum(a) == checksum(b));
}
