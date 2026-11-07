// Pit falls end to end (architecture §5.6, §5.10, v0.57): a monkey whose
// centre ends a tick over a pit while it is not dashing falls for
// PlayerDef::fallTicks ticks (counting the tick it went in), out of control
// and out of reach, and is then eliminated by nobody. A dash carries it over
// a pit narrower than the dash's reach and drops it into one it cannot
// clear; the body may overhang the edge as long as the centre does not; a
// round waits for every fall to end.
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kDash = buttonBit(Button::Dash);
constexpr uint8_t kThrow = buttonBit(Button::Throw);
const PlayerId kP0{0};
const PlayerId kP1{1};

constexpr uint8_t kRight = 0;  // input angles: 0 = +x, 128 = -x
constexpr uint8_t kLeft = 128;

constexpr int32_t kNarrowMinX = -9;  // the narrow pit: x in [-9, -7], 2 units
constexpr int32_t kNarrowMaxX = -7;
constexpr int32_t kWideMinX = 3;  // the wide pit: x in [3, 9], 6 units
constexpr int32_t kWideMaxX = 9;

Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}
Vec2 atRatio(int32_t num, int32_t den, int32_t y) {
  return Vec2{Fixed::fromRatio(num, den), Fixed::fromInt(y)};
}

// The default content plus arena 1, "pits": the dojo's box crossed by two
// pit strips running wall to wall, a narrow one west of the centre and a
// wide one east of it. A dash (4.4 units) clears the narrow one from beside
// it and not the wide one.
ContentDB pitContent() {
  ContentDB c = defaultContent();
  ArenaDef& a = c.arenas[1];
  a = c.arenas[0];
  a.name = ContentName::of("pits");
  a.spawnPoints[0] = at(-12, 0);
  a.spawnPoints[1] = at(12, 0);
  a.spawnPointCount = 2;
  a.wallCount = 0;
  a.hazards[0] = Hazard{at(kNarrowMinX, -9), at(kNarrowMaxX, 9), HazardKind::Pit, Fixed::one()};
  a.hazards[1] = Hazard{at(kWideMinX, -9), at(kWideMaxX, 9), HazardKind::Pit, Fixed::one()};
  a.hazardCount = 2;
  c.arenaCount = 2;
  return c;
}

struct Lab {
  ContentDB db = pitContent();
  MatchConfig config;
  MatchContent content;
  WorldState s;
  EventLog all;

  explicit Lab(PlayerMask players)
      : config{.seed = 0x917, .players = players, .arena = 1},
        content(matchContent(db, config)),
        s(initialWorld(config)) {
    EventLog log;
    for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i) {
      runTicks(s, frameFor(s), 1, content, log);
    }
    REQUIRE(s.rules.phase == MatchPhase::Playing);
  }
  Lab(const Lab&) = delete;
  Lab& operator=(const Lab&) = delete;

  EventLog step(PlayerInput p0 = {}, PlayerInput p1 = {}) {
    InputFrame f = frameFor(s);
    f.inputs[0] = p0;
    f.inputs[1] = p1;
    EventLog log;
    runTicks(s, f, 1, content, log);
    all.insert(all.end(), log.begin(), log.end());
    return log;
  }
  [[nodiscard]] const AbilityDef& dash() const { return db.abilityForSlot(kAbilitySlotDash); }
};

[[nodiscard]] bool overNarrowPit(Fixed x) {
  return x >= Fixed::fromInt(kNarrowMinX) && x <= Fixed::fromInt(kNarrowMaxX);
}

}  // namespace

TEST_CASE("a dash carries a monkey across a narrow pit", "[scenario][pit]") {
  Lab lab(0b11);
  placePlayer(lab.s, kP0, at(-6, 0));  // the centre one unit east of the edge
  placePlayer(lab.s, kP1, at(12, 0));
  lab.step(PlayerInput{kLeft, 255, kLeft, kDash, 0});
  REQUIRE(findEvent(lab.all, EventKind::PlayerDashed) != nullptr);
  int ticksOverPit = 0;
  for (int t = 0; t < lab.dash().activeTicks + lab.dash().recoveryTicks + 30; ++t) {
    const Player& p = playerAt(lab.s, kP0);
    if (overNarrowPit(p.transform.pos.x)) {
      CHECK(p.tags.has(Tag::Dashing));  // only ever over the pit mid-dash
      ++ticksOverPit;
    }
    lab.step();
  }
  CHECK(ticksOverPit > 0);
  CHECK(countEvents(lab.all, EventKind::PlayerFell) == 0);
  const Player& p = playerAt(lab.s, kP0);
  CHECK_FALSE(p.tags.has(Tag::Falling));
  CHECK(p.transform.pos.x < Fixed::fromInt(kNarrowMinX));  // landed on the far side
}

