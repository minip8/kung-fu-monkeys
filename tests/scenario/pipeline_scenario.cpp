// End-to-end M1 scenarios through step() with scripted PlayerInput frames and
// the default content (architecture §5.3, §5.6, §5.7, §5.11, §9). Nothing is
// hand-built past the starting positions: every banana is thrown, flies,
// bounces and returns through the real systems.
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kThrow = buttonBit(Button::Throw);
constexpr uint8_t kDash = buttonBit(Button::Dash);
constexpr uint8_t kAimRight = 0;   // +x
constexpr uint8_t kAimLeft = 128;  // -x
constexpr uint8_t kMoveRight = 0;  // moveDir 0 is +x now that moveMag carries idle (§5.4)
const PlayerId kP0{0};
const PlayerId kP1{1};

Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

// Kinds of the events in a log, filtered to `interesting`, in order.
std::vector<EventKind> kindsOf(const EventLog& log, std::initializer_list<EventKind> interesting) {
  std::vector<EventKind> out;
  for (const SimEvent& e : log) {
    for (const EventKind k : interesting) {
      if (e.kind == k) out.push_back(e.kind);
    }
  }
  return out;
}

// --- the duel used by the catch scenarios -------------------------------------

struct DashPress {
  int tick{-1};  // ticks after the throw press; -1 = never
  uint8_t subTick{0};
};

// Player 1 at (4, 0) throws at player 0 at (-4, 0) on relative tick 0 (the
// banana spawns on tick 2 and closes at 16 u/s). Player 0 faces +x with the
// stick idle and presses Dash (along facing, toward the banana) on
// relative tick `press.tick` at `press.subTick`. Without a dash the banana
// hits on relative tick 25.
EventLog duel(DashPress press, WorldState* out = nullptr) {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(31, 0b11);
  placePlayer(s, kP0, at(-4, 0));
  placePlayer(s, kP1, at(4, 0));
  EventLog log;
  for (int k = 0; k < 90; ++k) {
    InputFrame f = frameFor(s);
    f.inputs[0].aimDir = kAimRight;
    f.inputs[1].aimDir = kAimLeft;
    if (k == 0) f.inputs[1].buttons = kThrow;
    if (k == press.tick) {
      f.inputs[0].buttons = kDash;
      f.inputs[0].subTick = press.subTick;
    }
    runTicks(s, f, 1, content, log);
  }
  if (out != nullptr) *out = s;
  return log;
}

const SimEvent& onlyEvent(const EventLog& log, EventKind kind) {
  REQUIRE(countEvents(log, kind) == 1);
  return *findEvent(log, kind);
}

}  // namespace

TEST_CASE("a throw at a wall bounces, returns and is auto-caught", "[scenario][pipeline]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(41, 0b11);
  placePlayer(s, kP0, at(12, 0));    // 3.65 units from where a banana bounces off +x
  placePlayer(s, kP1, at(-12, -6));  // far from the whole flight
  const Tick start = s.tick;

  InputFrame f = frameFor(s);
  f.inputs[0].aimDir = kAimRight;
  f.inputs[0].buttons = kThrow;
  EventLog log;
  runTicks(s, f, 1, content, log);
  f.inputs[0].buttons = 0;

  // Watch the banana phase on the bounce tick.
  BoomerangPhase phaseAtBounce = BoomerangPhase::Grounded;
  for (int k = 0; k < 240 && countEvents(log, EventKind::BoomerangCaught) == 0; ++k) {
    const size_t before = log.size();
    runTicks(s, f, 1, content, log);
    for (size_t i = before; i < log.size(); ++i) {
      if (log[i].kind == EventKind::BoomerangBounced) {
        const Boomerang* b = s.boomerangs.get(log[i].entity);
        REQUIRE(b != nullptr);
        phaseAtBounce = b->phase;
      }
    }
    if (k == 2) CHECK(playerAt(s, kP0).ammo == 0);  // banana in the air
  }

  CHECK(kindsOf(log,
                {EventKind::BoomerangThrown, EventKind::BoomerangBounced,
                 EventKind::BoomerangCaught, EventKind::PlayerHit, EventKind::BoomerangGrounded}) ==
        std::vector<EventKind>{EventKind::BoomerangThrown, EventKind::BoomerangBounced,
                               EventKind::BoomerangCaught});
  CHECK(phaseAtBounce == BoomerangPhase::Outbound);
  const SimEvent& thrown = onlyEvent(log, EventKind::BoomerangThrown);
  const SimEvent& bounced = onlyEvent(log, EventKind::BoomerangBounced);
  const SimEvent& caught = onlyEvent(log, EventKind::BoomerangCaught);
  CHECK(thrown.tick == start.plus(2));  // Throw startup
  CHECK(bounced.entity == thrown.entity);
  CHECK(caught.entity == thrown.entity);
  CHECK(caught.player == kP0);
  CHECK(caught.a == static_cast<uint32_t>(CatchHow::Auto));
  // It flew out for outboundTicks before turning back.
  CHECK(caught.tick.minus(thrown.tick) > content.boomerang.outboundTicks);
  CHECK(playerAt(s, kP0).ammo == 1);
  CHECK(s.boomerangs.empty());
  CHECK(s.pickups.empty());
}

