// The banana economy per mode (architecture §5.3, §5.6, v0.57, PR #49 round
// 6): ModeDef::deathBananas (drop or vanish), the point kinds' catchScore and
// suicideScore, and bananaRegrowTicks. The ammo power-up's expiry (the extra
// bananas vanish) is in banana_conservation_scenario.cpp.
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kRounds = 0;
constexpr uint8_t kDm = 1;         // deathmatch, vanish, catch +1, suicide -1
constexpr uint8_t kTdm = 2;        // the same between two teams
constexpr uint8_t kRegrow = 3;     // deathmatch with a 30-tick regrow
constexpr uint8_t kDryRegrow = 4;  // rounds with a 30-tick regrow
constexpr uint8_t kPits = 1;       // the dojo with a pit east of the centre
constexpr uint16_t kRegrowTicks = 30;
constexpr uint8_t kAimLeft = 128;
constexpr uint8_t kAimRight = 0;
constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};
constexpr PlayerId kP2{2};

Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

ModeDef pointMode(const char* name, ModeKind kind) {
  ModeDef m;
  m.name = ContentName::of(name);
  m.kind = kind;
  m.roundStartTicks = 10;
  m.scoreToWin = 3;
  m.timeLimitTicks = 6000;
  m.respawnTicks = 40;
  m.respawnInvulnTicks = 0;
  m.deathBananas = DeathBananas::Vanish;
  m.catchScore = 1;
  m.suicideScore = -1;
  return m;
}

ContentDB economyContent() {
  ContentDB c = defaultContent();
  c.player.fallTicks = 60;
  c.modes[kDm] = pointMode("dm", ModeKind::Deathmatch);
  c.modes[kTdm] = pointMode("tdm", ModeKind::TeamDeathmatch);
  c.modes[kTdm].teams = 2;
  c.modes[kRegrow] = pointMode("regrow", ModeKind::Deathmatch);
  c.modes[kRegrow].bananaRegrowTicks = kRegrowTicks;
  c.modes[kDryRegrow] = c.modes[kRounds];
  c.modes[kDryRegrow].name = ContentName::of("dry_regrow");
  c.modes[kDryRegrow].bananaRegrowTicks = kRegrowTicks;
  c.modeCount = 5;
  ArenaDef& a = c.arenas[kPits];
  a = c.arenas[0];
  a.name = ContentName::of("pits");
  a.wallCount = 0;
  a.hazards[0] = Hazard{at(6, -3), at(10, 3), HazardKind::Pit, Fixed::one()};
  a.hazardCount = 1;
  c.arenaCount = 2;
  return c;
}

const ContentDB& db() {
  static const ContentDB c = economyContent();
  return c;
}

struct Match {
  MatchConfig config;
  MatchContent content;
  WorldState s;
  EventLog log;

  Match(uint8_t mode, PlayerMask players, uint8_t arena = 0)
      : config{.seed = 0xBA7A,
               .players = players,
               .arena = arena,
               .mode = mode,
               .teams = teamsFor(db().modeAt(mode), players)},
        content(matchContent(db(), config)),
        s(initialWorld(config)) {
    for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i)
      step();
    REQUIRE(s.rules.phase == MatchPhase::Playing);
    // Everyone apart along y = -6, far from the pit and from each other.
    int x = -12;
    for (uint8_t i = 0; i < kMaxPlayers; ++i) {
      if (s.playerBySlot(PlayerId{i}) != nullptr) placePlayer(s, PlayerId{i}, at(x, -6));
      x += 6;
    }
    log.clear();
  }
  Match(const Match&) = delete;
  Match& operator=(const Match&) = delete;

  EventLog step(const InputFrame* frame = nullptr) {
    EventLog out;
    runTicks(s, frame != nullptr ? *frame : frameFor(s), 1, content, out);
    log.insert(log.end(), out.begin(), out.end());
    return out;
  }
  void steps(int n) {
    for (int i = 0; i < n; ++i)
      step();
  }
  // Kills `victim` with a banana of `by` hitting it on the next tick.
  EventLog hit(PlayerId victim, PlayerId by) {
    const Vec2 v = playerAt(s, victim).transform.pos;
    spawnBanana(s, by, v - Vec2{Fixed::one(), Fixed{}}, Vec2{Fixed::fromRatio(1, 4), Fixed{}},
                BoomerangPhase::Outbound, db());
    return step();
  }
  // Drops `p` into the middle of the pit; steps until its fall begins.
  EventLog pushIn(PlayerId p) {
    placePlayer(s, p, at(8, 0));
    return step();
  }
  [[nodiscard]] uint32_t bananaPickups() const {
    uint32_t n = 0;
    s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& k) {
      if (k.kind == PickupKind::Banana) n += k.count;
    });
    return n;
  }
  [[nodiscard]] uint32_t bananasOf(PlayerId owner) const {
    uint32_t n = 0;
    s.boomerangs.forEachAlive([&](EntityId /*id*/, const Boomerang& b) {
      if (b.owner == owner) ++n;
    });
    return n;
  }
};

