// Pit falls end to end (architecture §5.6, §5.10, v0.57): a monkey whose
// whole contact disc ends a tick over the arena's pits (their union) while
// it is not dashing falls for PlayerDef::fallTicks ticks (counting the tick
// it went in), out of control and out of reach, and is then eliminated by
// nobody. A dash carries it over a pit narrower than the dash's reach and
// drops it into one it cannot clear; a body partly over the edge stays up,
// one straddling two adjoining pit boxes goes, and a pit narrower than the
// body takes nobody; a round waits for every fall to end. A dead (or doomed)
// monkey's bananas in flight drop where they are as pickups, unless they
// come down over a pit, where they drop into the hole.
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
const PlayerId kP2{2};

constexpr uint8_t kRight = 0;  // input angles: 0 = +x, 128 = -x
constexpr uint8_t kLeft = 128;

constexpr int32_t kNarrowMinX = -9;  // the narrow pit: x in [-9, -7], 2 units
constexpr int32_t kNarrowMaxX = -7;
constexpr int32_t kWideMinX = 3;  // the wide pit: x in [3, 9], 6 units
constexpr int32_t kWideMaxX = 9;
constexpr int32_t kSeamX = 13;  // the split pit: [11, 13] and [13, 15], two boxes

Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}
Vec2 atRatio(int32_t num, int32_t den, int32_t y) {
  return Vec2{Fixed::fromRatio(num, den), Fixed::fromInt(y)};
}

// The default content plus arena 1, "pits": the dojo's box crossed by pit
// strips running wall to wall: a narrow one (index 0) west of the centre, a
// wide one (1) east of it, one split into two adjoining boxes (2, 3) at the
// east end, and one narrower than a monkey (4, 0.8 units). A dash (4.4
// units) clears the narrow one from beside it and not the wide one.
ContentDB pitContent() {
  ContentDB c = defaultContent();
  ArenaDef& a = c.arenas[1];
  a = c.arenas[0];
  a.name = ContentName::of("pits");
  a.spawnPoints[0] = at(-12, 0);
  a.spawnPoints[1] = at(0, 6);
  a.spawnPoints[2] = at(-14, -7);
  a.spawnPointCount = 3;
  a.wallCount = 0;
  a.hazards[0] = Hazard{at(kNarrowMinX, -9), at(kNarrowMaxX, 9), HazardKind::Pit, Fixed::one()};
  a.hazards[1] = Hazard{at(kWideMinX, -9), at(kWideMaxX, 9), HazardKind::Pit, Fixed::one()};
  a.hazards[2] = Hazard{at(kSeamX - 2, -9), at(kSeamX, 9), HazardKind::Pit, Fixed::one()};
  a.hazards[3] = Hazard{at(kSeamX, -9), at(kSeamX + 2, 9), HazardKind::Pit, Fixed::one()};
  a.hazards[4] = Hazard{atRatio(-22, 5, -9), atRatio(-18, 5, 9), HazardKind::Pit, Fixed::one()};
  a.hazardCount = 5;
  c.arenaCount = 2;
  return c;
}

struct Lab {
  ContentDB db;
  MatchConfig config;
  MatchContent content;
  WorldState s;
  EventLog all;

  explicit Lab(PlayerMask players, ContentDB contentDb = pitContent())
      : db(contentDb),
        config{.seed = 0x917, .players = players, .arena = 1},
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
  [[nodiscard]] Fixed radius() const { return db.player.radius; }
};

[[nodiscard]] bool overNarrowPit(Fixed x) {
  return x >= Fixed::fromInt(kNarrowMinX) && x <= Fixed::fromInt(kNarrowMaxX);
}

// Somewhere clear of every pit for the bystander.
Vec2 aside() {
  return at(-14, 7);
}

}  // namespace

