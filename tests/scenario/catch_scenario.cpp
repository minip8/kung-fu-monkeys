// End-to-end perfect-dash catch through step() with scripted inputs
// (architecture §5.6, §5.11, §9): dash into an incoming banana at the right
// moment to catch it; dash too early and it hits.
//
// These exercise Dash and Throw (abilitySystem, which grants PerfectDash),
// flight (projectileSystem) and swept collision together with the catch
// rule, so they only pass once the whole M1 pipeline is integrated. Remove
// the SKIP lines at integration.
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kAimLeft = 128;  // half a turn: toward -x
constexpr uint8_t kAimRight = 0;

// Player 1 throws at player 0 across the arena's x axis; player 0 faces the
// throw and presses Dash (toward the banana) on the first tick the banana is
// within `dashGap` of it. Returns the events of the whole exchange.
EventLog throwAndDash(Fixed dashGap) {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(31, 0b11);
  placePlayer(s, PlayerId{0}, Vec2{Fixed::fromInt(-4), Fixed::zero()});
  placePlayer(s, PlayerId{1}, Vec2{Fixed::fromInt(4), Fixed::zero()});

  InputFrame f = frameFor(s);
  f.inputs[0].aimDir = kAimRight;
  f.inputs[1].aimDir = kAimLeft;
  f.inputs[1].buttons = buttonBit(Button::Throw);
  EventLog log;
  runTicks(s, f, 1, content, log);
  f.inputs[1].buttons = 0;

  bool dashed = false;
  for (int t = 0; t < 120; ++t) {
    f.inputs[0].buttons = 0;
    const Player* p0 = s.playerBySlot(PlayerId{0});
    if (!dashed && p0 != nullptr) {
      s.boomerangs.forEachAlive([&](EntityId /*id*/, const Boomerang& b) {
        if (b.transform.pos.x - p0->transform.pos.x <= dashGap) dashed = true;
      });
      if (dashed) f.inputs[0].buttons = buttonBit(Button::Dash);
    }
    runTicks(s, f, 1, content, log);
  }
  REQUIRE(dashed);
  return log;
}

}  // namespace

TEST_CASE("a well-timed dash catches the incoming banana", "[scenario][catch]") {
  SKIP("needs integrated systems: dash, throw, projectile and collision");
  // Contact distance is 0.85 (player 0.5 + banana 0.35) and dash plus banana
  // close at ~0.63 u/tick, so pressing at a gap of 2 meets the banana within
  // the 4-tick perfect window.
  const EventLog log = throwAndDash(Fixed::fromInt(2));
  CHECK(countEvents(log, EventKind::PlayerDashed) == 1);
  const SimEvent* caught = findEvent(log, EventKind::BoomerangCaught);
  REQUIRE(caught != nullptr);
  CHECK(caught->player == PlayerId{0});
  CHECK(caught->a == static_cast<uint32_t>(CatchHow::Perfect));
  CHECK(countEvents(log, EventKind::PlayerHit) == 0);
  CHECK(countEvents(log, EventKind::RoundEnded) == 0);
  // The catch happens within the perfect window of the dash.
  const SimEvent* dashed = findEvent(log, EventKind::PlayerDashed);
  REQUIRE(dashed != nullptr);
  CHECK(caught->tick.minus(dashed->tick) <
        defaultContent().abilityForSlot(kAbilitySlotDash).perfectWindowTicks);
}

TEST_CASE("a dash pressed too early is still dashing but gets hit", "[scenario][catch]") {
  SKIP("needs integrated systems: dash, throw, projectile and collision");
  // At a gap of 6 the banana arrives ~8 ticks into the dash: past the perfect
  // window, so the catch declines and the hit applies.
  const EventLog log = throwAndDash(Fixed::fromInt(6));
  CHECK(countEvents(log, EventKind::PlayerDashed) == 1);
  CHECK(countEvents(log, EventKind::BoomerangCaught) == 0);
  const SimEvent* hit = findEvent(log, EventKind::PlayerHit);
  REQUIRE(hit != nullptr);
  CHECK(hit->player == PlayerId{0});
  CHECK(countEvents(log, EventKind::PlayerEliminated) == 1);
  const SimEvent* ended = findEvent(log, EventKind::RoundEnded);
  REQUIRE(ended != nullptr);
  CHECK(ended->player == PlayerId{1});
}
