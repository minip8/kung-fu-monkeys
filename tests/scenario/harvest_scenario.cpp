// Harvest (architecture §5.3, §11 "Contest design: bananas as the economy",
// v0.82): team t's home is zone t. On every Playing tick that is a multiple
// of bankTicks each living monkey in its own home with a banana banks one (a
// point to it); on every one that is a multiple of stealTicks each living
// monkey with room in its hand in an enemy home takes one back out, when that
// team's score is above zero and none of its living monkeys stands there (the
// point comes off its highest-scoring slot). First team to scoreToWin, else
// the leader at the clock, a tied lead a draw; eliminations score nothing; a
// respawn holds respawnAmmo. modeData word 0 carries the two beats. The
// fight's rules the kind leans on (a baseline's ammo_max, the perfect catch,
// a death's drop, the pit, the dash attack, a stack's pickup) are pinned here
// under the kind's numbers, and so is the scripted generator's steering.
// Hand-built content only: no shipped row or arena.
#include "kfm/sim/scripted_inputs.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>
#include <vector>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kHomes = 1;     // the dojo with 1-unit cells, two homes, a tree and a pit
constexpr uint8_t kNoTree = 2;    // the same without the tree
constexpr uint8_t kOneZone = 3;   // the same with the west home alone
constexpr uint8_t kHarvest = 1;   // bank every 4 ticks, steal every 6, first team to 20
constexpr uint8_t kWin = 2;       // the same, first team to 3
constexpr uint8_t kClock = 3;     // the same on a 100-tick clock, first team to 99
constexpr uint8_t kNoSteal = 4;   // steal_ticks 0
constexpr uint8_t kKeepAmmo = 5;  // respawn_ammo absent
constexpr uint8_t kTdm = 6;       // team deathmatch over the same teams (no objective)
constexpr uint16_t kBank = 4;
constexpr uint16_t kSteal = 6;
constexpr uint8_t kHand = 3;  // the baseline's ammo_max: 1 + 2
constexpr uint8_t kAimLeft = 128;
constexpr uint8_t kAimRight = 0;
constexpr PlayerId kP0{0};  // team 0, home zone 0 (west)
constexpr PlayerId kP1{1};  // team 1, home zone 1 (east)
constexpr PlayerId kP2{2};  // team 0
constexpr PlayerId kP3{3};  // team 1

[[nodiscard]] constexpr Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}
constexpr Vec2 kWestHome = at(-13, 0);
constexpr Vec2 kEastHome = at(13, 0);
constexpr Vec2 kPit = at(0, -8);

ContentDB harvestContent() {
  ContentDB c = defaultContent();
  const uint8_t base = addPowerUp(c, "harvest_base", 1,
                                  {statGrant(StatKind::AmmoMax, Fixed::one(), Fixed::fromInt(2))});
  ArenaDef& homes = c.arenas[kHomes];
  homes = c.arenas[0];
  homes.name = ContentName::of("homes");
  homes.cellSize = Fixed::one();
  homes.spawnGroups = 2;
  homes.zones[0] = Zone{at(-15, -2), at(-11, 2)};
  homes.zones[1] = Zone{at(11, -2), at(15, 2)};
  homes.zoneCount = 2;
  homes.hazards[0] = Hazard{at(-2, -9), at(2, -7), HazardKind::Pit, Fixed::one()};
  homes.hazardCount = 1;
  PickupSpawner tree;
  tree.pos = Vec2{Fixed::half(), Fixed::half()};
  tree.firstTicks = 6000;  // not within any test: the tree is there for compatible()
  tree.intervalTicks = 600;
  tree.tree = 1;
  homes.pickupSpawners[0] = tree;
  homes.pickupSpawnerCount = 1;
  c.arenas[kNoTree] = homes;
  c.arenas[kNoTree].name = ContentName::of("no_tree");
  c.arenas[kNoTree].pickupSpawnerCount = 0;
  c.arenas[kOneZone] = homes;
  c.arenas[kOneZone].name = ContentName::of("one_zone");
  c.arenas[kOneZone].zoneCount = 1;
  c.arenaCount = 4;

  ModeDef& m = c.modes[kHarvest];
  m.name = ContentName::of("harvest");
  m.kind = ModeKind::Harvest;
  m.teams = 2;
  m.bankTicks = kBank;
  m.stealTicks = kSteal;
  m.scoreToWin = 20;
  m.timeLimitTicks = 60000;
  m.roundStartTicks = 10;
  m.respawnTicks = 20;
  m.respawnInvulnTicks = 0;
  m.respawnAmmo = 0;
  m.deathBananas = DeathBananas::Drop;
  m.baseline = base;
  m.arena = kHomes;
  c.modes[kWin] = m;
  c.modes[kWin].name = ContentName::of("harvest_win");
  c.modes[kWin].scoreToWin = 3;
  c.modes[kClock] = m;
  c.modes[kClock].name = ContentName::of("harvest_clock");
  c.modes[kClock].timeLimitTicks = 100;
  c.modes[kClock].scoreToWin = 99;
  c.modes[kNoSteal] = m;
  c.modes[kNoSteal].name = ContentName::of("harvest_nosteal");
  c.modes[kNoSteal].stealTicks = 0;
  c.modes[kKeepAmmo] = m;
  c.modes[kKeepAmmo].name = ContentName::of("harvest_keep");
  c.modes[kKeepAmmo].respawnAmmo = kRespawnAmmoStart;
  c.modes[kTdm] = m;
  c.modes[kTdm].name = ContentName::of("tdm");
  c.modes[kTdm].kind = ModeKind::TeamDeathmatch;
  c.modeCount = 7;
  return c;
}