// A banana far from everyone, owned by `owner`, still on its way out.
EntityId bananaAway(Match& m, PlayerId owner) {
  return spawnBanana(m.s, owner, at(-6, 8), Vec2{Fixed::fromRatio(1, 8), Fixed{}},
                     BoomerangPhase::Outbound, db());
}

// P0 stands at (-4, 0) empty-handed facing east; `thrower` at (4, 0) throws
// west at it (own: P0's own banana comes back at it from there instead), and
// P0 dashes into it as it closes to 2 units (the catch_scenario timing).
void dashCatch(Match& m, PlayerId thrower) {
  placePlayer(m.s, kP0, at(-4, 0)).ammo = 0;
  InputFrame f = frameFor(m.s);
  f.inputs[0].aimDir = kAimRight;
  if (thrower == kP0) {
    spawnBanana(m.s, kP0, at(4, 0), Vec2{-db().boomerang.speed, Fixed{}}, BoomerangPhase::Returning,
                db());
  } else {
    placePlayer(m.s, thrower, at(4, 0)).ammo = 1;
    f.inputs[thrower.value].aimDir = kAimLeft;
    f.inputs[thrower.value].buttons = buttonBit(Button::Throw);
    m.step(&f);
    f.inputs[thrower.value].buttons = 0;
  }
  bool dashed = false;
  for (int t = 0; t < 120; ++t) {
    f.inputs[0].buttons = 0;
    const Player* p0 = m.s.playerBySlot(kP0);
    if (!dashed && p0 != nullptr) {
      m.s.boomerangs.forEachAlive([&](EntityId /*id*/, const Boomerang& b) {
        if (b.transform.pos.x - p0->transform.pos.x <= Fixed::fromInt(2)) dashed = true;
      });
      if (dashed) f.inputs[0].buttons = buttonBit(Button::Dash);
    }
    m.step(&f);
  }
  REQUIRE(dashed);
}

[[nodiscard]] bool perfectCatchBy(const EventLog& log, PlayerId p) {
  return std::ranges::any_of(log, [&](const SimEvent& e) {
    return e.kind == EventKind::BoomerangCaught && e.player == p &&
           e.a == static_cast<uint32_t>(CatchHow::Perfect);
  });
}

