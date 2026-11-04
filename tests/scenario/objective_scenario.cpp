// Objectives (architecture §5.3 "Mode seams", §5.10, v0.33): arena zones and
// zoneOccupants(), the hill and capture kinds, and ContentDB::compatible().
#include "kfm/sim/zones.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kZoned = 1;      // the dojo with two spawn groups and three zones
constexpr uint8_t kHill = 1;       // hill: 30 ticks a point, first team to 2
constexpr uint8_t kHillClock = 2;  // hill on a 100-tick clock, first team to 9
constexpr uint16_t kHillTicks = 30;
constexpr uint8_t kBases = 2;    // the dojo with two spawn groups and a base zone per side
constexpr uint8_t kCapture = 3;  // capture: first team to 2, a dropped flag home after 50
constexpr uint16_t kFlagReturnTicks = 50;
constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};
constexpr PlayerId kP2{2};
constexpr PlayerId kP3{3};

[[nodiscard]] constexpr Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

ContentDB objectiveContent() {
  ContentDB c = defaultContent();
  ArenaDef& zoned = c.arenas[kZoned];
  zoned = c.arenas[0];
  zoned.name = ContentName::of("zoned");
  zoned.spawnGroups = 2;
  zoned.zones[0] = Zone{at(-2, -2), at(2, 2)};     // the centre: hill
  zoned.zones[1] = Zone{at(-15, -2), at(-11, 2)};  // west (capture: team 0's is zone 0)
  zoned.zones[2] = Zone{at(11, -2), at(15, 2)};    // east
  zoned.zoneCount = 3;
  ArenaDef& bases = c.arenas[kBases];
  bases = zoned;
  bases.name = ContentName::of("bases");
  bases.zones[0] = zoned.zones[1];  // team 0: west
  bases.zones[1] = zoned.zones[2];  // team 1: east
  bases.zoneCount = 2;
  c.arenaCount = 3;
  ModeDef& hill = c.modes[kHill];
  hill.name = ContentName::of("hill");
  hill.kind = ModeKind::Hill;
  hill.teams = 2;
  hill.hillTicks = kHillTicks;
  hill.scoreToWin = 2;
  hill.timeLimitTicks = 6000;
  hill.roundStartTicks = 10;
  hill.respawnTicks = 20;
  hill.respawnInvulnTicks = 0;
  c.modes[kHillClock] = hill;
  c.modes[kHillClock].name = ContentName::of("hill_clock");
  c.modes[kHillClock].scoreToWin = 9;
  c.modes[kHillClock].timeLimitTicks = 100;
  ModeDef& capture = c.modes[kCapture];
  capture = hill;
  capture.name = ContentName::of("capture");
  capture.kind = ModeKind::Capture;
  capture.flagReturnTicks = kFlagReturnTicks;
  c.modeCount = 4;
  return c;
}

const ContentDB& content() {
  static const ContentDB c = objectiveContent();
  return c;
}

MatchConfig objectiveConfig(uint8_t mode, PlayerMask players = 0b1111, uint64_t seed = 0x0B1E,
                            uint8_t arena = kZoned) {
  return MatchConfig{.seed = seed,
                     .players = players,
                     .arena = arena,
                     .mode = mode,
                     .teams = teamsFor(content().modeAt(mode), players)};
}

WorldState playing(const MatchConfig& config, EventLog& log) {
  WorldState s = initialWorld(config);
  const MatchContent m = matchContent(content(), config);
  for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i) {
    runTicks(s, frameFor(s), 1, m, log);
  }
  REQUIRE(s.rules.phase == MatchPhase::Playing);
  return s;
}

// Every monkey on its own spot outside every zone.
void parkEveryone(WorldState& s) {
  const std::array<Vec2, 4> spots = {at(-8, 6), at(8, 6), at(-8, -6), at(8, -6)};
  for (uint8_t i = 0; i < 4; ++i) {
    if (s.playerBySlot(PlayerId{i}) != nullptr) placePlayer(s, PlayerId{i}, spots[i]);
  }
}

// The ScoreChanged events in the log from `from` on.
std::vector<SimEvent> scoresSince(const EventLog& log, size_t from) {
  std::vector<SimEvent> out;
  for (size_t i = from; i < log.size(); ++i) {
    if (log[i].kind == EventKind::ScoreChanged) out.push_back(log[i]);
  }
  return out;
}