TEST_CASE("walking into a pit falls out of control, then is eliminated by nobody",
          "[scenario][pit]") {
  Lab lab(0b11);
  placePlayer(lab.s, kP0, at(-6, 0));
  placePlayer(lab.s, kP1, at(12, 0));
  const int fallTicks = lab.db.player.fallTicks;
  REQUIRE(fallTicks > 1);
  // Walk west holding Throw (a charging wind-up) until the centre crosses
  // the edge.
  int fellOn = -1;
  for (int t = 0; t < 60 && fellOn < 0; ++t) {
    const Fixed before = playerAt(lab.s, kP0).transform.pos.x;
    const EventLog log = lab.step(PlayerInput{kLeft, 255, kLeft, kThrow, 0});
    if (const SimEvent* e = findEvent(log, EventKind::PlayerFell)) {
      CHECK(e->player == kP0);
      CHECK(e->a == 0);  // the narrow pit's hazard index
      CHECK(before > Fixed::fromInt(kNarrowMaxX));
      CHECK(playerAt(lab.s, kP0).transform.pos.x <= Fixed::fromInt(kNarrowMaxX));
      fellOn = t;
    }
  }
  REQUIRE(fellOn >= 0);
  const Vec2 wentIn = playerAt(lab.s, kP0).transform.pos;
  CHECK(countEvents(lab.all, EventKind::BoomerangThrown) == 0);
  // The fall: no control (a release, a dash, the stick east), no movement,
  // and the round goes on until it ends.
  int elapsed = 1;  // the tick it went in counts
  for (; elapsed < fallTicks; ++elapsed) {
    const Player& p = playerAt(lab.s, kP0);
    REQUIRE(p.tags.has(Tag::Falling));
    CHECK(p.transform.pos == wentIn);
    CHECK(p.abilities[kAbilitySlotThrow].phase == AbilityPhase::Idle);
    CHECK(lab.s.rules.phase == MatchPhase::Playing);
    const uint8_t buttons = (elapsed % 2) == 0 ? kDash : uint8_t{0};  // and Throw released
    const EventLog log = lab.step(PlayerInput{kRight, 255, kRight, buttons, 0});
    if (elapsed + 1 < fallTicks) CHECK(countEvents(log, EventKind::PlayerEliminated) == 0);
  }
  CHECK(countEvents(lab.all, EventKind::BoomerangThrown) == 0);
  CHECK(countEvents(lab.all, EventKind::PlayerDashed) == 0);
  // Out on the fallTicks-th tick counting the one it went in.
  const SimEvent& out = firstEvent(lab.all, EventKind::PlayerEliminated);
  CHECK(out.player == kP0);
  CHECK(out.a == PlayerId::invalid().value);  // killer 0xFF
  CHECK(out.tick.value == firstEvent(lab.all, EventKind::PlayerFell).tick.value +
                              static_cast<uint32_t>(fallTicks) - 1);
  CHECK(countEvents(lab.all, EventKind::PlayerHit) == 0);
  CHECK(lab.s.playerBySlot(kP0) == nullptr);
  const SimEvent& ended = firstEvent(lab.all, EventKind::RoundEnded);
  CHECK(ended.player == kP1);
  CHECK(ended.tick == out.tick);
}

TEST_CASE("a dash that ends over a wide pit falls when the dash ends", "[scenario][pit]") {
  Lab lab(0b11);
  placePlayer(lab.s, kP0, at(2, 0));  // one unit west of the wide pit
  placePlayer(lab.s, kP1, at(-12, 0));
  lab.step(PlayerInput{kRight, 255, kRight, kDash, 0});
  REQUIRE(findEvent(lab.all, EventKind::PlayerDashed) != nullptr);
  const Tick pressed = firstEvent(lab.all, EventKind::PlayerDashed).tick;
  for (int t = 0; t < 30 && findEvent(lab.all, EventKind::PlayerFell) == nullptr; ++t) {
    CHECK(playerAt(lab.s, kP0).tags.has(Tag::Dashing));  // no fall while dashing
    lab.step();
  }
  const SimEvent& fell = firstEvent(lab.all, EventKind::PlayerFell);
  CHECK(fell.player == kP0);
  CHECK(fell.a == 1);
  // The first tick after the dash's Active phase.
  CHECK(fell.tick.value == pressed.value + lab.dash().activeTicks);
  const Player& p = playerAt(lab.s, kP0);
  CHECK_FALSE(p.tags.has(Tag::Dashing));
  CHECK(p.transform.pos.x > Fixed::fromInt(kWideMinX));
  CHECK(p.transform.pos.x < Fixed::fromInt(kWideMaxX));
}

