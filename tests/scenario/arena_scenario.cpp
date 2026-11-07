// Arena statics end to end (architecture §5.10, v0.22): scripted inputs
// through step() on an arena with an interior wall, a pit and a slow patch.
// A dash stops at a wall, a banana bounces off one and still comes home, a
// throw into a wall at point blank bounces instead of spawning inside it, a
// monkey walking into a pit is eliminated by nobody (v0.57: after its fall,
// tests/scenario/pit_scenario.cpp) while a banana flies over it, and the slow patch halves move
// speed through the derived stats.
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kDash = buttonBit(Button::Dash);
constexpr uint8_t kThrow = buttonBit(Button::Throw);
const PlayerId kP0{0};
const PlayerId kP1{1};

constexpr uint8_t kRight = 0;  // input angles: 0 = +x, 64 = +y, 128 = -x
constexpr uint8_t kUp = 64;
constexpr uint8_t kLeft = 128;

Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

// The default content plus arena 1, "lab": the dojo's box with a 1 × 6 block
// whose west face is x = 0, a pit strip at x in [-14, -12] and a slow patch
// around (7, 0).
ContentDB labContent() {
  ContentDB c = defaultContent();
  ArenaDef& a = c.arenas[1];
  a = c.arenas[0];
  a.name = ContentName::of("lab");
  a.spawnPoints[0] = at(-10, 0);
  a.spawnPoints[1] = at(10, 0);
  a.spawnPointCount = 2;
  a.walls[0] = Wall{at(0, -3), at(1, 3)};
  a.wallCount = 1;
  a.hazards[0] = Hazard{at(-14, -8), at(-12, 8), HazardKind::Pit, Fixed::one()};
  a.hazards[1] = Hazard{at(5, -3), at(9, 3), HazardKind::Slow, Fixed::half()};
  a.hazardCount = 2;
  c.arenaCount = 2;
  // Mode 1: a short deathmatch (v0.23) for the pit's mode bookkeeping.
  ModeDef& dm = c.modes[1];
  dm.name = ContentName::of("deathmatch");
  dm.kind = ModeKind::Deathmatch;
  dm.roundStartTicks = 10;
  dm.scoreToWin = 3;
  dm.respawnTicks = 40;
  dm.respawnInvulnTicks = 25;
  c.modeCount = 2;
  return c;
}

struct Lab {
  ContentDB db = labContent();
  MatchConfig config;
  MatchContent content;
  WorldState s;

  explicit Lab(PlayerMask players, uint8_t mode = 0)
      : config{.seed = 0x1AB, .players = players, .arena = 1, .mode = mode},
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

  EventLog step(PlayerInput p0, PlayerInput p1 = {}) {
    InputFrame f = frameFor(s);
    f.inputs[0] = p0;
    f.inputs[1] = p1;
    EventLog log;
    runTicks(s, f, 1, content, log);
    return log;
  }
};

}  // namespace

TEST_CASE("a dash stops at a wall and its phases run on", "[scenario][arena]") {
  Lab lab(0b1);
  const AbilityDef& dash = lab.db.abilityForSlot(kAbilitySlotDash);
  placePlayer(lab.s, kP0, at(-2, 0));
  // Press Dash with the stick +x, then let go: 4.4 units of dash toward a
  // face 1.5 units away.
  const EventLog log = lab.step(PlayerInput{kRight, 255, kRight, kDash, 0});
  REQUIRE(findEvent(log, EventKind::PlayerDashed) != nullptr);
  int dashingTicks = 0;
  int perfectTicks = 0;
  for (int k = 0; k < dash.activeTicks + dash.recoveryTicks + 5; ++k) {
    const Player& p = playerAt(lab.s, kP0);
    dashingTicks += p.tags.has(Tag::Dashing) ? 1 : 0;
    perfectTicks += p.tags.has(Tag::PerfectDash) ? 1 : 0;
    CHECK(p.transform.pos.x <= -lab.db.player.radius);
    lab.step(PlayerInput{});
  }
  const Player& p = playerAt(lab.s, kP0);
  // Flush against the west face, not through it; y untouched.
  CHECK(p.transform.pos == Vec2{-lab.db.player.radius, Fixed::zero()});
  // The wall cut the displacement short but not the ability.
  CHECK(dashingTicks == dash.activeTicks);
  CHECK(perfectTicks >= dash.perfectWindowTicks);
}

