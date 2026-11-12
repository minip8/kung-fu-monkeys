// A mode's banana flight (architecture §11 "Contest design", v0.80): a mode
// may override the BoomerangDef's outbound ticks and return turn rate
// (ModeDef::bananaOutboundTicks / ::bananaTurnRateBam, 0 = the def's), read
// by ProjectileSystem; nothing of it is in the world state.
#include "kfm/sim/scripted_inputs.hpp"
#include "kfm/sim/step.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};
constexpr uint8_t kFlight = 1;  // mode 0 with the flight overridden
constexpr uint16_t kOutbound = 10;
constexpr uint16_t kTurnRate = 4000;

ContentDB flightContent(uint16_t outbound, uint16_t turnRate) {
  ContentDB c = defaultContent();
  c.modes[kFlight] = c.modes[0];
  c.modes[kFlight].name = ContentName::of("flight");
  c.modes[kFlight].bananaOutboundTicks = outbound;
  c.modes[kFlight].bananaTurnRateBam = turnRate;
  c.modeCount = 2;
  return c;
}

WorldState playingIn(const MatchContent& m, uint8_t mode, uint64_t seed) {
  WorldState s = initialWorld(MatchConfig{.seed = seed, .players = 0b11, .mode = mode});
  EventLog log;
  for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i) {
    runTicks(s, frameFor(s), 1, m, log);
  }
  REQUIRE(s.rules.phase == MatchPhase::Playing);
  return s;
}

struct Turn {
  uint16_t outboundTicks{0};  // the ticks it flew out
  int32_t firstTurn{0};       // the heading change on its first returning tick
};

// A banana of kP0 flying straight away from it along +x, through step():
// how long it flies out and how hard it turns on the way back.
Turn flightIn(const MatchContent& m, uint8_t mode) {
  WorldState s = playingIn(m, mode, 0xF117);
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-12), Fixed::zero()});
  placePlayer(s, kP1, Vec2{Fixed::fromInt(12), Fixed::fromInt(6)});
  const EntityId id =
      spawnBanana(s, kP0, Vec2{Fixed::fromInt(-11), Fixed::zero()},
                  Vec2{m.db.boomerang.speed, Fixed::zero()}, BoomerangPhase::Outbound, m.db);
  EventLog log;
  Turn out;
  for (int t = 0; t < 200; ++t) {
    const Boomerang* before = s.boomerangs.get(id);
    REQUIRE(before != nullptr);
    const Angle facing = before->transform.facing;
    runTicks(s, frameFor(s), 1, m, log);
    const Boomerang* b = s.boomerangs.get(id);
    REQUIRE(b != nullptr);
    if (b->phase == BoomerangPhase::Returning) {
      out.outboundTicks = b->ticksAlive;
      out.firstTurn = b->transform.facing.signedDeltaFrom(facing);
      break;
    }
    REQUIRE(b->phase == BoomerangPhase::Outbound);
  }
  if (out.outboundTicks == 0) FAIL_CHECK("the banana never turned for home");
  return out;
}

}  // namespace

TEST_CASE("banana flight: a mode's override turns it home sooner and harder",
          "[scenario][projectile][modes]") {
  const ContentDB c = flightContent(kOutbound, kTurnRate);
  REQUIRE(c.boomerang.outboundTicks != kOutbound);
  REQUIRE(c.boomerang.turnRateBam != kTurnRate);
  const MatchContent flight(c, 0, kFlight);
  CHECK(flight.bananaOutboundTicks() == kOutbound);
  CHECK(flight.bananaTurnRateBam() == kTurnRate);
  const Turn overridden = flightIn(flight, kFlight);
  CHECK(overridden.outboundTicks == kOutbound);
  // The owner is straight behind: the first returning tick turns by exactly the rate.
  CHECK(overridden.firstTurn == -int32_t{kTurnRate});

  // Mode 0 leaves both 0: the BoomerangDef's flight.
  const MatchContent plain(c, 0, 0);
  CHECK(plain.bananaOutboundTicks() == c.boomerang.outboundTicks);
  CHECK(plain.bananaTurnRateBam() == c.boomerang.turnRateBam);
  const Turn def = flightIn(plain, 0);
  CHECK(def.outboundTicks == c.boomerang.outboundTicks);
  CHECK(def.firstTurn == -int32_t{c.boomerang.turnRateBam});
}

TEST_CASE("banana flight: 0 is the def's, bit for bit", "[scenario][projectile][modes]") {
  // The same scripted match in a mode that leaves the flight at 0 and in one
  // that names the def's own numbers: every tick's state is the same.
  const ContentDB zero = flightContent(0, 0);
  const ContentDB named = flightContent(zero.boomerang.outboundTicks, zero.boomerang.turnRateBam);
  const MatchContent a(zero, 0, kFlight);
  const MatchContent b(named, 0, kFlight);
  WorldState sa = playingIn(a, kFlight, 0xF1A7);
  WorldState sb = playingIn(b, kFlight, 0xF1A7);
  Rng ra = Rng::seeded(0x5EED);
  Rng rb = Rng::seeded(0x5EED);
  EventLog la;
  EventLog lb;
  int thrown = 0;
  for (int t = 0; t < 900 && sa.rules.phase != MatchPhase::MatchEnded; ++t) {
    runTicks(sa, scriptedFrame(ra, sa, a), 1, a, la);
    runTicks(sb, scriptedFrame(rb, sb, b), 1, b, lb);
    REQUIRE(checksum(sa) == checksum(sb));
  }
  for (const SimEvent& e : la) {
    if (e.kind == EventKind::BoomerangThrown) ++thrown;
  }
  CHECK(thrown > 0);
  CHECK(la.size() == lb.size());
}
