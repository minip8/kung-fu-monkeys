// MovementSystem (contract step 4): intent × moveSpeed, dash passthrough,
// HitStun, match-phase freeze, arena clamp.
#include "kfm/sim/systems.hpp"
#include "sim_test_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

const PlayerId kP0{0};

bool near(Fixed a, Fixed b, int32_t rawTol = 4) {
  return std::abs(a.raw - b.raw) <= rawTol;
}

// Decodes one input for player 0 and runs movement.
void moveWith(WorldState& s, PlayerInput in, const ContentDB& content = defaultContent()) {
  InputFrame f = frameFor(s);
  f.inputs[0] = in;
  inputSystem(s, f);
  movementSystem(s, content);
}

}  // namespace

TEST_CASE("speed scales with moveMag / 255 * moveSpeed", "[sim][movement]") {
  const ContentDB& content = defaultContent();
  for (const uint8_t mag : {uint8_t{255}, uint8_t{128}, uint8_t{1}, uint8_t{0}}) {
    WorldState s = makePlayingWorld(1, 0b1);
    const Vec2 start{Fixed::fromInt(2), Fixed::fromInt(3)};
    placePlayer(s, kP0, start);
    moveWith(s, PlayerInput{64, mag, 0, 0, 0});  // +y
    const Player& p = *s.playerBySlot(kP0);
    const Fixed want = content.player.moveSpeed * Fixed::fromRatio(mag, 255);
    INFO("mag = " << int{mag});
    CHECK(p.transform.prevPos == start);
    CHECK(near(p.transform.vel.y, want));
    CHECK(near(p.transform.vel.x, Fixed::zero()));
    CHECK(p.transform.pos == start + p.transform.vel);
  }
}

TEST_CASE("idle input stops the player", "[sim][movement]") {
  WorldState s = makePlayingWorld(2, 0b1);
  placePlayer(s, kP0, Vec2::zero()).transform.vel = Vec2{Fixed::one(), Fixed::one()};
  moveWith(s, PlayerInput{64, 0, 0, 0, 0});  // moveMag 0 = idle
  CHECK(s.playerBySlot(kP0)->transform.vel == Vec2::zero());
  CHECK(s.playerBySlot(kP0)->transform.pos == Vec2::zero());
}

TEST_CASE("derived moveSpeed is used, not the content base", "[sim][movement]") {
  WorldState s = makePlayingWorld(3, 0b1);
  Player& p = placePlayer(s, kP0, Vec2::zero());
  p.stats.moveSpeed = Fixed::half();
  moveWith(s, PlayerInput{128, 255, 0, 0, 0});  // -x
  CHECK(near(p.transform.vel.x, -Fixed::half()));
}

TEST_CASE("dashing keeps the ability-set velocity", "[sim][movement]") {
  WorldState s = makePlayingWorld(4, 0b1);
  Player& p = placePlayer(s, kP0, Vec2::zero());
  const Vec2 dashVel{Fixed::fromRatio(1, 3), Fixed::zero()};
  p.tags.set(Tag::Dashing);
  p.transform.vel = dashVel;
  // inputSystem does not touch vel; movement must not replace it with intent.
  moveWith(s, PlayerInput{64, 255, 0, 0, 0});
  CHECK(p.transform.vel == dashVel);
  CHECK(p.transform.pos == dashVel);
}

TEST_CASE("HitStun stops movement, even while dashing", "[sim][movement]") {
  WorldState s = makePlayingWorld(5, 0b1);
  Player& p = placePlayer(s, kP0, Vec2::zero());
  p.tags.set(Tag::HitStun);
  moveWith(s, PlayerInput{64, 255, 0, 0, 0});
  CHECK(p.transform.vel == Vec2::zero());
  CHECK(p.transform.pos == Vec2::zero());

  p.tags.set(Tag::Dashing);
  p.transform.vel = Vec2{Fixed::one(), Fixed::zero()};
  moveWith(s, PlayerInput{0, 0, 0, 0, 0});
  CHECK(p.transform.vel == Vec2::zero());
  CHECK(p.transform.pos == Vec2::zero());
}