// Team t's flag pickup, if one lies in the arena.
const Pickup* flagOf(const WorldState& s, uint8_t team) {
  const Pickup* found = nullptr;
  s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& k) {
    if (k.kind == PickupKind::Flag && k.spawner == team) found = &k;
  });
  return found;
}

// Kills `victim` with a banana of `owner` fired from one unit to its left.
void eliminate(WorldState& s, const MatchContent& m, PlayerId owner, PlayerId victim,
               EventLog& log) {
  const Vec2 pos = playerAt(s, victim).transform.pos;
  spawnBanana(s, owner, pos - Vec2{Fixed::one(), Fixed::zero()},
              Vec2{Fixed::fromRatio(1, 2), Fixed::zero()}, BoomerangPhase::Outbound, m.db);
  const size_t before = log.size();
  for (int t = 0; t < 10; ++t) {
    runTicks(s, frameFor(s), 1, m, log);
    for (size_t i = before; i < log.size(); ++i) {
      if (log[i].kind == EventKind::PlayerEliminated && log[i].player == victim) return;
    }
  }
  FAIL("the banana missed");
}

}  // namespace

TEST_CASE("zones: occupants are the live monkeys whose centre is inside", "[scenario][zones]") {
  const MatchContent m(content(), kZoned, 0);
  WorldState s = makePlayingWorld(0x20E5, 0b1111, content());
  placePlayer(s, kP0, at(0, 0));
  placePlayer(s, kP1, at(2, 2));  // on the corner: inside (closed)
  placePlayer(s, kP2, Vec2{Fixed::fromInt(2) + Fixed::epsilon(), Fixed::zero()});  // just out
  placePlayer(s, kP3, at(-13, 0));
  const ArenaDef& arena = m.arena;
  CHECK(zoneOccupants(s, arena, 0) == 0b0011);
  CHECK(zoneOccupants(s, arena, 1) == 0b1000);
  CHECK(zoneOccupants(s, arena, 2) == 0);
  CHECK(zoneOccupants(s, arena, 3) == 0);  // no such zone
  CHECK(zoneOccupants(s, arena, kMaxArenaZones) == 0);
  // An eliminated monkey still in the pool counts for nobody.
  playerAt(s, kP1).tags.set(Tag::Eliminated);
  CHECK(zoneOccupants(s, arena, 0) == 0b0001);
  // Zones block nothing: a monkey walks straight through one.
  EventLog log;
  playerAt(s, kP1).tags.clear(Tag::Eliminated);
  placePlayer(s, kP0, at(-4, 0));
  InputFrame f = frameFor(s);
  f.inputs[0].moveDir = 0;  // +x
  f.inputs[0].moveMag = 255;
  runTicks(s, f, 90, m, log);
  CHECK(playerAt(s, kP0).transform.pos.x > Fixed::fromInt(2));
}

TEST_CASE("hill: a sole holder scores every hillTicks", "[scenario][hill]") {
  const MatchConfig config = objectiveConfig(kHill);
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  parkEveryone(s);
  log.clear();
  placePlayer(s, kP2, at(1, 0));  // team 0 (slots 0 and 2), alone on the hill
  runTicks(s, frameFor(s), kHillTicks - 1, m, log);
  CHECK(scoresSince(log, 0).empty());
  const SimEvent& holder = firstEvent(log, EventKind::HillHolderChanged);
  CHECK(holder.a == 0);
  CHECK(holder.b == 0);
  CHECK(countEvents(log, EventKind::HillHolderChanged) == 1);
  runTicks(s, frameFor(s), 1, m, log);
  std::vector<SimEvent> scores = scoresSince(log, 0);
  REQUIRE(scores.size() == 1);
  CHECK(scores[0].player == kP2);
  CHECK(scores[0].a == 1);
  // Two of a team on the hill: the lowest slot in the zone gets the point.
  placePlayer(s, kP0, at(-1, 0));
  const size_t before = log.size();
  runTicks(s, frameFor(s), kHillTicks, m, log);
  scores = scoresSince(log, before);
  REQUIRE(scores.size() == 1);
  CHECK(scores[0].player == kP0);
  // Two points: team 0 takes the match, named by its top scorer (the lowest on a tie).
  CHECK(s.rules.phase == MatchPhase::MatchEnded);
  CHECK(firstEvent(log, EventKind::MatchEnded).player == kP0);
  CHECK(m.teamScore(s.rules.scores, 0) == 2);
  CHECK(m.teamScore(s.rules.scores, 1) == 0);
}