TEST_CASE("a banana bounces off an interior wall and still returns", "[scenario][arena]") {
  Lab lab(0b1);
  placePlayer(lab.s, kP0, at(-4, 0));
  EventLog log = lab.step(PlayerInput{0, 0, kRight, kThrow, 0});
  bool bounced = false;
  bool flewBack = false;
  bool caught = false;
  for (int t = 0; t < 400 && !caught; ++t) {
    log = lab.step(PlayerInput{});
    if (const SimEvent* e = findEvent(log, EventKind::BoomerangBounced)) {
      const Boomerang* b = lab.s.boomerangs.get(e->entity);
      REQUIRE(b != nullptr);
      CHECK(b->phase == BoomerangPhase::Outbound);
      CHECK(b->transform.pos.x < Fixed::zero());  // still west of the block
      bounced = true;
    }
    lab.s.boomerangs.forEachAlive([&](EntityId /*id*/, const Boomerang& b) {
      if (b.phase == BoomerangPhase::Outbound && b.transform.vel.x < Fixed::zero()) flewBack = true;
      CHECK(b.transform.pos.x < Fixed::zero());  // never inside or behind the block
    });
    CHECK(countEvents(log, EventKind::BoomerangGrounded) == 0);
    if (const SimEvent* e = findEvent(log, EventKind::BoomerangCaught)) {
      CHECK(e->player == kP0);
      CHECK(e->a == static_cast<uint32_t>(CatchHow::Auto));
      caught = true;
    }
  }
  CHECK(bounced);
  CHECK(flewBack);
  CHECK(caught);
  CHECK(playerAt(lab.s, kP0).ammo == lab.db.player.ammoMax);
}

TEST_CASE("a throw into a wall at point blank bounces off it", "[scenario][arena]") {
  Lab lab(0b1);
  placePlayer(lab.s, kP0, Vec2{-lab.db.player.radius, Fixed::zero()});  // touching the face
  EventLog log;
  for (int t = 0; t < 4 && findEvent(log, EventKind::BoomerangThrown) == nullptr; ++t) {
    log = lab.step(PlayerInput{0, 0, kRight, t == 0 ? kThrow : uint8_t{0}, 0});
  }
  const SimEvent& thrown = firstEvent(log, EventKind::BoomerangThrown);
  CHECK(findEvent(log, EventKind::BoomerangBounced) != nullptr);
  const Boomerang* b = lab.s.boomerangs.get(thrown.entity);
  REQUIRE(b != nullptr);
  CHECK(b->transform.pos.x <= -lab.db.boomerang.radius);
  CHECK(b->transform.vel.x < Fixed::zero());
}

TEST_CASE("walking into a pit eliminates by nobody and ends the round", "[scenario][arena]") {
  Lab lab(0b11);
  placePlayer(lab.s, kP0, at(-11, 0));
  placePlayer(lab.s, kP1, at(10, 0));
  EventLog all;
  for (int t = 0; t < 120 && lab.s.rules.phase == MatchPhase::Playing; ++t) {
    const Fixed before = playerAt(lab.s, kP0).transform.pos.x;
    const EventLog log = lab.step(PlayerInput{kLeft, 255, kLeft, 0, 0});
    all.insert(all.end(), log.begin(), log.end());
    if (findEvent(log, EventKind::PlayerFell) != nullptr) {
      // The tick it went in (v0.57: the fall starts), its whole body came
      // over the pit (the centre a radius past the edge x = -12).
      const Fixed goesAt = Fixed::fromInt(-12) - lab.db.player.radius;
      CHECK(before > goesAt);
      CHECK(playerAt(lab.s, kP0).transform.pos.x <= goesAt);
    }
  }
  CHECK(firstEvent(all, EventKind::PlayerFell).player == kP0);
  const SimEvent& out = firstEvent(all, EventKind::PlayerEliminated);
  CHECK(out.player == kP0);
  CHECK(out.a == PlayerId::invalid().value);  // killer 0xFF
  CHECK(countEvents(all, EventKind::PlayerHit) == 0);
  CHECK(lab.s.playerBySlot(kP0) == nullptr);
  CHECK(firstEvent(all, EventKind::RoundEnded).player == kP1);
}