TEST_CASE("movement is frozen outside Playing", "[sim][movement]") {
  for (const MatchPhase phase : {MatchPhase::Lobby, MatchPhase::RoundStarting,
                                 MatchPhase::RoundEnded, MatchPhase::MatchEnded}) {
    WorldState s = makePlayingWorld(6, 0b1);
    s.rules.phase = phase;
    const Vec2 start{Fixed::fromInt(-3), Fixed::one()};
    Player& p = placePlayer(s, kP0, start);
    p.tags.set(Tag::Dashing);  // a dash in flight when the round ended stops too
    p.transform.vel = Vec2{Fixed::one(), Fixed::zero()};
    moveWith(s, PlayerInput{64, 255, 0, 0, 0});
    INFO("phase = " << static_cast<int>(phase));
    CHECK(p.transform.vel == Vec2::zero());
    CHECK(p.transform.pos == start);
    CHECK(p.transform.prevPos == start);
  }
}

TEST_CASE("arena clamp on all four walls zeroes the clamped component", "[sim][movement]") {
  const ContentDB& content = defaultContent();
  const Fixed limX = content.arena.halfExtents.x - content.player.radius;
  const Fixed limY = content.arena.halfExtents.y - content.player.radius;
  const Fixed eps = Fixed::fromRatio(1, 100);
  struct Case {
    Vec2 start;
    uint8_t moveDir{0};  // 1/256 turn
    bool clampX{false};
  };
  // Start a hair inside each wall and walk diagonally into it, so only one
  // component is clamped and the other keeps moving.
  const Case cases[] = {
      {Vec2{limX - eps, Fixed::zero()}, 32, true},     // +x wall, moving +x+y
      {Vec2{-limX + eps, Fixed::zero()}, 160, true},   // -x wall, moving -x-y
      {Vec2{Fixed::zero(), limY - eps}, 96, false},    // +y wall, moving -x+y
      {Vec2{Fixed::zero(), -limY + eps}, 224, false},  // -y wall, moving +x-y
  };
  for (const Case& c : cases) {
    WorldState s = makePlayingWorld(7, 0b1);
    Player& p = placePlayer(s, kP0, c.start);
    moveWith(s, PlayerInput{c.moveDir, 255, 0, 0, 0});
    INFO("moveDir = " << int{c.moveDir});
    if (c.clampX) {
      CHECK(abs(p.transform.pos.x) == limX);
      CHECK(p.transform.vel.x == Fixed::zero());
      CHECK(p.transform.vel.y != Fixed::zero());
      CHECK(p.transform.pos.y == c.start.y + p.transform.vel.y);
    } else {
      CHECK(abs(p.transform.pos.y) == limY);
      CHECK(p.transform.vel.y == Fixed::zero());
      CHECK(p.transform.vel.x != Fixed::zero());
      CHECK(p.transform.pos.x == c.start.x + p.transform.vel.x);
    }
  }
}

TEST_CASE("a dash into a corner stops at both walls", "[sim][movement]") {
  const ContentDB& content = defaultContent();
  const Vec2 lim = content.arena.halfExtents - Vec2{content.player.radius, content.player.radius};
  WorldState s = makePlayingWorld(8, 0b1);
  Player& p = placePlayer(s, kP0, lim - Vec2{Fixed::fromRatio(1, 10), Fixed::fromRatio(1, 10)});
  p.tags.set(Tag::Dashing);
  p.transform.vel = Vec2{Fixed::one(), Fixed::one()};
  moveWith(s, PlayerInput{});
  CHECK(p.transform.pos == lim);
  CHECK(p.transform.vel == Vec2::zero());
}

TEST_CASE("movement applies to every alive player in one pass", "[sim][movement]") {
  WorldState s = makePlayingWorld(9, 0b101);
  placePlayer(s, PlayerId{0}, Vec2::zero());
  placePlayer(s, PlayerId{2}, Vec2::zero());
  InputFrame f = frameFor(s);
  f.inputs[0] = PlayerInput{64, 255, 0, 0, 0};
  f.inputs[2] = PlayerInput{192, 255, 0, 0, 0};
  inputSystem(s, f);
  movementSystem(s, defaultContent());
  CHECK(s.playerBySlot(PlayerId{0})->transform.pos.y > Fixed::zero());
  CHECK(s.playerBySlot(PlayerId{2})->transform.pos.y < Fixed::zero());
}
