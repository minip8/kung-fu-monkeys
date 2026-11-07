// The dry-round rule of the round kinds (architecture §5.3, v0.57): a round
// with no banana left anywhere (none in a hand, in flight, grounded or lying
// around, a falling owner's banana bound for its pit counting as gone, and
// no power-up that tops ammo up lying around or still to be laid) ends as the
// kind's clock-out would: at once on an arena without pits, after
// ModeDef::dryRoundTicks dry ticks on one with pits (a monkey can still fall
// in). The parry-then-fall softlock is the case that called for it.
#include "kfm/sim/systems.hpp"
#include "loadout_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

const PlayerId kP0{0};
const PlayerId kP1{1};
const PlayerId kP2{2};
constexpr uint8_t kAbility = buttonBit(Button::Ability1);
constexpr uint8_t kLeft = 128;

Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

void emptyHands(WorldState& s) {
  s.players.forEachAlive([](EntityId /*id*/, Player& p) { p.ammo = 0; });
}

}  // namespace

TEST_CASE("dry round: no banana anywhere on a pitless arena ends the round as a draw at once",
          "[scenario][dry]") {
  const ContentDB& c = defaultContent();
  WorldState s = makePlayingWorld(41, 0b11, c);
  EventLog log;
  SECTION("a banana lying around keeps it going") {
    emptyHands(s);
    spawnPickup(s, at(5, 5));
    idle(s, 120, c, log);
    CHECK(countEvents(log, EventKind::RoundEnded) == 0);
    CHECK(s.rules.dryTicks == 0);
  }
  SECTION("a banana in flight keeps it going") {
    emptyHands(s);
    spawnBanana(s, kP0, at(0, 5), Vec2{Fixed::fromRatio(1, 10), Fixed{}});
    idle(s, 20, c, log);
    CHECK(countEvents(log, EventKind::RoundEnded) == 0);
  }
  SECTION("nothing at all: the round ends on the next tick, a draw") {
    emptyHands(s);
    idle(s, 1, c, log);
    const SimEvent* ended = findEvent(log, EventKind::RoundEnded);
    REQUIRE(ended != nullptr);
    CHECK_FALSE(ended->player.isValid());
    CHECK(s.rules.scores[0] == 0);
    CHECK(s.rules.scores[1] == 0);
  }
}

TEST_CASE("dry round: a spawner that can still lay an ammo power-up keeps it going",
          "[scenario][dry]") {
  ContentDB c = defaultContent();
  const uint8_t more =
      addPowerUp(c, "more", 600, {statGrant(StatKind::AmmoMax, Fixed::one(), Fixed::fromInt(2))});
  ArenaDef& a = c.arenas[0];
  a.pickupSpawners[0] =
      PickupSpawner{at(0, 6), static_cast<uint16_t>(1u << more), 600, 600, uint8_t{1}};
  a.pickupSpawnerCount = 1;
  WorldState s = makePlayingWorld(42, 0b11, c);
  emptyHands(s);
  EventLog log;
  idle(s, 120, c, log);
  CHECK(countEvents(log, EventKind::RoundEnded) == 0);
}

TEST_CASE("dry round: survival ends it as its clock would", "[scenario][dry]") {
  ContentDB c = defaultContent();
  ModeDef& m = c.modes[1];
  m = c.modes[0];
  m.name = ContentName::of("survival");
  m.kind = ModeKind::Survival;
  m.timeLimitTicks = 3600;
  c.modeCount = 2;
  const MatchContent match = matchContent(c, MatchConfig{.seed = 43, .players = 0b11, .mode = 1});
  WorldState s = playingWorld(43, 0b11, match);
  emptyHands(s);
  playerAt(s, kP1).hp = 2;  // the healthiest wins a clock-out
  EventLog log;
  idle(s, 1, match, log);
  const SimEvent* ended = findEvent(log, EventKind::RoundEnded);
  REQUIRE(ended != nullptr);
  CHECK(ended->player == kP1);
}

TEST_CASE("dry round: a parry, then the parrier falls: the round still ends, after the grace",
          "[scenario][dry][parry]") {
  // The softlock: P1 throws its only banana at P0, P0 parries it (now P0's)
  // and then walks into a pit holding its own. Both bananas go down with P0;
  // P1 and P2 are left with none, and nothing but a pit can eliminate either.
  ContentDB c = withExtraSlot(parryDef());
  ArenaDef& a = c.arenas[0];
  a.hazards[0] = Hazard{at(-3, -2), at(-1, 2), HazardKind::Pit, Fixed::one()};
  a.hazardCount = 1;
  const uint16_t grace = c.modes[0].dryRoundTicks;
  REQUIRE(grace > 0);
  WorldState s = makePlayingWorld(13, 0b111, c);
  placePlayer(s, kP0, at(0, 0));
  placePlayer(s, kP1, at(14, 0)).ammo = 0;  // its banana is the one in flight
  placePlayer(s, kP2, at(-12, 7)).ammo = 0;
  REQUIRE(playerAt(s, kP0).ammo > 0);
  const EntityId banana = spawnBanana(s, kP1, at(6, 0), Vec2{-c.boomerang.speed, Fixed::zero()});
  EventLog log;
  // The parry's window [press + 2, press + 10) covers the touch at ~19.3.
  idle(s, 12, c, log);
  pressOnce(s, kP0, kAbility, c, log);
  idle(s, 9, c, log);
  REQUIRE(findEvent(log, EventKind::BoomerangParried) != nullptr);
  REQUIRE(s.boomerangs.get(banana) != nullptr);
  REQUIRE(s.boomerangs.get(banana)->owner == kP0);
  // P0 walks west into the pit.
  for (int t = 0; t < 120 && findEvent(log, EventKind::PlayerFell) == nullptr; ++t) {
    InputFrame f = frameFor(s);
    f.inputs[0] = PlayerInput{kLeft, 255, kLeft, 0, 0};
    runTicks(s, f, 1, c, log);
  }
  REQUIRE(findEvent(log, EventKind::PlayerFell) != nullptr);
  idle(s, c.player.fallTicks, c, log);
  const SimEvent& out = firstEvent(log, EventKind::PlayerEliminated);
  REQUIRE(out.player == kP0);
  CHECK(countEvents(log, EventKind::PlayerHit) == 0);  // the parried banana went to the pit
  // Two monkeys left, not a banana between them: dry from the elimination on.
  idle(s, grace + 10, c, log);
  const SimEvent& ended = firstEvent(log, EventKind::RoundEnded);
  CHECK_FALSE(ended.player.isValid());
  CHECK(ended.tick.value == out.tick.value + grace - 1u);
  CHECK(s.playerBySlot(kP1) != nullptr);
  CHECK(s.playerBySlot(kP2) != nullptr);
}