TEST_CASE("a deathmatch pit fall counts a death, scores nobody and respawns",
          "[scenario][arena][mode]") {
  Lab lab(0b11, 1);
  REQUIRE(lab.content.mode.kind == ModeKind::Deathmatch);
  placePlayer(lab.s, kP0, at(-11, 0));
  placePlayer(lab.s, kP1, at(10, 0));
  EventLog all;
  int fellAt = -1;
  for (int t = 0; t < 120 && fellAt < 0; ++t) {
    const EventLog log = lab.step(PlayerInput{kLeft, 255, kLeft, 0, 0});
    all.insert(all.end(), log.begin(), log.end());
    if (findEvent(log, EventKind::PlayerEliminated) != nullptr) fellAt = t;
  }
  REQUIRE(fellAt >= 0);
  CHECK(firstEvent(all, EventKind::PlayerEliminated).a == PlayerId::invalid().value);
  CHECK(lab.s.rules.deaths[0] == 1);
  CHECK(lab.s.rules.deaths[1] == 0);
  CHECK(lab.s.rules.scores[0] == 0);
  CHECK(lab.s.rules.scores[1] == 0);
  CHECK(countEvents(all, EventKind::ScoreChanged) == 0);
  CHECK(lab.s.playerBySlot(kP0) == nullptr);
  CHECK(lab.s.rules.phase == MatchPhase::Playing);  // a deathmatch plays on

  // Idle until the respawn: it comes back at a spawn point, clear of the pit.
  const SimEvent* respawned = nullptr;
  for (int t = 0; t < lab.content.mode.respawnTicks + 5 && respawned == nullptr; ++t) {
    all = lab.step(PlayerInput{});
    respawned = findEvent(all, EventKind::PlayerRespawned);
    if (respawned != nullptr) CHECK(respawned->player == kP0);
  }
  REQUIRE(respawned != nullptr);
  const Player& p = playerAt(lab.s, kP0);
  CHECK(p.hp == lab.db.player.maxHp);
  CHECK(p.transform.pos.x > Fixed::fromInt(-12));
  CHECK(lab.s.rules.scores[1] == 0);
}

TEST_CASE("a banana flies over a pit untouched", "[scenario][arena]") {
  Lab lab(0b1);
  placePlayer(lab.s, kP0, at(-10, 0));
  EventLog log;
  for (int t = 0; t < 4 && findEvent(log, EventKind::BoomerangThrown) == nullptr; ++t) {
    log = lab.step(PlayerInput{0, 0, kLeft, t == 0 ? kThrow : uint8_t{0}, 0});
  }
  const EntityId id = firstEvent(log, EventKind::BoomerangThrown).entity;
  const Boomerang* thrown = lab.s.boomerangs.get(id);
  REQUIRE(thrown != nullptr);
  const Vec2 vel0 = thrown->transform.vel;
  int ticksOverPit = 0;
  for (int t = 0; t < 14; ++t) {
    log = lab.step(PlayerInput{});
    const Boomerang* b = lab.s.boomerangs.get(id);
    REQUIRE(b != nullptr);
    CHECK(b->phase == BoomerangPhase::Outbound);
    CHECK(b->transform.vel == vel0);
    CHECK(countEvents(log, EventKind::BoomerangGrounded) == 0);
    const Fixed x = b->transform.pos.x;
    if (x >= Fixed::fromInt(-14) && x <= Fixed::fromInt(-12)) ++ticksOverPit;
  }
  CHECK(ticksOverPit > 3);
  CHECK(lab.s.playerBySlot(kP0) != nullptr);
}

TEST_CASE("a slow patch scales move speed through the derived stats", "[scenario][arena]") {
  Lab lab(0b1);
  const Fixed base = lab.db.player.moveSpeed;
  placePlayer(lab.s, kP0, at(7, 0));
  lab.step(PlayerInput{});  // ModifierSystem derives the stats from the position
  CHECK(playerAt(lab.s, kP0).stats.moveSpeed == base * Fixed::half());
  const Vec2 before = playerAt(lab.s, kP0).transform.pos;
  lab.step(PlayerInput{kUp, 255, kUp, 0, 0});
  const Fixed moved = playerAt(lab.s, kP0).transform.pos.y - before.y;
  CHECK(std::abs(moved.raw - (base * Fixed::half()).raw) <= 4);  // stick quantisation

  placePlayer(lab.s, kP0, at(3, 5));  // outside the patch and the block
  lab.step(PlayerInput{});
  CHECK(playerAt(lab.s, kP0).stats.moveSpeed == base);
}