const ContentDB& db() {
  static const ContentDB c = harvestContent();
  return c;
}

// A harvest match in Playing with every monkey parked outside both homes.
struct Match {
  MatchConfig config;
  MatchContent content;
  WorldState s;
  EventLog log;

  Match(uint8_t mode, PlayerMask players, const TeamSet& teams)
      : config{.seed = 0x4A27, .players = players, .arena = kHomes, .mode = mode, .teams = teams},
        content(matchContent(db(), config)),
        s(initialWorld(config)) {
    for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i)
      step();
    REQUIRE(s.rules.phase == MatchPhase::Playing);
    const std::array<Vec2, 4> spots = {at(-8, 6), at(8, 6), at(-8, -4), at(8, -4)};
    for (uint8_t i = 0; i < 4; ++i) {
      if (s.playerBySlot(PlayerId{i}) != nullptr) placePlayer(s, PlayerId{i}, spots[i]);
    }
    log.clear();
  }
  explicit Match(uint8_t mode = kHarvest, PlayerMask players = 0b1111)
      : Match(mode, players, teamsFor(db().modeAt(mode), players)) {}
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
  // Idles until the next step's Playing tick (ticksInPhase after it) is one
  // `beat` says is wanted; nobody is in a home meanwhile unless placed.
  void before(const std::function<bool(uint32_t)>& beat) {
    for (int i = 0; i < 1000 && !beat(s.rules.ticksInPhase + 1); ++i)
      step();
    REQUIRE(beat(s.rules.ticksInPhase + 1));
    log.clear();
  }
  void beforeBankOnly() {
    before([](uint32_t t) { return t % kBank == 0 && t % kSteal != 0; });
  }
  void beforeStealOnly() {
    before([](uint32_t t) { return t % kSteal == 0 && t % kBank != 0; });
  }
  Player& put(PlayerId id, Vec2 pos, uint8_t ammo) {
    Player& p = placePlayer(s, id, pos);
    p.ammo = ammo;
    return p;
  }
  [[nodiscard]] uint32_t bananaPickups() const {
    uint32_t n = 0;
    s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& k) {
      if (k.kind == PickupKind::Banana) n += k.count;
    });
    return n;
  }
  // Kills `victim` with a banana of `by` hitting it on the next tick.
  EventLog hit(PlayerId victim, PlayerId by) {
    const Vec2 v = playerAt(s, victim).transform.pos;
    spawnBanana(s, by, v - Vec2{Fixed::one(), Fixed{}}, Vec2{Fixed::fromRatio(1, 4), Fixed{}},
                BoomerangPhase::Outbound, db());
    return step();
  }
};

[[nodiscard]] std::vector<SimEvent> eventsOf(const EventLog& log, EventKind kind) {
  std::vector<SimEvent> out;
  for (const SimEvent& e : log) {
    if (e.kind == kind) out.push_back(e);
  }
  return out;
}