TEST_CASE("a dash carries a monkey across a narrow pit", "[scenario][pit]") {
  Lab lab(0b11);
  placePlayer(lab.s, kP0, at(-6, 0));  // the centre one unit east of the edge
  placePlayer(lab.s, kP1, aside());
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
  placePlayer(lab.s, kP1, aside());
  const int fallTicks = lab.db.player.fallTicks;
  REQUIRE(fallTicks > 1);
  // Walk west holding Throw (a charging wind-up) until the whole body is
  // over the pit: the centre a radius past the edge.
  const Fixed goesAt = Fixed::fromInt(kNarrowMaxX) - lab.radius();
  int fellOn = -1;
  for (int t = 0; t < 60 && fellOn < 0; ++t) {
    const Fixed before = playerAt(lab.s, kP0).transform.pos.x;
    const EventLog log = lab.step(PlayerInput{kLeft, 255, kLeft, kThrow, 0});
    if (const SimEvent* e = findEvent(log, EventKind::PlayerFell)) {
      CHECK(e->player == kP0);
      CHECK(e->a == 0);  // the narrow pit's hazard index
      CHECK(before > goesAt);
      CHECK(playerAt(lab.s, kP0).transform.pos.x <= goesAt);
      fellOn = t;
    }
  }
  REQUIRE(fellOn >= 0);
  const Vec2 wentIn = playerAt(lab.s, kP0).transform.pos;
  CHECK(countEvents(lab.all, EventKind::BoomerangThrown) == 0);
  // The fall: no control (a release, a dash, the stick east), no movement,
  // the held banana still in hand, and the round goes on until it ends.
  int elapsed = 1;  // the tick it went in counts
  for (; elapsed < fallTicks; ++elapsed) {
    const Player& p = playerAt(lab.s, kP0);
    REQUIRE(p.tags.has(Tag::Falling));
    CHECK(p.transform.pos == wentIn);
    CHECK(p.abilities[kAbilitySlotThrow].phase == AbilityPhase::Idle);
    CHECK(p.ammo == lab.db.player.ammoStart);
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
  CHECK(countEvents(lab.all, EventKind::PickupSpawned) == 0);  // its banana went down too
  CHECK(lab.s.playerBySlot(kP0) == nullptr);
  const SimEvent& ended = firstEvent(lab.all, EventKind::RoundEnded);
  CHECK(ended.player == kP1);
  CHECK(ended.tick == out.tick);
}

TEST_CASE("a dash that ends over a wide pit falls when the dash ends", "[scenario][pit]") {
  Lab lab(0b11);
  placePlayer(lab.s, kP0, at(2, 0));  // one unit west of the wide pit
  placePlayer(lab.s, kP1, aside());
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

TEST_CASE("a monkey falls only once its whole body is over the pit", "[scenario][pit]") {
  Lab lab(0b11);
  placePlayer(lab.s, kP1, aside());
  const Fixed edge = Fixed::fromInt(kNarrowMaxX);
  const auto stays = [&](Fixed x) {
    placePlayer(lab.s, kP0, Vec2{x, Fixed::zero()});
    for (int t = 0; t < 30; ++t)
      lab.step();
    return countEvents(lab.all, EventKind::PlayerFell) == 0;
  };
  CHECK(stays(edge));                                    // the centre on the edge
  CHECK(stays(edge - (lab.radius() / 2)));               // three quarters of the body over it
  CHECK(stays(edge - lab.radius() + Fixed::epsilon()));  // a sliver of the disc on the floor
  // The disc touching the edge from inside (the boxes are closed): it goes.
  placePlayer(lab.s, kP0, Vec2{edge - lab.radius(), Fixed::zero()});
  const EventLog log = lab.step();
  CHECK(countEvents(log, EventKind::PlayerFell) == 1);
  CHECK(playerAt(lab.s, kP0).tags.has(Tag::Falling));
}

TEST_CASE("a pit made of adjoining boxes is one pit; one narrower than a monkey takes nobody",
          "[scenario][pit]") {
  SECTION("straddling the seam of two pit boxes") {
    Lab lab(0b11);
    placePlayer(lab.s, kP1, aside());
    placePlayer(lab.s, kP0, at(kSeamX, 0));  // half the body over each box
    const EventLog log = lab.step();
    const SimEvent* fell = findEvent(log, EventKind::PlayerFell);
    REQUIRE(fell != nullptr);
    CHECK((fell->a == 2 || fell->a == 3));
  }
  SECTION("0.8 units wide, centred in it") {
    Lab lab(0b11);
    placePlayer(lab.s, kP1, aside());
    REQUIRE(lab.radius() * 2 > Fixed::fromRatio(4, 5));
    placePlayer(lab.s, kP0, at(-4, 0));
    for (int t = 0; t < 60; ++t)
      lab.step();
    CHECK(countEvents(lab.all, EventKind::PlayerFell) == 0);
  }
}

TEST_CASE("a falling monkey is out of reach: bananas fly over it", "[scenario][pit]") {
  Lab lab(0b11);
  placePlayer(lab.s, kP1, aside());
  placePlayer(lab.s, kP0, at(-8, 0));  // the middle of the narrow pit
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
        b->transform.pos.x < Fixed::fromInt(-9)) {
      passed = true;
    }
  }
  CHECK(passed);
  CHECK(firstEvent(lab.all, EventKind::PlayerEliminated).a == PlayerId::invalid().value);
}