[[nodiscard]] uint32_t scoreEvents(const EventLog& log, ScoreReason why) {
  uint32_t n = 0;
  for (const SimEvent& e : log) {
    if (e.kind == EventKind::ScoreChanged && e.b == static_cast<uint32_t>(why)) ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("economy: a death in a vanish mode takes every banana the monkey owns",
          "[scenario][banana][economy]") {
  Match m(kDm, 0b111);
  playerAt(m.s, kP0).ammo = 1;
  bananaAway(m, kP0);
  const EventLog log = m.hit(kP0, kP1);
  REQUIRE(firstEvent(log, EventKind::PlayerEliminated).player == kP0);
  CHECK(countEvents(log, EventKind::BananaVanished) == 1);
  CHECK(countEvents(log, EventKind::BananaDropped) == 0);
  CHECK(m.bananasOf(kP0) == 0);
  CHECK(m.bananaPickups() == 0);
  // The respawn hands out the starting banana.
  m.steps(db().modes[kDm].respawnTicks);
  CHECK(playerAt(m.s, kP0).ammo == db().player.ammoStart);
}

TEST_CASE("economy: a death in a drop mode leaves its bananas lying",
          "[scenario][banana][economy]") {
  Match m(kRounds, 0b111);
  playerAt(m.s, kP0).ammo = 1;
  bananaAway(m, kP0);
  const EventLog log = m.hit(kP0, kP1);
  REQUIRE(firstEvent(log, EventKind::PlayerEliminated).player == kP0);
  CHECK(countEvents(log, EventKind::BananaVanished) == 0);
  CHECK(countEvents(log, EventKind::BananaDropped) == 1);  // the held one
  CHECK(m.bananaPickups() == 2);                           // and the one away, grounded
}

TEST_CASE("economy: a pit fall takes the held banana down; the ones away follow the mode",
          "[scenario][banana][economy][pit]") {
  SECTION("vanish: gone the moment the fall begins") {
    Match m(kDm, 0b111, kPits);
    playerAt(m.s, kP0).ammo = 1;
    bananaAway(m, kP0);
    const EventLog fell = m.pushIn(kP0);
    REQUIRE(countEvents(fell, EventKind::PlayerFell) == 1);
    CHECK(countEvents(fell, EventKind::BananaVanished) == 1);
    CHECK(m.bananasOf(kP0) == 0);
    m.steps(db().player.fallTicks);
    REQUIRE(countEvents(m.log, EventKind::PlayerEliminated) == 1);
    CHECK(countEvents(m.log, EventKind::BananaDropped) == 0);
    CHECK(m.bananaPickups() == 0);
  }
  SECTION("drop: the one away comes down at once, the held one still goes down") {
    Match m(kRounds, 0b111, kPits);
    playerAt(m.s, kP0).ammo = 1;
    bananaAway(m, kP0);
    const EventLog fell = m.pushIn(kP0);
    REQUIRE(countEvents(fell, EventKind::PlayerFell) == 1);
    CHECK(countEvents(fell, EventKind::BananaVanished) == 0);
    CHECK(m.bananaPickups() == 1);
    m.steps(db().player.fallTicks);
    CHECK(countEvents(m.log, EventKind::BananaDropped) == 0);
    CHECK(m.bananaPickups() == 1);
  }
}

TEST_CASE("economy: a suicide costs suicide_score and a score may go below zero",
          "[scenario][economy][score]") {
  Match m(kDm, 0b111, kPits);
  m.pushIn(kP0);
  m.steps(db().player.fallTicks);
  REQUIRE(countEvents(m.log, EventKind::PlayerEliminated) == 1);
  CHECK(m.s.rules.scores[0] == -1);
  REQUIRE(scoreEvents(m.log, ScoreReason::Suicide) == 1);
  const SimEvent& lost = firstEvent(m.log, EventKind::ScoreChanged);
  CHECK(lost.player == kP0);
  CHECK(static_cast<int32_t>(lost.a) == -1);
  // Respawned, it falls again: -2. Nobody else scored.
  m.steps(db().modes[kDm].respawnTicks);
  REQUIRE(m.s.playerBySlot(kP0) != nullptr);
  m.pushIn(kP0);
  m.steps(db().player.fallTicks);
  CHECK(m.s.rules.scores[0] == -2);
  CHECK(m.s.rules.scores[1] == 0);
  CHECK(m.s.rules.phase == MatchPhase::Playing);
}

TEST_CASE("economy: a team's score is its slots' sum, suicides included",
          "[scenario][economy][score][teams]") {
  // Teams over slots 0, 1, 2: {0, 2} and {1}.
  Match m(kTdm, 0b111, kPits);
  REQUIRE(m.content.relation(kP0, kP2) == Relation::Ally);
  m.pushIn(kP0);
  m.steps(db().player.fallTicks);
  m.hit(kP1, kP2);  // an enemy elimination: +1 for P2
  CHECK(m.s.rules.scores[0] == -1);
  CHECK(m.s.rules.scores[2] == 1);
  CHECK(m.content.teamScore(m.s.rules.scores, 0) == 0);
  CHECK(m.content.teamScore(m.s.rules.scores, 1) == 0);
}

TEST_CASE("economy: a perfect catch of an enemy's banana scores catch_score",
          "[scenario][economy][score][catch]") {
  Match m(kDm, 0b11);
  dashCatch(m, kP1);
  REQUIRE(perfectCatchBy(m.log, kP0));
  CHECK(m.s.rules.scores[0] == 1);
  CHECK(scoreEvents(m.log, ScoreReason::Catch) == 1);
}

TEST_CASE("economy: the own banana or an ally's scores nothing when caught",
          "[scenario][economy][score][catch]") {
  SECTION("own") {
    Match m(kDm, 0b11);
    dashCatch(m, kP0);
    CHECK(countEvents(m.log, EventKind::BoomerangCaught) == 1);
    CHECK(countEvents(m.log, EventKind::ScoreChanged) == 0);
    CHECK(m.s.rules.scores[0] == 0);
  }
  SECTION("ally") {
    Match m(kTdm, 0b111);
    REQUIRE(m.content.relation(kP0, kP2) == Relation::Ally);
    dashCatch(m, kP2);
    REQUIRE(perfectCatchBy(m.log, kP0));
    CHECK(countEvents(m.log, EventKind::ScoreChanged) == 0);
    CHECK(m.s.rules.scores[0] == 0);
  }
  SECTION("a mode without a catch score") {
    Match m(kRounds, 0b11);
    dashCatch(m, kP1);
    REQUIRE(perfectCatchBy(m.log, kP0));
    CHECK(countEvents(m.log, EventKind::ScoreChanged) == 0);
  }
}

TEST_CASE("economy: catch points count toward score_to_win", "[scenario][economy][score]") {
  Match m(kDm, 0b11);
  m.s.rules.scores[0] = static_cast<int8_t>(db().modes[kDm].scoreToWin - 1);
  dashCatch(m, kP1);
  REQUIRE(perfectCatchBy(m.log, kP0));
  const SimEvent* ended = findEvent(m.log, EventKind::MatchEnded);
  REQUIRE(ended != nullptr);
  CHECK(ended->player == kP0);
}

TEST_CASE("economy: an empty hand regrows a banana after banana_regrow_ticks",
          "[scenario][banana][economy][regrow]") {
  Match m(kRegrow, 0b11);
  playerAt(m.s, kP0).ammo = 0;
  m.steps(kRegrowTicks - 1);
  CHECK(playerAt(m.s, kP0).ammo == 0);
  CHECK(playerAt(m.s, kP0).regrowTicks == kRegrowTicks - 1);
  const EventLog grew = m.step();
  REQUIRE(countEvents(grew, EventKind::BananaRegrown) == 1);
  CHECK(firstEvent(grew, EventKind::BananaRegrown).player == kP0);
  CHECK(playerAt(m.s, kP0).ammo == 1);
  CHECK(playerAt(m.s, kP0).regrowTicks == 0);
  // P1 held its banana throughout: no clock.
  CHECK(playerAt(m.s, kP1).regrowTicks == 0);
}

TEST_CASE("economy: getting a banana back resets the regrow clock",
          "[scenario][banana][economy][regrow]") {
  Match m(kRegrow, 0b11);
  playerAt(m.s, kP0).ammo = 0;
  m.steps(20);
  REQUIRE(playerAt(m.s, kP0).regrowTicks == 20);
  spawnPickup(m.s, playerAt(m.s, kP0).transform.pos);
  m.step();
  REQUIRE(countEvents(m.log, EventKind::PickupTaken) == 1);
  CHECK(playerAt(m.s, kP0).ammo == 1);
  CHECK(playerAt(m.s, kP0).regrowTicks == 0);
}

TEST_CASE("economy: a banana in flight is still owned, so a throw never doubles up",
          "[scenario][banana][economy][regrow]") {
  Match m(kRegrow, 0b11);
  InputFrame f = frameFor(m.s);
  f.inputs[0].aimDir = kAimRight;
  f.inputs[0].buttons = buttonBit(Button::Throw);
  m.step(&f);
  f.inputs[0].buttons = 0;
  int flying = 0;
  for (int t = 0; t < 600 && countEvents(m.log, EventKind::BoomerangCaught) == 0; ++t) {
    if (m.bananasOf(kP0) > 0) {
      ++flying;
      CHECK(playerAt(m.s, kP0).regrowTicks == 0);
    }
    m.step(&f);
  }
  REQUIRE(countEvents(m.log, EventKind::BoomerangThrown) == 1);
  REQUIRE(countEvents(m.log, EventKind::BoomerangCaught) == 1);  // it came back
  CHECK(flying > kRegrowTicks);                                  // away longer than a regrow
  CHECK(countEvents(m.log, EventKind::BananaRegrown) == 0);
  CHECK(playerAt(m.s, kP0).ammo == 1);
}

TEST_CASE("economy: the regrow clock stops while a monkey falls",
          "[scenario][banana][economy][regrow][pit]") {
  Match m(kRegrow, 0b11, kPits);
  playerAt(m.s, kP0).ammo = 0;
  m.steps(10);
  m.pushIn(kP0);
  const uint16_t before = playerAt(m.s, kP0).regrowTicks;
  m.steps(db().player.fallTicks - 2);
  REQUIRE(playerAt(m.s, kP0).tags.has(Tag::Falling));
  CHECK(playerAt(m.s, kP0).regrowTicks == before);
  CHECK(countEvents(m.log, EventKind::BananaRegrown) == 0);
}

TEST_CASE("economy: a round mode that regrows never runs dry", "[scenario][economy][regrow]") {
  Match m(kDryRegrow, 0b11);
  playerAt(m.s, kP0).ammo = 0;
  playerAt(m.s, kP1).ammo = 0;
  m.steps(5);
  // On the dojo (no pits) a dry round would have ended at once.
  CHECK(countEvents(m.log, EventKind::RoundEnded) == 0);
  CHECK(m.s.rules.dryTicks == 0);
  m.steps(kRegrowTicks);
  CHECK(countEvents(m.log, EventKind::BananaRegrown) == 2);
}