TEST_CASE("throws decide rounds; rounds respawn everyone; roundsToWin ends the match",
          "[scenario][pipeline]") {
  const ContentDB& content = defaultContent();
  WorldState s = initialWorld(MatchConfig{42, 0b11});
  EventLog log;
  for (uint8_t round = 1; round <= content.mode.roundsToWin; ++round) {
    INFO("round " << int{round});
    // Through the countdown with idle inputs.
    for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i) {
      runTicks(s, frameFor(s), 1, content, log);
    }
    REQUIRE(s.rules.phase == MatchPhase::Playing);
    REQUIRE(s.rules.round == round);
    // Both respawned whole: full hp, starting ammo, nothing left in the arena.
    for (const PlayerId id : {kP0, kP1}) {
      const Player* p = s.playerBySlot(id);
      REQUIRE(p != nullptr);
      CHECK(p->hp == content.player.maxHp);
      CHECK(p->ammo == content.player.ammoStart);
      CHECK(p->tags == Tags{});
    }
    CHECK(s.boomerangs.empty());
    CHECK(s.pickups.empty());

    // Player 1 throws at a stationary player 0.
    placePlayer(s, kP0, at(-4, 0));
    placePlayer(s, kP1, at(4, 0));
    EventLog roundLog;
    InputFrame f = frameFor(s);
    f.inputs[0].aimDir = kAimRight;
    f.inputs[1].aimDir = kAimLeft;
    f.inputs[1].buttons = kThrow;
    runTicks(s, f, 1, content, roundLog);
    f.inputs[1].buttons = 0;
    for (int i = 0; i < 120 && s.rules.phase == MatchPhase::Playing; ++i) {
      runTicks(s, f, 1, content, roundLog);
    }

    CHECK(kindsOf(roundLog,
                  {EventKind::BoomerangThrown, EventKind::PlayerHit, EventKind::PlayerEliminated,
                   EventKind::BoomerangGrounded, EventKind::PickupSpawned, EventKind::RoundEnded,
                   EventKind::MatchEnded, EventKind::BoomerangCaught}) ==
          (round < content.mode.roundsToWin
               ? std::vector<EventKind>{EventKind::BoomerangThrown, EventKind::PlayerHit,
                                        EventKind::PlayerEliminated, EventKind::BoomerangGrounded,
                                        EventKind::PickupSpawned, EventKind::RoundEnded}
               : std::vector<EventKind>{EventKind::BoomerangThrown, EventKind::PlayerHit,
                                        EventKind::PlayerEliminated, EventKind::BoomerangGrounded,
                                        EventKind::PickupSpawned, EventKind::RoundEnded,
                                        EventKind::MatchEnded}));
    const SimEvent& ended = onlyEvent(roundLog, EventKind::RoundEnded);
    CHECK(ended.player == kP1);
    CHECK(ended.a == round);
    CHECK(onlyEvent(roundLog, EventKind::PlayerHit).tick == ended.tick);
    CHECK(s.rules.scores[1] == round);
    CHECK(s.rules.scores[0] == 0);
    CHECK(s.playerBySlot(kP0) == nullptr);
    log.insert(log.end(), roundLog.begin(), roundLog.end());
  }
  CHECK(s.rules.phase == MatchPhase::MatchEnded);
  CHECK(onlyEvent(log, EventKind::MatchEnded).player == kP1);
  CHECK(countEvents(log, EventKind::RoundStarted) == content.mode.roundsToWin);
  CHECK(countEvents(log, EventKind::MatchStarted) == 1);

  // MatchEnded is terminal: nothing restarts, nothing happens.
  EventLog after;
  runTicks(s, frameFor(s), content.mode.roundEndTicks * 2, content, after);
  CHECK(after.empty());
  CHECK(s.rules.phase == MatchPhase::MatchEnded);
}