[[nodiscard]] int16_t teamScore(const Match& m, uint8_t team) {
  return m.content.teamScore(m.s.rules.scores, team);
}

}  // namespace

TEST_CASE("harvest: compatible needs a zone per team and a tree", "[scenario][harvest]") {
  CHECK(db().compatible(kHomes, kHarvest));
  CHECK_FALSE(db().compatible(kNoTree, kHarvest));
  CHECK_FALSE(db().compatible(kOneZone, kHarvest));
  CHECK(db().compatible(kNoTree, kTdm));  // the team deathmatch twin needs neither
  CHECK(scoresPoints(ModeKind::Harvest));
  CHECK(homeZoneOf(db().arenas[kHomes], 0) == 0);
  CHECK(homeZoneOf(db().arenas[kHomes], 1) == 1);
  CHECK(homeZoneOf(db().arenas[kHomes], 2) == kNoZone);
  CHECK(homeZoneOf(db().arenas[kOneZone], 1) == kNoZone);
}

TEST_CASE("harvest: modeData word 0 holds bank_ticks low and steal_ticks high",
          "[scenario][harvest]") {
  const Match m;
  CHECK(m.s.rules.modeData[0] == (uint32_t{kBank} | (uint32_t{kSteal} << 16)));
  for (size_t w = 1; w < m.s.rules.modeData.size(); ++w)
    CHECK(m.s.rules.modeData[w] == 0);
  const Match off(kNoSteal);
  CHECK(off.s.rules.modeData[0] == kBank);
}

TEST_CASE("harvest: the baseline raises the capacity without filling the hand",
          "[scenario][harvest][powerup]") {
  const Match m;
  for (uint8_t i = 0; i < 4; ++i) {
    INFO("slot " << int{i});
    const Player& p = playerAt(m.s, PlayerId{i});
    CHECK(p.stats.ammoMax == kHand);
    CHECK(p.ammo == db().player.ammoStart);  // the opening hand: ammo_start, not 3
  }
}

TEST_CASE("harvest: every living monkey in its own home banks one a bank beat",
          "[scenario][harvest]") {
  Match m;
  m.beforeBankOnly();
  // Two of team 0 at home, and an enemy standing with them (it banks
  // nothing there and stops nothing).
  m.put(kP0, kWestHome, 3);
  m.put(kP2, at(-12, 1), 1);
  m.put(kP1, at(-14, -1), 2);
  m.step();
  CHECK(playerAt(m.s, kP0).ammo == 2);
  CHECK(playerAt(m.s, kP2).ammo == 0);
  CHECK(playerAt(m.s, kP1).ammo == 2);
  CHECK(m.s.rules.scores[0] == 1);
  CHECK(m.s.rules.scores[2] == 1);
  CHECK(m.s.rules.scores[1] == 0);
  const std::vector<SimEvent> scores = eventsOf(m.log, EventKind::ScoreChanged);
  REQUIRE(scores.size() == 2);
  CHECK(scores[0].player == kP0);  // to the banker, in slot order
  CHECK(scores[1].player == kP2);
  for (const SimEvent& e : scores) {
    CHECK(e.b == static_cast<uint32_t>(ScoreReason::Objective));
    CHECK(e.a == 1);
  }
  CHECK(m.bananaPickups() == 0);  // a banked banana leaves the world

  // Off the beat nothing banks.
  REQUIRE((m.s.rules.ticksInPhase + 1) % kBank != 0);
  m.log.clear();
  m.step();
  CHECK(eventsOf(m.log, EventKind::ScoreChanged).empty());
  CHECK(playerAt(m.s, kP0).ammo == 2);

  // The next beat banks one more each: P2 has none left (a steal beat on
  // the way finds the home defended).
  m.before([](uint32_t t) { return t % kBank == 0; });
  m.step();
  CHECK(playerAt(m.s, kP0).ammo == 1);
  CHECK(m.s.rules.scores[0] == 2);
  CHECK(m.s.rules.scores[2] == 1);
  CHECK(teamScore(m, 0) == 3);
}

