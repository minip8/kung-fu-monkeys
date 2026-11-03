#include "kfm/sim/systems.hpp"
#include "sim_test_util.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {
constexpr uint8_t kThrow = buttonBit(Button::Throw);
constexpr uint8_t kDash = buttonBit(Button::Dash);
}  // namespace

TEST_CASE("held button fires a press edge once", "[sim][input]") {
  WorldState s = makePlayingWorld(1, 0b11);
  InputFrame f = frameFor(s);
  f.inputs[0].buttons = kDash;

  inputSystem(s, f);
  const Player& p = *s.playerBySlot(PlayerId{0});
  CHECK(p.intent.pressed == kDash);
  CHECK(p.intent.held == kDash);
  CHECK(p.prevButtons == kDash);

  inputSystem(s, f);  // still held
  CHECK(p.intent.pressed == 0);
  CHECK(p.intent.held == kDash);

  f.inputs[0].buttons = kDash | kThrow;  // second button joins
  inputSystem(s, f);
  CHECK(p.intent.pressed == kThrow);
  CHECK(p.intent.held == (kDash | kThrow));

  f.inputs[0].buttons = 0;  // release
  inputSystem(s, f);
  CHECK(p.intent.pressed == 0);
  CHECK(p.intent.held == 0);

  f.inputs[0].buttons = kDash;  // press again
  inputSystem(s, f);
  CHECK(p.intent.pressed == kDash);

  // The other player never pressed anything.
  CHECK(s.playerBySlot(PlayerId{1})->intent.pressed == 0);
}

TEST_CASE("move, aim, facing and subTick are decoded", "[sim][input]") {
  WorldState s = makePlayingWorld(2, 0b1);
  InputFrame f = frameFor(s);
  f.inputs[0] = PlayerInput{64, 255, 128, kThrow, 200};  // move +y at full speed, aim -x
  inputSystem(s, f);
  const Player& p = *s.playerBySlot(PlayerId{0});
  CHECK(abs(p.intent.move.x) <= Fixed::fromRaw(2));
  CHECK(abs(p.intent.move.y - Fixed::one()) <= Fixed::fromRaw(2));
  CHECK(p.intent.aim == Angle::fromInput8(128));
  CHECK(p.transform.facing == Angle::fromInput8(128));
  CHECK(p.intent.subTick == 200);
  CHECK(p.intent.pressed == kThrow);

  // Half analog magnitude halves the move vector.
  f.inputs[0] = PlayerInput{64, 128, 0, 0, 0};
  inputSystem(s, f);
  CHECK(abs(p.intent.move.y - Fixed::fromRatio(128, 255)) <= Fixed::fromRaw(2));

  // moveDir 0 is idle regardless of magnitude.
  f.inputs[0] = PlayerInput{0, 255, 0, 0, 0};
  inputSystem(s, f);
  CHECK(p.intent.move == Vec2::zero());
}

TEST_CASE("absent players get a zero input", "[sim][input]") {
  WorldState s = makePlayingWorld(3, 0b11);
  InputFrame f = frameFor(s);
  f.inputs[1] = PlayerInput{10, 255, 77, kDash, 9};
  inputSystem(s, f);
  const Player& p = *s.playerBySlot(PlayerId{1});
  REQUIRE(p.intent.held == kDash);

  f.present = 0b01;  // player 1 dropped out; its stale input must be ignored
  inputSystem(s, f);
  CHECK(p.intent.move == Vec2::zero());
  CHECK(p.intent.held == 0);
  CHECK(p.intent.pressed == 0);
  CHECK(p.intent.subTick == 0);
  CHECK(p.prevButtons == 0);

  f.present = 0b11;  // back with the button still down: that is a new press
  inputSystem(s, f);
  CHECK(p.intent.pressed == kDash);
}