TEST_CASE("a monkey falls when its centre is over the pit, not its body", "[scenario][pit]") {
  Lab lab(0b11);
  placePlayer(lab.s, kP1, at(12, 0));
  // Three quarters of a unit from the edge: half the radius hangs over it.
  placePlayer(lab.s, kP0, atRatio(-27, 4, 0));
  REQUIRE(lab.db.player.radius > Fixed::fromRatio(1, 4));
  for (int t = 0; t < 60; ++t)
    lab.step();
  CHECK(countEvents(lab.all, EventKind::PlayerFell) == 0);
  // The centre on the edge (the box is closed): it goes.
  placePlayer(lab.s, kP0, at(kNarrowMaxX, 0));
  const EventLog log = lab.step();
  CHECK(countEvents(log, EventKind::PlayerFell) == 1);
  CHECK(playerAt(lab.s, kP0).tags.has(Tag::Falling));
}

TEST_CASE("a falling monkey is out of reach: bananas fly over it", "[scenario][pit]") {
  Lab lab(0b11);
  placePlayer(lab.s, kP1, at(12, 0));
  placePlayer(lab.s, kP0, at(kNarrowMaxX, 0));
  lab.step();
  REQUIRE(playerAt(lab.s, kP0).tags.has(Tag::Falling));
  // P1's banana flying west straight through P0's centre.
  const EntityId id = spawnBanana(lab.s, kP1, at(-5, 0), Vec2{-Fixed::fromRatio(1, 4), Fixed{}},
                                  BoomerangPhase::Outbound, lab.db);
  bool passed = false;
  while (lab.s.playerBySlot(kP0) != nullptr) {
    const EventLog log = lab.step();
    CHECK(countEvents(log, EventKind::PlayerHit) == 0);
    const Boomerang* b = lab.s.boomerangs.get(id);
    if (b != nullptr && lab.s.playerBySlot(kP0) != nullptr &&
        b->transform.pos.x < Fixed::fromInt(kNarrowMaxX - 1)) {
      passed = true;
    }
  }
  CHECK(passed);
  CHECK(firstEvent(lab.all, EventKind::PlayerEliminated).a == PlayerId::invalid().value);
}

TEST_CASE("a round waits for every fall: a doomed last monkey does not win", "[scenario][pit]") {
  Lab lab(0b11);
  // Both walk into a pit, P1 a few ticks after P0.
  placePlayer(lab.s, kP0, atRatio(-13, 2, 0));  // half a unit from the narrow pit
  placePlayer(lab.s, kP1, at(2, 0));            // one unit from the wide pit
  int falls = 0;
  for (int t = 0; t < 200 && lab.s.rules.phase == MatchPhase::Playing; ++t) {
    const EventLog log =
        lab.step(PlayerInput{kLeft, 255, kLeft, 0, 0}, PlayerInput{kRight, 255, kRight, 0, 0});
    falls += countEvents(log, EventKind::PlayerFell);
    if (countEvents(log, EventKind::PlayerEliminated) > 0 && lab.s.players.size() == 1) {
      // The first one out: the other is falling, so the round goes on.
      CHECK(countEvents(log, EventKind::RoundEnded) == 0);
    }
  }
  CHECK(falls == 2);
  CHECK(countEvents(lab.all, EventKind::PlayerEliminated) == 2);
  const SimEvent& p0Fell = firstEvent(lab.all, EventKind::PlayerFell);
  CHECK(p0Fell.player == kP0);  // P0 went in first, so P1 was the last one standing
  const SimEvent& ended = firstEvent(lab.all, EventKind::RoundEnded);
  CHECK_FALSE(ended.player.isValid());  // a draw
  CHECK(lab.s.rules.scores[0] == 0);
  CHECK(lab.s.rules.scores[1] == 0);
}