TEST_CASE("hill: a contested or empty zone holds every clock", "[scenario][hill]") {
  const MatchConfig config = objectiveConfig(kHill);
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  parkEveryone(s);
  log.clear();
  placePlayer(s, kP0, at(0, 0));
  runTicks(s, frameFor(s), 20, m, log);  // team 0's clock at 20
  placePlayer(s, kP1, at(1, 1));         // team 1 contests
  runTicks(s, frameFor(s), 200, m, log);
  CHECK(scoresSince(log, 0).empty());
  int changes = 0;
  for (const SimEvent& e : log) {
    if (e.kind != EventKind::HillHolderChanged) continue;
    ++changes;
    if (changes == 2) CHECK(e.a == 0xFF);
  }
  CHECK(changes == 2);
  placePlayer(s, kP1, at(8, 6));  // team 1 leaves: team 0 resumes at 20
  runTicks(s, frameFor(s), 9, m, log);
  CHECK(scoresSince(log, 0).empty());
  runTicks(s, frameFor(s), 1, m, log);
  REQUIRE(scoresSince(log, 0).size() == 1);
  CHECK(scoresSince(log, 0)[0].player == kP0);
  // Team 1 holds alone for 15, then the zone is empty for a while: its
  // clock waits at 15 and only team 0's restarted.
  placePlayer(s, kP0, at(-8, 6));
  placePlayer(s, kP3, at(0, 0));
  runTicks(s, frameFor(s), 15, m, log);
  placePlayer(s, kP3, at(8, -6));
  runTicks(s, frameFor(s), 300, m, log);
  placePlayer(s, kP3, at(0, 0));
  const size_t before = log.size();
  runTicks(s, frameFor(s), kHillTicks - 15, m, log);
  const std::vector<SimEvent> scores = scoresSince(log, before);
  REQUIRE(scores.size() == 1);
  CHECK(scores[0].player == kP3);
  CHECK(s.rules.phase == MatchPhase::Playing);  // one point each
}

TEST_CASE("hill: eliminations score nothing, the eliminated respawn", "[scenario][hill]") {
  const MatchConfig config = objectiveConfig(kHill);
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  parkEveryone(s);
  log.clear();
  const Vec2 victim = playerAt(s, kP1).transform.pos;
  spawnBanana(s, kP0, victim - Vec2{Fixed::one(), Fixed::zero()},
              Vec2{Fixed::fromRatio(1, 2), Fixed::zero()}, BoomerangPhase::Outbound, m.db);
  runTicks(s, frameFor(s), 5, m, log);
  CHECK(firstEvent(log, EventKind::PlayerEliminated).player == kP1);
  CHECK(scoresSince(log, 0).empty());
  CHECK(s.rules.deaths[kP1.value] == 1);
  runTicks(s, frameFor(s), 20, m, log);
  CHECK(countEvents(log, EventKind::PlayerRespawned) == 1);
  CHECK(s.playerBySlot(kP1) != nullptr);
}

TEST_CASE("hill: the clock ends the match with the leading team, a tie is a draw",
          "[scenario][hill]") {
  const MatchConfig config = objectiveConfig(kHillClock);
  const MatchContent m = matchContent(content(), config);
  {
    EventLog log;
    WorldState s = playing(config, log);
    parkEveryone(s);
    placePlayer(s, kP1, at(0, 0));
    runTicks(s, frameFor(s), 100, m, log);
    CHECK(s.rules.phase == MatchPhase::MatchEnded);
    CHECK(firstEvent(log, EventKind::MatchEnded).player == kP1);
  }
  {
    EventLog log;
    WorldState s = playing(config, log);
    parkEveryone(s);
    runTicks(s, frameFor(s), 100, m, log);
    CHECK(s.rules.phase == MatchPhase::MatchEnded);
    CHECK_FALSE(firstEvent(log, EventKind::MatchEnded).player.isValid());
  }
}