TEST_CASE("harvest: only a living monkey in its own home banks", "[scenario][harvest]") {
  Match m;
  m.beforeBankOnly();
  m.put(kP0, kEastHome, 3);                         // the enemy's home: no banking there
  m.put(kP2, at(-10, 0), 3);                        // just outside its own
  m.put(kP1, kEastHome, 1).tags.set(Tag::Falling);  // falling: out of play
  playerAt(m.s, kP1).fallTicks = 100;
  m.step();
  CHECK(eventsOf(m.log, EventKind::ScoreChanged).empty());
  CHECK(playerAt(m.s, kP0).ammo == 3);
  CHECK(playerAt(m.s, kP2).ammo == 3);
  CHECK(playerAt(m.s, kP1).ammo == 1);
}

TEST_CASE("harvest: a lone raider with room takes one back out on a steal beat",
          "[scenario][harvest]") {
  Match m;
  m.s.rules.scores[1] = 2;
  m.s.rules.scores[3] = 3;
  m.beforeStealOnly();
  m.put(kP0, kEastHome, 0);
  m.step();
  CHECK(playerAt(m.s, kP0).ammo == 1);
  CHECK(m.s.rules.scores[3] == 2);  // off the highest-scoring slot
  CHECK(m.s.rules.scores[1] == 2);
  CHECK(m.s.rules.scores[0] == 0);  // the raider scores nothing
  const std::vector<SimEvent> scores = eventsOf(m.log, EventKind::ScoreChanged);
  REQUIRE(scores.size() == 1);
  CHECK(scores[0].player == kP3);
  CHECK(static_cast<int32_t>(scores[0].a) == 2);
  CHECK(scores[0].b == static_cast<uint32_t>(ScoreReason::Stolen));
  const SimEvent& stolen = firstEvent(m.log, EventKind::BananaStolen);
  CHECK(stolen.player == kP0);
  CHECK(stolen.a == kP3.value);
  CHECK(stolen.b == 1);  // the zone

  SECTION("a tie for the highest takes it off the lower slot") {
    REQUIRE(m.s.rules.scores[1] == m.s.rules.scores[3]);
    m.before([](uint32_t t) { return t % kSteal == 0; });
    m.step();
    CHECK(m.s.rules.scores[1] == 1);
    CHECK(m.s.rules.scores[3] == 2);
    CHECK(playerAt(m.s, kP0).ammo == 2);
  }
  SECTION("off the beat nothing is taken") {
    REQUIRE((m.s.rules.ticksInPhase + 1) % kSteal != 0);
    m.log.clear();
    m.step();
    CHECK(eventsOf(m.log, EventKind::BananaStolen).empty());
  }
}

TEST_CASE("harvest: several raiders take one each on one beat", "[scenario][harvest]") {
  Match m;
  m.s.rules.scores[1] = 5;
  m.beforeStealOnly();
  m.put(kP0, kEastHome, 0);
  m.put(kP2, at(12, 1), 2);
  m.step();
  CHECK(playerAt(m.s, kP0).ammo == 1);
  CHECK(playerAt(m.s, kP2).ammo == 3);
  CHECK(m.s.rules.scores[1] == 3);
  CHECK(countEvents(m.log, EventKind::BananaStolen) == 2);
}

TEST_CASE("harvest: a living defender stops a theft, a dead or falling one does not",
          "[scenario][harvest]") {
  Match m;
  m.s.rules.scores[1] = 4;
  m.beforeStealOnly();
  m.put(kP0, kEastHome, 0);
  SECTION("a defender in the zone") {
    m.put(kP3, at(14, 1), 3);  // full-handed and standing there: it defends
    m.step();
    CHECK(countEvents(m.log, EventKind::BananaStolen) == 0);
    CHECK(playerAt(m.s, kP0).ammo == 0);
    CHECK(m.s.rules.scores[1] == 4);
  }
  SECTION("a defender just outside the zone") {
    m.put(kP3, at(10, 0), 3);
    m.step();
    CHECK(countEvents(m.log, EventKind::BananaStolen) == 1);
  }
  SECTION("a dead defender") {
    m.put(kP3, at(14, 1), 0);
    m.s.players.despawn(m.s.players.idAt(kP3.value));
    m.s.rules.respawnTicks[kP3.value] = 100;
    m.step();
    CHECK(countEvents(m.log, EventKind::BananaStolen) == 1);
    CHECK(m.s.rules.scores[1] == 3);
  }
  SECTION("a falling defender") {
    Player& p = m.put(kP3, at(14, 1), 0);
    p.tags.set(Tag::Falling);
    p.fallTicks = 100;
    m.step();
    CHECK(countEvents(m.log, EventKind::BananaStolen) == 1);
  }
}