TEST_CASE("a perfect dash catches the incoming banana; mistimed dashes get hit",
          "[scenario][pipeline]") {
  const ContentDB& content = defaultContent();
  REQUIRE(content.player.ammoMax == 1);

  SECTION("well timed: caught, no hit, ammo stays at the cap") {
    WorldState s;
    const EventLog log = duel(DashPress{18, 0}, &s);
    const SimEvent& caught = onlyEvent(log, EventKind::BoomerangCaught);
    CHECK(caught.player == kP0);
    CHECK(caught.a == static_cast<uint32_t>(CatchHow::Perfect));
    CHECK(caught.entity == onlyEvent(log, EventKind::BoomerangThrown).entity);
    CHECK(countEvents(log, EventKind::PlayerHit) == 0);
    CHECK(countEvents(log, EventKind::RoundEnded) == 0);
    // The catcher held a banana already: ammoMax caps it at 1.
    CHECK(playerAt(s, kP0).ammo == content.player.ammoMax);
    // The thrower's banana is gone; it stays empty-handed.
    CHECK(playerAt(s, kP1).ammo == 0);
    CHECK(s.boomerangs.empty());
  }
  SECTION("too early: the window has closed when the banana arrives") {
    const EventLog log = duel(DashPress{8, 0});
    CHECK(countEvents(log, EventKind::PlayerDashed) == 1);
    CHECK(countEvents(log, EventKind::BoomerangCaught) == 0);
    CHECK(onlyEvent(log, EventKind::PlayerHit).player == kP0);
    CHECK(onlyEvent(log, EventKind::RoundEnded).player == kP1);
  }
  SECTION("too late: the banana lands before the press") {
    const EventLog log = duel(DashPress{26, 0});
    CHECK(countEvents(log, EventKind::BoomerangCaught) == 0);
    CHECK(onlyEvent(log, EventKind::PlayerHit).player == kP0);
    CHECK(countEvents(log, EventKind::PlayerDashed) == 0);  // eliminated before pressing
  }
}