TEST_CASE("a round waits for every fall: a doomed last monkey does not win", "[scenario][pit]") {
  Lab lab(0b11);
  // Both walk into a pit, P1 a few ticks after P0.
  placePlayer(lab.s, kP0, atRatio(-13, 2, 0));  // its body half a unit from the narrow pit
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

TEST_CASE("a hit monkey's banana in flight drops on the spot and anyone may take it",
          "[scenario][pit][banana]") {
  Lab lab(0b111);
  placePlayer(lab.s, kP0, at(-12, -6)).ammo = 0;
  placePlayer(lab.s, kP1, at(0, 6));
  placePlayer(lab.s, kP2, at(10, 6)).ammo = 0;
  // P0's banana flying east over the floor between the narrow and wide pits.
  const Vec2 where = at(0, -4);
  const EntityId own = spawnBanana(lab.s, kP0, where, Vec2{Fixed::fromRatio(1, 10), Fixed{}},
                                   BoomerangPhase::Outbound, lab.db);
  // P1's banana about to hit P0.
  spawnBanana(lab.s, kP1, at(-11, -6), Vec2{-Fixed::fromRatio(1, 4), Fixed{}},
              BoomerangPhase::Outbound, lab.db);
  const EventLog log = lab.step();
  const SimEvent* out = findEvent(log, EventKind::PlayerEliminated);
  REQUIRE(out != nullptr);
  REQUIRE(out->player == kP0);
  // On the tick of the death: grounded and laid as a pickup where it was.
  CHECK(lab.s.boomerangs.get(own) == nullptr);
  const SimEvent& grounded = firstEvent(log, EventKind::BoomerangGrounded);
  CHECK(grounded.entity == own);
  const SimEvent& laid = firstEvent(log, EventKind::PickupSpawned);
  const Pickup* pickup = lab.s.pickups.get(laid.entity);
  REQUIRE(pickup != nullptr);
  CHECK(pickup->kind == PickupKind::Banana);
  CHECK(abs(pickup->pos.x - where.x) <= Fixed::fromRatio(1, 5));
  CHECK(pickup->pos.y == where.y);
  // Anyone may take it: P2 walks onto it.
  placePlayer(lab.s, kP2, pickup->pos);
  const EventLog taken = lab.step();
  const SimEvent* took = findEvent(taken, EventKind::PickupTaken);
  REQUIRE(took != nullptr);
  CHECK(took->player == kP2);
  CHECK(playerAt(lab.s, kP2).ammo == 1);
}

TEST_CASE("a faller's banana in flight drops the moment the fall begins",
          "[scenario][pit][banana]") {
  Lab lab(0b111);
  placePlayer(lab.s, kP0, at(-8, 0)).ammo = 0;  // wholly over the narrow pit
  placePlayer(lab.s, kP1, aside());
  placePlayer(lab.s, kP2, at(10, 6)).ammo = 0;
  const EntityId own = spawnBanana(lab.s, kP0, at(0, -4), Vec2{Fixed::fromRatio(1, 10), Fixed{}},
                                   BoomerangPhase::Returning, lab.db);
  const EventLog log = lab.step();
  REQUIRE(findEvent(log, EventKind::PlayerFell) != nullptr);
  CHECK(countEvents(log, EventKind::PlayerEliminated) == 0);  // doomed, not out yet
  CHECK(lab.s.boomerangs.get(own) == nullptr);
  CHECK(firstEvent(log, EventKind::BoomerangGrounded).entity == own);
  const Pickup* pickup = lab.s.pickups.get(firstEvent(log, EventKind::PickupSpawned).entity);
  REQUIRE(pickup != nullptr);
  placePlayer(lab.s, kP2, pickup->pos);
  const EventLog taken = lab.step();
  REQUIRE(findEvent(taken, EventKind::PickupTaken) != nullptr);
  CHECK(firstEvent(taken, EventKind::PickupTaken).player == kP2);
}

TEST_CASE("a banana that comes down over a pit drops into the hole, never a pickup",
          "[scenario][pit][banana]") {
  Lab lab(0b111);
  placePlayer(lab.s, kP0, at(-8, 0));  // wholly over the narrow pit, holding its banana
  placePlayer(lab.s, kP1, aside());
  placePlayer(lab.s, kP2, at(10, 6));
  REQUIRE(playerAt(lab.s, kP0).ammo > 0);  // the one in its hand goes down with it
  // Its other banana over the middle of the wide pit when the fall begins.
  const Vec2 overPit = at(6, -3);
  const EntityId own = spawnBanana(lab.s, kP0, overPit, Vec2{Fixed::fromRatio(1, 100), Fixed{}},
                                   BoomerangPhase::Returning, lab.db);
  const EventLog log = lab.step();
  REQUIRE(findEvent(log, EventKind::PlayerFell) != nullptr);
  const SimEvent* in = findEvent(log, EventKind::BoomerangFell);
  REQUIRE(in != nullptr);
  CHECK(in->entity == own);
  CHECK(in->player == kP0);
  const Boomerang* b = lab.s.boomerangs.get(own);
  REQUIRE(b != nullptr);
  CHECK(b->phase == BoomerangPhase::Sinking);
  CHECK(countEvents(log, EventKind::PickupSpawned) == 0);
  // It sinks for pitSinkTicks, then is gone; it was never a pickup.
  for (int t = 0; t < lab.db.boomerang.pitSinkTicks + 1; ++t)
    lab.step();
  CHECK(lab.s.boomerangs.get(own) == nullptr);
  CHECK(countEvents(lab.all, EventKind::PickupSpawned) == 0);
  CHECK(lab.s.pickups.empty());

  // A banana grounding over a pit for any reason (the return clock, a slam)
  // goes the same way.
  const EntityId other =
      spawnBanana(lab.s, kP1, at(6, 3), Vec2{}, BoomerangPhase::Grounded, lab.db);
  const EventLog next = lab.step();
  CHECK(firstEvent(next, EventKind::BoomerangFell).entity == other);
  CHECK(lab.s.pickups.empty());
}

TEST_CASE("after a fall the next round hands out bananas as ever", "[scenario][pit][banana]") {
  Lab lab(0b11);
  placePlayer(lab.s, kP1, aside());
  placePlayer(lab.s, kP0, at(-8, 0));
  while (lab.s.playerBySlot(kP0) != nullptr)
    lab.step();
  const uint8_t round = lab.s.rules.round;
  const auto nextRoundOn = [&] {
    return lab.s.rules.round > round && lab.s.rules.phase == MatchPhase::Playing;
  };
  for (int t = 0; t < 1200 && !nextRoundOn(); ++t)
    lab.step();
  REQUIRE(nextRoundOn());
  CHECK(playerAt(lab.s, kP0).ammo == lab.db.player.ammoStart);
  CHECK(playerAt(lab.s, kP1).ammo == lab.db.player.ammoStart);
  CHECK_FALSE(playerAt(lab.s, kP0).tags.has(Tag::Falling));
}