TEST_CASE("compatible: objective modes need their zones", "[scenario][zones][hill]") {
  const ContentDB& c = content();
  CHECK_FALSE(c.compatible(0, kHill));  // the dojo: no zone, one spawn group
  CHECK(c.compatible(kZoned, kHill));
  CHECK(c.compatible(0, 0));
  CHECK(c.firstCompatibleArena(kHill) == std::optional<uint8_t>{kZoned});
  ContentDB oneGroup = c;
  oneGroup.arenas[kZoned].spawnGroups = 1;
  CHECK_FALSE(oneGroup.compatible(kZoned, kHill));
  ContentDB noZone = c;
  noZone.arenas[kZoned].zoneCount = 0;
  noZone.arenas[kBases].zoneCount = 0;
  CHECK_FALSE(noZone.compatible(kZoned, kHill));
  CHECK_FALSE(noZone.firstCompatibleArena(kHill).has_value());
}

TEST_CASE("capture: take the enemy flag, carry it home, score", "[scenario][capture]") {
  const MatchConfig config = objectiveConfig(kCapture, 0b1111, 0x0B1E, kBases);
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  // The round start laid one flag per team at the centre of its zone.
  const Pickup* home0 = flagOf(s, 0);
  const Pickup* home1 = flagOf(s, 1);
  REQUIRE(home0 != nullptr);
  REQUIRE(home1 != nullptr);
  CHECK(home0->pos == at(-13, 0));
  CHECK(home1->pos == at(13, 0));
  CHECK(countEvents(log, EventKind::PickupSpawned) == 2);
  parkEveryone(s);
  log.clear();
  // Its own team cannot take a flag.
  placePlayer(s, kP0, at(-13, 0));
  runTicks(s, frameFor(s), 1, m, log);
  CHECK(flagOf(s, 0) != nullptr);
  CHECK_FALSE(playerAt(s, kP0).tags.has(Tag::Carrying));
  placePlayer(s, kP0, at(-8, 6));
  // An enemy takes it and carries it.
  placePlayer(s, kP1, at(-13, 0));
  runTicks(s, frameFor(s), 1, m, log);
  CHECK(flagOf(s, 0) == nullptr);
  CHECK(playerAt(s, kP1).tags.has(Tag::Carrying));
  const SimEvent& taken = firstEvent(log, EventKind::PickupTaken);
  CHECK(taken.player == kP1);
  CHECK(taken.a == static_cast<uint32_t>(PickupKind::Flag));
  CHECK(taken.b == 0);
  // A carrier takes no second flag (a stray team-0 flag stays put).
  const EntityId stray = s.pickups.spawn();
  REQUIRE(stray.isValid());
  s.pickups.items[stray.index] = Pickup{PickupKind::Flag, at(-8, 0), 0, 0};
  placePlayer(s, kP1, at(-8, 0));
  runTicks(s, frameFor(s), 1, m, log);
  CHECK(s.pickups.get(stray) != nullptr);
  s.pickups.despawn(stray);
  // Into its own zone (east) while its own flag is home: a capture.
  CHECK(scoresSince(log, 0).empty());
  placePlayer(s, kP1, at(12, 1));
  runTicks(s, frameFor(s), 1, m, log);
  const std::vector<SimEvent> scores = scoresSince(log, 0);
  REQUIRE(scores.size() == 1);
  CHECK(scores[0].player == kP1);
  CHECK(scores[0].a == 1);
  CHECK_FALSE(playerAt(s, kP1).tags.has(Tag::Carrying));
  REQUIRE(flagOf(s, 0) != nullptr);
  CHECK(flagOf(s, 0)->pos == at(-13, 0));  // back home
  // A second capture takes the match.
  placePlayer(s, kP3, at(-13, 0));
  runTicks(s, frameFor(s), 1, m, log);
  placePlayer(s, kP3, at(13, -1));
  runTicks(s, frameFor(s), 1, m, log);
  CHECK(s.rules.phase == MatchPhase::MatchEnded);
  CHECK(m.teamScore(s.rules.scores, 1) == 2);
  CHECK(firstEvent(log, EventKind::MatchEnded).player.isValid());
  CHECK(m.teamOf(firstEvent(log, EventKind::MatchEnded).player) == 1);
}