TEST_CASE("harvest: no theft with a full hand, from a team at zero, at home or with steal off",
          "[scenario][harvest]") {
  SECTION("a raider with a full hand") {
    Match m;
    m.s.rules.scores[1] = 4;
    m.beforeStealOnly();
    m.put(kP0, kEastHome, kHand);
    m.step();
    CHECK(countEvents(m.log, EventKind::BananaStolen) == 0);
    CHECK(m.s.rules.scores[1] == 4);
  }
  SECTION("a team at zero, or below it") {
    Match m;
    m.s.rules.scores[1] = 1;
    m.s.rules.scores[3] = -1;  // a suicide: the team's score is 0
    m.beforeStealOnly();
    m.put(kP0, kEastHome, 0);
    m.step();
    CHECK(countEvents(m.log, EventKind::BananaStolen) == 0);
    m.s.rules.scores[3] = -2;
    m.beforeStealOnly();
    m.step();
    CHECK(countEvents(m.log, EventKind::BananaStolen) == 0);
  }
  SECTION("its own home") {
    Match m;
    m.s.rules.scores[0] = 4;
    m.beforeStealOnly();
    m.put(kP0, kWestHome, 0);
    m.step();
    CHECK(countEvents(m.log, EventKind::BananaStolen) == 0);
    CHECK(m.s.rules.scores[0] == 4);
  }
  SECTION("steal_ticks 0: never") {
    Match m(kNoSteal);
    m.s.rules.scores[1] = 4;
    m.put(kP0, kEastHome, 0);
    m.steps(5 * kSteal);
    CHECK(countEvents(m.log, EventKind::BananaStolen) == 0);
    CHECK(m.s.rules.scores[1] == 4);
  }
}

TEST_CASE("harvest: a team with no present slot", "[scenario][harvest]") {
  // Slots 0 and 2 play, both team 0; team 1's slot 1 left with two banked.
  TeamSet teams{};
  for (uint8_t i = 0; i < kMaxPlayers; ++i)
    teams[i] = static_cast<uint8_t>(i % 2);
  teams[2] = 0;
  Match m(kHarvest, 0b0101, teams);
  REQUIRE(m.s.playerBySlot(kP1) == nullptr);
  m.s.rules.scores[1] = 2;
  // Its home has no defender: a raid takes the point off the absent slot.
  m.beforeStealOnly();
  m.put(kP0, kEastHome, 0);
  m.step();
  CHECK(countEvents(m.log, EventKind::BananaStolen) == 1);
  CHECK(m.s.rules.scores[1] == 1);
  CHECK(playerAt(m.s, kP0).ammo == 1);
  // Team 0 banks at home and leads alone: the absent team is not counted.
  m.beforeBankOnly();
  m.put(kP2, kWestHome, 1);
  m.step();
  CHECK(m.s.rules.scores[2] == 1);
  CHECK(m.s.rules.phase == MatchPhase::Playing);
}

TEST_CASE("harvest: the first team to score_to_win wins", "[scenario][harvest]") {
  Match m(kWin);
  m.s.rules.scores[0] = 2;
  m.beforeBankOnly();
  m.put(kP2, kWestHome, 1);
  m.step();
  CHECK(m.s.rules.phase == MatchPhase::MatchEnded);
  CHECK(teamScore(m, 0) == 3);
  const SimEvent& ended = firstEvent(m.log, EventKind::MatchEnded);
  CHECK(ended.player == kP0);  // the winning team's highest-scoring present slot
}

TEST_CASE("harvest: at the clock the leader wins, a tied lead is a draw", "[scenario][harvest]") {
  SECTION("a leader") {
    Match m(kClock);
    m.s.rules.scores[3] = 1;
    m.steps(200);
    REQUIRE(m.s.rules.phase == MatchPhase::MatchEnded);
    CHECK(firstEvent(m.log, EventKind::MatchEnded).player == kP3);
  }
  SECTION("a draw") {
    Match m(kClock);
    m.s.rules.scores[0] = 2;
    m.s.rules.scores[1] = 1;
    m.s.rules.scores[3] = 1;
    m.steps(200);
    REQUIRE(m.s.rules.phase == MatchPhase::MatchEnded);
    CHECK_FALSE(firstEvent(m.log, EventKind::MatchEnded).player.isValid());
  }
}

