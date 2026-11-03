// End-to-end throw scenarios (architecture §9): scripted PlayerInput frames
// through the full step() pipeline. Only what Throw owns is asserted (spawn
// tick, owner, spawn point, launch velocity, ammo); the banana's flight
// belongs to ProjectileSystem, so its later path is not checked here.
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kThrow = buttonBit(Button::Throw);
constexpr uint8_t kDash = buttonBit(Button::Dash);
const PlayerId kP0{0};

bool near(Fixed a, Fixed b, int32_t rawTol = 8) {
  return std::abs(a.raw - b.raw) <= rawTol;
}

EventLog stepWith(WorldState& s, PlayerInput in, const ContentDB& content = defaultContent()) {
  InputFrame f = frameFor(s);
  f.inputs[0] = in;
  EventLog log;
  runTicks(s, f, 1, content, log);
  return log;
}

}  // namespace

TEST_CASE("a throw launches one banana two ticks after the press", "[scenario][throw]") {
  const ContentDB& content = defaultContent();
  const AbilityDef& thr = content.abilityForSlot(kAbilitySlotThrow);
  WorldState s = makePlayingWorld(1, 0b1);
  const Vec2 origin{Fixed::fromInt(-2), Fixed::one()};
  placePlayer(s, kP0, origin);
  REQUIRE(s.playerBySlot(kP0)->ammo == content.player.ammoStart);
  REQUIRE(content.player.ammoStart == 1);

  // Press Throw aiming -y (192), stick idle; release afterwards.
  const Tick pressTick = s.tick;
  EventLog log = stepWith(s, PlayerInput{0, 0, 192, kThrow, 0});
  for (int k = 1; k <= int{thr.startupTicks}; ++k) {
    CHECK(countEvents(log, EventKind::BoomerangThrown) == 0);
    log = stepWith(s, PlayerInput{0, 0, 192, 0, 0});
  }
  // Startup 2: the third stepWith (tick T+2) spawned the banana.
  const SimEvent* thrown = findEvent(log, EventKind::BoomerangThrown);
  REQUIRE(thrown != nullptr);
  CHECK(thrown->tick == pressTick.plus(thr.startupTicks));
  CHECK(thrown->player == kP0);
  CHECK(s.playerBySlot(kP0)->ammo == 0);

  const Boomerang* b = s.boomerangs.get(thrown->entity);
  REQUIRE(b != nullptr);
  CHECK(b->owner == kP0);
  CHECK(b->phase == BoomerangPhase::Outbound);
  // Spawn point = thrower + aim × spawnOffset. prevPos holds it whether or not
  // ProjectileSystem already integrated the banana this tick.
  CHECK(near(b->transform.prevPos.x, origin.x));
  CHECK(near(b->transform.prevPos.y, origin.y - content.boomerang.spawnOffset));
  CHECK(near(b->transform.vel.x, Fixed::zero()));
  CHECK(near(b->transform.vel.y, -content.boomerang.speed));
  const bool unmoved = b->transform.pos == b->transform.prevPos;
  const bool movedOnce = b->transform.pos == b->transform.prevPos + b->transform.vel;
  CHECK((unmoved || movedOnce));
}

TEST_CASE("throwing with no ammo does nothing; a second banana needs ammo", "[scenario][throw]") {
  const ContentDB& content = defaultContent();
  const AbilityDef& thr = content.abilityForSlot(kAbilitySlotThrow);
  WorldState s = makePlayingWorld(2, 0b1);
  placePlayer(s, kP0, Vec2::zero());
  int thrown = 0;
  // Two presses well past the cooldown; ammoStart = 1 allows only one banana.
  for (int k = 0; k < 3 * thr.cooldownTicks; ++k) {
    const uint8_t buttons = (k == 0 || k == 2 * thr.cooldownTicks) ? kThrow : 0;
    thrown +=
        countEvents(stepWith(s, PlayerInput{0, 0, 0, buttons, 0}), EventKind::BoomerangThrown);
  }
  CHECK(thrown == 1);
  CHECK(s.playerBySlot(kP0)->ammo == 0);
  CHECK(s.playerBySlot(kP0)->abilities[kAbilitySlotThrow].phase == AbilityPhase::Idle);
}

TEST_CASE("throw while dashing spawns the banana from the moving player", "[scenario][throw]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(3, 0b1);
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-8), Fixed::zero()});
  EventLog log = stepWith(s, PlayerInput{0, 0, 0, kDash | kThrow, 0});
  CHECK(countEvents(log, EventKind::PlayerDashed) == 1);
  stepWith(s, PlayerInput{0, 0, 0, 0, 0});
  // Abilities run before movement: the banana spawns at the position the
  // player had at the start of the spawn tick.
  const Vec2 posBeforeSpawnTick = s.playerBySlot(kP0)->transform.pos;
  log = stepWith(s, PlayerInput{0, 0, 0, 0, 0});
  const SimEvent* thrown = findEvent(log, EventKind::BoomerangThrown);
  REQUIRE(thrown != nullptr);
  const Boomerang& b = *s.boomerangs.get(thrown->entity);
  CHECK(near(b.transform.prevPos.x, posBeforeSpawnTick.x + content.boomerang.spawnOffset));
}

namespace {

// A scripted two-player run mixing movement, aim, dashes and throws.
std::vector<uint32_t> scriptedChecksums(uint64_t seed) {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(seed, 0b11);
  std::vector<uint32_t> sums;
  EventSink sink;
  for (int k = 0; k < 600; ++k) {
    InputFrame f = frameFor(s);
    f.inputs[0] =
        PlayerInput{static_cast<uint8_t>(1 + (k * 7) % 255), 255, static_cast<uint8_t>(k * 3),
                    static_cast<uint8_t>((k % 50 == 0 ? kThrow : 0) | (k % 70 == 10 ? kDash : 0)),
                    static_cast<uint8_t>(k * 11)};
    f.inputs[1] =
        PlayerInput{static_cast<uint8_t>(200 - (k % 120)), 180, static_cast<uint8_t>(255 - k),
                    static_cast<uint8_t>(k % 33 == 0 ? kDash | kThrow : 0), 17};
    sink.clear();
    step(s, f, content, sink);
    sums.push_back(checksum(s));
  }
  return sums;
}

}  // namespace

TEST_CASE("identical scripted runs give identical per-tick checksums", "[scenario][throw]") {
  const std::vector<uint32_t> a = scriptedChecksums(42);
  const std::vector<uint32_t> b = scriptedChecksums(42);
  CHECK(a == b);
  // The run actually did something: state changed along the way.
  CHECK(a.front() != a.back());
}