TEST_CASE("capture: an eliminated carrier drops the flag, which goes home untouched",
          "[scenario][capture]") {
  const MatchConfig config = objectiveConfig(kCapture, 0b1111, 0x0B1E, kBases);
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  parkEveryone(s);
  placePlayer(s, kP1, at(-13, 0));
  runTicks(s, frameFor(s), 1, m, log);
  REQUIRE(playerAt(s, kP1).tags.has(Tag::Carrying));
  placePlayer(s, kP1, at(0, 5));
  log.clear();
  eliminate(s, m, kP0, kP1, log);
  // Dropped where it stood, as a pickup.
  const Pickup* dropped = flagOf(s, 0);
  REQUIRE(dropped != nullptr);
  CHECK(dropped->pos == at(0, 5));
  const SimEvent& laid = firstEvent(log, EventKind::PickupSpawned);
  CHECK(laid.a == static_cast<uint32_t>(PickupKind::Flag));
  CHECK(laid.b == 0);
  CHECK(scoresSince(log, 0).empty());  // eliminations score nothing
  // The victim's own team cannot pick it up: only the clock returns it.
  placePlayer(s, kP2, at(0, 5));
  int ticks = 1;  // the drop tick counted none
  while (flagOf(s, 0) != nullptr && flagOf(s, 0)->pos == at(0, 5) && ticks < 200) {
    runTicks(s, frameFor(s), 1, m, log);
    ++ticks;
  }
  CHECK(ticks == kFlagReturnTicks + 1);
  REQUIRE(flagOf(s, 0) != nullptr);
  CHECK(flagOf(s, 0)->pos == at(-13, 0));
  CHECK(s.playerBySlot(kP1) != nullptr);  // respawned in the meantime
}

TEST_CASE("capture: no capture while the own flag is away; picking up again restarts",
          "[scenario][capture]") {
  const MatchConfig config = objectiveConfig(kCapture, 0b1111, 0x0B1E, kBases);
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  parkEveryone(s);
  // P1 (team 1) takes flag 0, P0 (team 0) takes flag 1.
  placePlayer(s, kP1, at(-13, 0));
  placePlayer(s, kP0, at(13, 0));
  runTicks(s, frameFor(s), 1, m, log);
  REQUIRE(playerAt(s, kP1).tags.has(Tag::Carrying));
  REQUIRE(playerAt(s, kP0).tags.has(Tag::Carrying));
  // P1 waits in its own zone: team 1's flag is away, no capture.
  placePlayer(s, kP1, at(12, 1));
  placePlayer(s, kP0, at(0, -5));
  log.clear();
  runTicks(s, frameFor(s), 20, m, log);
  CHECK(scoresSince(log, 0).empty());
  // P0 is eliminated: flag 1 drops at (0, -5); P3 (team 1) cannot take it,
  // P2 (team 0) may, which restarts its clock. P2 drops it again later.
  eliminate(s, m, kP1, kP0, log);
  REQUIRE(flagOf(s, 1) != nullptr);
  runTicks(s, frameFor(s), 30, m, log);
  placePlayer(s, kP2, at(0, -5));
  runTicks(s, frameFor(s), 1, m, log);
  REQUIRE(playerAt(s, kP2).tags.has(Tag::Carrying));
  placePlayer(s, kP2, at(-4, -2));  // clear of every spawn point
  eliminate(s, m, kP3, kP2, log);
  REQUIRE(flagOf(s, 1) != nullptr);
  CHECK(flagOf(s, 1)->pos == at(-4, -2));
  CHECK(scoresSince(log, 0).empty());
  // A fresh return clock: home after kFlagReturnTicks, then P1 captures.
  const size_t before = log.size();
  runTicks(s, frameFor(s), kFlagReturnTicks, m, log);
  REQUIRE(flagOf(s, 1) != nullptr);
  CHECK(flagOf(s, 1)->pos == at(13, 0));
  runTicks(s, frameFor(s), 1, m, log);
  const std::vector<SimEvent> scores = scoresSince(log, before);
  REQUIRE(scores.size() == 1);
  CHECK(scores[0].player == kP1);
}

TEST_CASE("compatible: capture needs a zone per team", "[scenario][zones][capture]") {
  const ContentDB& c = content();
  CHECK_FALSE(c.compatible(0, kCapture));
  CHECK(c.compatible(kBases, kCapture));
  CHECK(c.compatible(kZoned, kCapture));  // three zones: team t plays zone t
  ContentDB oneZone = c;
  oneZone.arenas[kBases].zoneCount = 1;
  oneZone.arenas[kZoned].zoneCount = 1;
  CHECK_FALSE(oneZone.compatible(kBases, kCapture));
  CHECK(oneZone.compatible(kBases, kHill));
  CHECK_FALSE(oneZone.firstCompatibleArena(kCapture).has_value());
}