TEST_CASE("harvest: an elimination scores nothing; the respawn holds respawn_ammo",
          "[scenario][harvest]") {
  SECTION("respawn_ammo 0: empty-handed") {
    Match m;
    m.put(kP1, at(8, 6), 2);
    const EventLog log = m.hit(kP1, kP0);
    REQUIRE(firstEvent(log, EventKind::PlayerEliminated).player == kP1);
    CHECK(countEvents(m.log, EventKind::ScoreChanged) == 0);
    CHECK(m.s.rules.deaths[1] == 1);
    m.steps(db().modes[kHarvest].respawnTicks);
    REQUIRE(countEvents(m.log, EventKind::PlayerRespawned) == 1);
    const Player& p = playerAt(m.s, kP1);
    CHECK(p.ammo == 0);
    CHECK(p.stats.ammoMax == kHand);  // the baseline still raises the capacity
    CHECK(countEvents(m.log, EventKind::ScoreChanged) == 0);
  }
  SECTION("respawn_ammo absent: [player] ammo_start, as every mode before") {
    Match m(kKeepAmmo);
    m.put(kP1, at(8, 6), 2);
    m.hit(kP1, kP0);
    m.steps(db().modes[kKeepAmmo].respawnTicks);
    REQUIRE(countEvents(m.log, EventKind::PlayerRespawned) == 1);
    CHECK(playerAt(m.s, kP1).ammo == db().player.ammoStart);
  }
}

TEST_CASE("harvest: a death drops the cargo, a pit takes it", "[scenario][harvest][banana]") {
  SECTION("a death in a drop mode drops what the monkey held") {
    Match m;
    m.put(kP1, at(8, 6), 3);
    const EventLog log = m.hit(kP1, kP0);
    REQUIRE(countEvents(log, EventKind::PlayerEliminated) == 1);
    CHECK(countEvents(log, EventKind::BananaDropped) == 3);
    CHECK(m.bananaPickups() == 3);  // the banana that hit it flies on
  }
  SECTION("a pit takes the cargo down with the monkey") {
    Match m;
    m.put(kP1, kPit, 3);
    m.step();
    REQUIRE(countEvents(m.log, EventKind::PlayerFell) == 1);
    m.steps(db().player.fallTicks + 1);
    REQUIRE(countEvents(m.log, EventKind::PlayerEliminated) == 1);
    CHECK(countEvents(m.log, EventKind::BananaDropped) == 0);
    CHECK(m.bananaPickups() == 0);
  }
}

