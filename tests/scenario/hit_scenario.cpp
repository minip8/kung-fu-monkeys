// End-to-end hits through step() with scripted inputs (architecture §5.6,
// §5.7, §9): a thrown banana kills, grounds and becomes a pickup; a missed
// banana returns to its thrower.
//
// These exercise Throw (abilitySystem), flight (projectileSystem) and swept
// collision together with the interaction rules.
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kAimLeft = 128;  // half a turn: toward -x
constexpr uint8_t kAimRight = 0;

// Index of the first event of a kind in log, or log.size().
size_t indexOf(const EventLog& log, EventKind kind) {
  for (size_t i = 0; i < log.size(); ++i) {
    if (log[i].kind == kind) return i;
  }
  return log.size();
}

}  // namespace

TEST_CASE("a thrown banana eliminates, grounds and ends the round", "[scenario][hit]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(21, 0b11);
  placePlayer(s, PlayerId{0}, Vec2{Fixed::fromInt(-4), Fixed::zero()});
  placePlayer(s, PlayerId{1}, Vec2{Fixed::fromInt(4), Fixed::zero()});

  InputFrame f = frameFor(s);
  f.inputs[0].aimDir = kAimRight;
  f.inputs[1].aimDir = kAimLeft;
  f.inputs[1].buttons = buttonBit(Button::Throw);
  EventLog log;
  runTicks(s, f, 1, content, log);
  f.inputs[1].buttons = 0;
  runTicks(s, f, 90, content, log);

  const SimEvent* thrown = findEvent(log, EventKind::BoomerangThrown);
  REQUIRE(thrown != nullptr);
  CHECK(thrown->player == PlayerId{1});

  const SimEvent* hit = findEvent(log, EventKind::PlayerHit);
  REQUIRE(hit != nullptr);
  CHECK(hit->player == PlayerId{0});
  CHECK(hit->entity == thrown->entity);
  CHECK(hit->a == 1);
  CHECK(hit->b == 0);
  const SimEvent* eliminated = findEvent(log, EventKind::PlayerEliminated);
  REQUIRE(eliminated != nullptr);
  CHECK(eliminated->player == PlayerId{0});
  CHECK(eliminated->a == 1);

  // Hit -> eliminated -> banana grounds -> pickup -> round over, in one tick.
  const size_t iHit = indexOf(log, EventKind::PlayerHit);
  const size_t iElim = indexOf(log, EventKind::PlayerEliminated);
  const size_t iGround = indexOf(log, EventKind::BoomerangGrounded);
  const size_t iPickup = indexOf(log, EventKind::PickupSpawned);
  const size_t iEnd = indexOf(log, EventKind::RoundEnded);
  REQUIRE(iEnd < log.size());
  CHECK(iHit < iElim);
  CHECK(iElim < iGround);
  CHECK(iGround < iPickup);
  CHECK(iPickup < iEnd);
  CHECK(log[iHit].tick == log[iEnd].tick);
  CHECK(log[iGround].entity == thrown->entity);
  CHECK(log[iEnd].player == PlayerId{1});
  CHECK(countEvents(log, EventKind::BoomerangCaught) == 0);

  CHECK(s.playerBySlot(PlayerId{0}) == nullptr);
  CHECK(s.rules.phase == MatchPhase::RoundEnded);
  CHECK(s.rules.scores[1] == 1);
  CHECK(s.boomerangs.empty());
  REQUIRE(s.pickups.size() == 1);
  // The pickup lies between the two players, on the victim's side.
  const Vec2 at = s.pickups.items[log[iPickup].entity.index].pos;
  CHECK(at.x < Fixed::fromInt(-3));
  CHECK(at.x > Fixed::fromInt(-4));
}

TEST_CASE("a missed banana returns and is auto-caught by its thrower", "[scenario][hit]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(22, 0b11);
  // Player 0 stands well clear of the throw line and of the return loop.
  placePlayer(s, PlayerId{0}, Vec2{Fixed::fromInt(-4), Fixed::fromInt(-7)});
  placePlayer(s, PlayerId{1}, Vec2{Fixed::fromInt(4), Fixed::zero()});

  InputFrame f = frameFor(s);
  f.inputs[1].aimDir = kAimLeft;
  f.inputs[1].buttons = buttonBit(Button::Throw);
  EventLog log;
  runTicks(s, f, 1, content, log);
  f.inputs[1].buttons = 0;
  runTicks(s, f, 200, content, log);

  CHECK(countEvents(log, EventKind::BoomerangThrown) == 1);
  CHECK(countEvents(log, EventKind::PlayerHit) == 0);
  const SimEvent* caught = findEvent(log, EventKind::BoomerangCaught);
  REQUIRE(caught != nullptr);
  CHECK(caught->player == PlayerId{1});
  CHECK(caught->a == static_cast<uint32_t>(CatchHow::Auto));
  CHECK(s.boomerangs.empty());
  CHECK(s.playerBySlot(PlayerId{1})->ammo == content.player.ammoStart);
  CHECK(s.rules.phase == MatchPhase::Playing);
}
