// End-to-end dash scenarios (architecture §9): scripted PlayerInput frames
// through the full step() pipeline -> expected events, tags and displacement.
// One player only, so no other system (collisions, round end) can interfere.
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kDash = buttonBit(Button::Dash);
const PlayerId kP0{0};

bool near(Fixed a, Fixed b, int32_t rawTol) {
  return std::abs(a.raw - b.raw) <= rawTol;
}

// Steps one tick with player 0's input and returns that tick's events.
EventLog stepWith(WorldState& s, PlayerInput in, const ContentDB& content = defaultContent()) {
  InputFrame f = frameFor(s);
  f.inputs[0] = in;
  EventLog log;
  runTicks(s, f, 1, content, log);
  return log;
}

}  // namespace

TEST_CASE("a dash along facing covers activeTicks x speed and then stops", "[scenario][dash]") {
  const ContentDB& content = defaultContent();
  const AbilityDef& dash = content.abilityForSlot(kAbilitySlotDash);
  WorldState s = makePlayingWorld(1, 0b1);
  const Vec2 start{Fixed::fromInt(-10), Fixed::fromInt(2)};
  placePlayer(s, kP0, start);

  // Press Dash aiming +x with the stick idle.
  const Tick pressTick = s.tick;
  EventLog log = stepWith(s, PlayerInput{0, 0, 0, kDash, 40});
  const SimEvent* dashed = findEvent(log, EventKind::PlayerDashed);
  REQUIRE(dashed != nullptr);
  CHECK(dashed->tick == pressTick);
  CHECK(dashed->player == kP0);
  CHECK(dashed->entity == s.players.idAt(0));

  // Ticks 1..A-1 of the dash, then recovery and beyond; button released.
  int dashedEvents = 0;
  for (int k = 1; k < dash.activeTicks + dash.recoveryTicks + 10; ++k) {
    const Player& p = *s.playerBySlot(kP0);
    INFO("k = " << k - 1);
    CHECK(p.tags.has(Tag::Dashing) == (k - 1 < dash.activeTicks));
    // Pressed at sub-tick 40: the window closes inside tick T + perfectWindowTicks.
    CHECK(p.tags.has(Tag::PerfectDash) == (k - 1 <= dash.perfectWindowTicks));
    log = stepWith(s, PlayerInput{0, 0, 0, 0, 0});
    dashedEvents += countEvents(log, EventKind::PlayerDashed);
  }
  CHECK(dashedEvents == 0);

  const Player& p = *s.playerBySlot(kP0);
  CHECK_FALSE(p.tags.has(Tag::Dashing));
  CHECK(p.transform.vel == Vec2::zero());
  // Exactly activeTicks of movement at dash speed along +x (cos 0 is exact).
  CHECK(p.transform.pos.x == start.x + dash.speed * int32_t{dash.activeTicks});
  CHECK(p.transform.pos.y == start.y);
}

TEST_CASE("a dash follows the stick, then walking resumes at move speed", "[scenario][dash]") {
  const ContentDB& content = defaultContent();
  const AbilityDef& dash = content.abilityForSlot(kAbilitySlotDash);
  WorldState s = makePlayingWorld(2, 0b1);
  const Vec2 start{Fixed::zero(), Fixed::fromInt(-6)};
  placePlayer(s, kP0, start);

  // Stick held +y the whole time, aim -x; press Dash on the first tick.
  stepWith(s, PlayerInput{64, 255, 128, kDash, 0});
  for (int k = 1; k < int{dash.activeTicks}; ++k)
    stepWith(s, PlayerInput{64, 255, 128, 0, 0});
  const Player& p = *s.playerBySlot(kP0);
  const Fixed dashed = p.transform.pos.y - start.y;
  CHECK(near(dashed, dash.speed * int32_t{dash.activeTicks}, 256));
  CHECK(near(p.transform.pos.x, start.x, 64));

  // First recovery tick: back to intent-driven velocity.
  stepWith(s, PlayerInput{64, 255, 128, 0, 0});
  CHECK(near(p.transform.vel.y, content.player.moveSpeed, 8));
  CHECK(p.transform.pos.y - start.y > dashed);
}

TEST_CASE("a dash into the wall stops at the arena edge", "[scenario][dash]") {
  const ContentDB& content = defaultContent();
  const Fixed limX = content.arena.halfExtents.x - content.player.radius;
  WorldState s = makePlayingWorld(3, 0b1);
  placePlayer(s, kP0, Vec2{limX - Fixed::one(), Fixed::zero()});
  stepWith(s, PlayerInput{0, 0, 0, kDash, 0});
  for (int k = 0; k < 20; ++k)
    stepWith(s, PlayerInput{0, 0, 0, 0, 0});
  const Player& p = *s.playerBySlot(kP0);
  CHECK(p.transform.pos.x == limX);
  CHECK(p.transform.pos.y == Fixed::zero());
}

TEST_CASE("dash cooldown holds across the full pipeline", "[scenario][dash]") {
  const ContentDB& content = defaultContent();
  const AbilityDef& dash = content.abilityForSlot(kAbilitySlotDash);
  WorldState s = makePlayingWorld(4, 0b1);
  placePlayer(s, kP0, Vec2::zero());
  // Mash Dash every other tick for two cooldowns: one dash per cooldown window.
  int dashes = 0;
  const int ticks = 2 * dash.cooldownTicks;
  for (int k = 0; k < ticks; ++k) {
    const uint8_t buttons = (k % 2 == 0) ? kDash : 0;
    dashes += countEvents(stepWith(s, PlayerInput{0, 0, 0, buttons, 0}), EventKind::PlayerDashed);
  }
  CHECK(dashes == 2);
}