TEST_CASE("harvest: a perfect catch puts the enemy's banana in a hand with room",
          "[scenario][harvest][catch]") {
  // P0 at (-4, 0) facing east, P1 at (4, 0) throws west at it, and P0 dashes
  // into the banana as it closes to 2 units (catch_scenario's timing).
  const auto dashCatch = [](Match& m, uint8_t ammo) {
    m.put(kP0, at(-4, 0), ammo);
    m.put(kP1, at(4, 0), 1);
    InputFrame f = frameFor(m.s);
    f.inputs[0].aimDir = kAimRight;
    f.inputs[1].aimDir = kAimLeft;
    f.inputs[1].buttons = buttonBit(Button::Throw);
    m.step(&f);
    f.inputs[1].buttons = 0;
    bool dashed = false;
    for (int t = 0; t < 60; ++t) {
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
  };
  SECTION("room in the hand: the catcher keeps it") {
    Match m;
    dashCatch(m, 0);
    const SimEvent& caught = firstEvent(m.log, EventKind::BoomerangCaught);
    CHECK(caught.player == kP0);
    CHECK(caught.a == static_cast<uint32_t>(CatchHow::Perfect));
    CHECK(playerAt(m.s, kP0).ammo == 1);
    CHECK(playerAt(m.s, kP1).ammo == 0);
    CHECK(countEvents(m.log, EventKind::BananaDropped) == 0);
    CHECK(countEvents(m.log, EventKind::ScoreChanged) == 0);  // a catch scores nothing
  }
  SECTION("a full hand: the catch declines and the banana hits (v0.16, rules/catch.cpp)") {
    // The design (v0.81) reads "past ammoMax it drops at its feet"; the sim
    // as built has a full-handed dasher decline the perfect catch, so the
    // banana falls through to the damage row and its cargo drops at death.
    Match m;
    dashCatch(m, kHand);
    CHECK(findEvent(m.log, EventKind::BoomerangCaught) == nullptr);
    CHECK(firstEvent(m.log, EventKind::PlayerHit).player == kP0);
    CHECK(firstEvent(m.log, EventKind::PlayerEliminated).player == kP0);
    CHECK(countEvents(m.log, EventKind::BananaDropped) == kHand);
  }
}

TEST_CASE("harvest: a dash attack needs a banana in hand", "[scenario][harvest][dash_attack]") {
  const auto duel = [](Match& m, uint8_t ammo) {
    m.put(kP0, at(-2, 6), ammo);
    m.put(kP1, at(2, 6), 1);
    for (int k = 0; k < 30; ++k) {
      InputFrame f = frameFor(m.s);
      f.inputs[0].aimDir = kAimRight;
      f.inputs[1].aimDir = kAimLeft;
      if (k == 0) f.inputs[0].buttons = buttonBit(Button::Dash);
      m.step(&f);
    }
    REQUIRE(countEvents(m.log, EventKind::PlayerDashed) == 1);
  };
  SECTION("empty-handed (it banked its last): it passes through") {
    Match m;
    duel(m, 0);
    CHECK(countEvents(m.log, EventKind::PlayerDashHit) == 0);
    CHECK(m.s.playerBySlot(kP1) != nullptr);
  }
  SECTION("a banana in hand: it strikes") {
    Match m;
    duel(m, 1);
    CHECK(firstEvent(m.log, EventKind::PlayerDashHit).player == kP1);
  }
}

TEST_CASE("harvest: a stack is taken as far as the hand holds", "[scenario][harvest][banana]") {
  Match m;
  m.put(kP0, at(-6, 6), 2);
  const EntityId stack = spawnPickup(m.s, at(-6, 6));
  m.s.pickups.items[stack.index].count = 3;
  m.step();
  CHECK(playerAt(m.s, kP0).ammo == kHand);
  const Pickup* left = m.s.pickups.get(stack);
  REQUIRE(left != nullptr);
  CHECK(left->count == 2);
}

TEST_CASE("harvest: the scripted generator gathers, then carries home, Rng-free",
          "[scenario][harvest][scripted]") {
  Match m;
  const ScriptedInputConfig config{};
  // The same world under team deathmatch: every Rng draw must be the same.
  const MatchContent tdm = matchContent(db(), MatchConfig{.seed = m.config.seed,
                                                          .players = m.config.players,
                                                          .arena = kHomes,
                                                          .mode = kTdm,
                                                          .teams = m.config.teams});
  const auto input = [&](PlayerId id) {
    Rng a = Rng::seeded(99);
    Rng b = Rng::seeded(99);
    const PlayerInput in = scriptedInput(a, m.s, m.content, id, config);
    (void)scriptedInput(b, m.s, tdm, id, config);
    CHECK(a.s == b.s);
    return in;
  };
  constexpr uint8_t kEast = 0;
  constexpr uint8_t kSouth = 192;
  constexpr uint8_t kWest = 128;
  SECTION("an empty hand goes for the nearest fruit, however far") {
    m.put(kP0, at(-13, 6), 0);
    spawnPickup(m.s, at(5, 6));
    spawnPickup(m.s, at(14, 6));
    const PlayerInput in = input(kP0);
    CHECK(in.moveMag == 255);
    CHECK(in.moveDir == kEast);
  }
  SECTION("a part-full hand too, even from home") {
    m.put(kP0, kWestHome, 2);
    spawnPickup(m.s, at(-16 + 1, 0));
    CHECK(input(kP0).moveDir == kWest);
  }
  SECTION("a full hand goes home") {
    m.put(kP0, at(-13, 6), kHand);
    spawnPickup(m.s, at(-11, 6));
    const PlayerInput in = input(kP0);
    CHECK(in.moveMag == 255);
    CHECK(in.moveDir == kSouth);
  }
  SECTION("nothing left to pick: home, and it stays there") {
    m.put(kP0, at(-13, 6), 1);
    CHECK(input(kP0).moveDir == kSouth);
    m.put(kP0, kWestHome, 1);
    CHECK(input(kP0).moveMag == 0);
  }
}