// §5.11: the dash starts on the tick boundary whatever the sub-tick, so these
// pairs move identically and meet the banana at the same tick and tHit; only
// the press instant tick + subTick/256 differs, and it alone decides.
TEST_CASE("the press sub-tick decides a catch at the window edges", "[scenario][pipeline]") {
  SECTION("closing edge: contact 4 ticks after the press, early in the tick") {
    // Window [press, press + 4): pressed at sub-tick 0 it closed on the tick
    // boundary; pressed at 255/256 it is still open at the contact instant.
    const EventLog late = duel(DashPress{14, 255});
    const EventLog early = duel(DashPress{14, 0});
    const SimEvent& caught = onlyEvent(late, EventKind::BoomerangCaught);
    const SimEvent& hit = onlyEvent(early, EventKind::PlayerHit);
    CHECK(caught.a == static_cast<uint32_t>(CatchHow::Perfect));
    CHECK(countEvents(late, EventKind::PlayerHit) == 0);
    CHECK(countEvents(early, EventKind::BoomerangCaught) == 0);
    CHECK(caught.tick == hit.tick);  // same contact, judged differently
    CHECK(caught.tick.minus(onlyEvent(late, EventKind::PlayerDashed).tick) == 4);
  }
  SECTION("opening edge: contact in the press tick itself") {
    // Pressed at sub-tick 0 the window is open at the contact instant; pressed
    // at 255/256 the banana arrived before the press.
    const EventLog onTime = duel(DashPress{23, 0});
    const EventLog tooLate = duel(DashPress{23, 255});
    const SimEvent& caught = onlyEvent(onTime, EventKind::BoomerangCaught);
    const SimEvent& hit = onlyEvent(tooLate, EventKind::PlayerHit);
    CHECK(caught.a == static_cast<uint32_t>(CatchHow::Perfect));
    CHECK(caught.tick == hit.tick);
    CHECK(caught.tick.minus(onlyEvent(onTime, EventKind::PlayerDashed).tick) == 0);
    CHECK(countEvents(tooLate, EventKind::BoomerangCaught) == 0);
  }
}

TEST_CASE("walking over a grounded banana picks it up unless at ammoMax", "[scenario][pipeline]") {
  const ContentDB& content = defaultContent();
  // Ground a banana: player 1 eliminates player 2 at (0, 0) in a 3-player
  // match (so the round goes on), leaving a pickup near the origin.
  WorldState s = makePlayingWorld(43, 0b111);
  placePlayer(s, kP0, at(-6, 6));
  placePlayer(s, kP1, at(6, 0));
  placePlayer(s, PlayerId{2}, at(0, 0));
  InputFrame f = frameFor(s);
  f.inputs[1].aimDir = kAimLeft;
  f.inputs[1].buttons = kThrow;
  EventLog log;
  runTicks(s, f, 1, content, log);
  f.inputs[1].buttons = 0;
  for (int i = 0; i < 60 && s.pickups.empty(); ++i) {
    runTicks(s, f, 1, content, log);
  }
  REQUIRE(countEvents(log, EventKind::PlayerEliminated) == 1);
  REQUIRE(s.pickups.size() == 1);
  REQUIRE(s.rules.phase == MatchPhase::Playing);
  const SimEvent& spawned = onlyEvent(log, EventKind::PickupSpawned);
  const Vec2 lying = s.pickups.items[spawned.entity.index].pos;

  SECTION("at ammoMax the pickup stays") {
    REQUIRE(playerAt(s, kP0).ammo == content.player.ammoMax);
    placePlayer(s, kP0, lying - at(3, 0));
    InputFrame walk = frameFor(s);
    walk.inputs[0] = PlayerInput{kMoveRight, 255, 0, 0, 0};
    EventLog walked;
    runTicks(s, walk, 60, content, walked);  // 7 units: well past the pickup
    CHECK(playerAt(s, kP0).transform.pos.x > lying.x + Fixed::fromInt(2));
    CHECK(countEvents(walked, EventKind::PickupTaken) == 0);
    CHECK(s.pickups.isAlive(spawned.entity));
    CHECK(playerAt(s, kP0).ammo == content.player.ammoMax);
  }
  SECTION("empty-handed, the player takes it") {
    // Player 1 threw: its ammo is 0. Walk it across the pickup along +x.
    REQUIRE(playerAt(s, kP1).ammo == 0);
    placePlayer(s, kP1, lying - at(3, 0));
    InputFrame walk = frameFor(s);
    walk.inputs[1] = PlayerInput{kMoveRight, 255, 0, 0, 0};
    EventLog walked;
    runTicks(s, walk, 60, content, walked);
    const SimEvent& taken = onlyEvent(walked, EventKind::PickupTaken);
    CHECK(taken.player == kP1);
    CHECK(taken.entity == spawned.entity);
    CHECK(s.pickups.empty());
    CHECK(playerAt(s, kP1).ammo == 1);
  }
}
