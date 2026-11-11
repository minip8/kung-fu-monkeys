// Control (architecture §5.3, §11 "Contest design", v0.79): every zone of the
// arena has an owner; the team with strictly the most monkeys in a zone it
// does not own captures it after captureTicks; an owned zone pays its team's
// clock whether or not anyone stands on it, hillTicks of clock a point. The
// modeData layout is a bot contract (docs/bot-abi.md §3): words 0-1 the
// clocks, word 2 the owners, word 3 the challengers, words 4-7 the progress.
// Hand-built content only: no shipped row or arena.
#include "kfm/sim/scripted_inputs.hpp"
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

constexpr uint8_t kZoned = 1;         // the dojo with two spawn groups and three zones
constexpr uint8_t kControl = 1;       // 10 ticks to capture, 20 zone-ticks a point, first to 20
constexpr uint8_t kControlWin = 2;    // the same, 5 zone-ticks a point, first team to 2
constexpr uint8_t kControlClock = 3;  // the same on a 100-tick clock, first team to 99
constexpr uint8_t kTeamDm = 4;        // team deathmatch with the same teams (no objective)
constexpr uint16_t kCaptureTicks = 10;
constexpr uint16_t kPointTicks = 20;
constexpr uint8_t kNobody = 0xFF;
constexpr PlayerId kP0{0};  // team 0
constexpr PlayerId kP1{1};  // team 1
constexpr PlayerId kP2{2};  // team 0
constexpr PlayerId kP3{3};  // team 1

[[nodiscard]] constexpr Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

ContentDB controlContent() {
  ContentDB c = defaultContent();
  ArenaDef& zoned = c.arenas[kZoned];
  zoned = c.arenas[0];
  zoned.name = ContentName::of("zoned");
  zoned.spawnGroups = 2;
  zoned.zones[0] = Zone{at(-2, -2), at(2, 2)};     // the centre
  zoned.zones[1] = Zone{at(-15, -2), at(-11, 2)};  // west
  zoned.zones[2] = Zone{at(11, -2), at(15, 2)};    // east
  zoned.zoneCount = 3;
  c.arenaCount = 2;
  ModeDef& control = c.modes[kControl];
  control.name = ContentName::of("control");
  control.kind = ModeKind::Control;
  control.teams = 2;
  control.captureTicks = kCaptureTicks;
  control.hillTicks = kPointTicks;
  control.scoreToWin = 20;
  control.timeLimitTicks = 60000;
  control.roundStartTicks = 10;
  control.respawnTicks = 20;
  control.respawnInvulnTicks = 0;
  c.modes[kControlWin] = control;
  c.modes[kControlWin].name = ContentName::of("control_win");
  c.modes[kControlWin].hillTicks = 5;
  c.modes[kControlWin].scoreToWin = 2;
  c.modes[kControlClock] = control;
  c.modes[kControlClock].name = ContentName::of("control_clock");
  c.modes[kControlClock].timeLimitTicks = 100;
  c.modes[kControlClock].scoreToWin = 99;
  c.modes[kTeamDm] = control;
  c.modes[kTeamDm].name = ContentName::of("team_dm");
  c.modes[kTeamDm].kind = ModeKind::TeamDeathmatch;
  c.modeCount = 5;
  return c;
}

const ContentDB& content() {
  static const ContentDB c = controlContent();
  return c;
}

MatchConfig controlConfig(uint8_t mode, PlayerMask players = 0b1111) {
  return MatchConfig{.seed = 0xC0A7,
                     .players = players,
                     .arena = kZoned,
                     .mode = mode,
                     .teams = teamsFor(content().modeAt(mode), players)};
}

// The match in Playing with every monkey parked outside every zone. The
// dojo's spawn points at (+-12, 0) lie in the side zones, so a monkey may
// have stood in one for the first Playing tick: never long enough to take
// it, and the capture under way is cleared here so each test starts clean.
WorldState playing(const MatchConfig& config, EventLog& log) {
  WorldState s = initialWorld(config);
  const MatchContent m = matchContent(content(), config);
  for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i) {
    runTicks(s, frameFor(s), 1, m, log);
  }
  REQUIRE(s.rules.phase == MatchPhase::Playing);
  const std::array<Vec2, 4> spots = {at(-8, 6), at(8, 6), at(-8, -6), at(8, -6)};
  for (uint8_t i = 0; i < 4; ++i) {
    if (s.playerBySlot(PlayerId{i}) != nullptr) placePlayer(s, PlayerId{i}, spots[i]);
  }
  REQUIRE(s.rules.modeData[0] == 0);  // no clock, no owner
  REQUIRE(s.rules.modeData[1] == 0);
  REQUIRE(s.rules.modeData[2] == 0);
  s.rules.modeData = {};
  log.clear();
  return s;
}

// The modeData fields, read the way docs/bot-abi.md §3 tells a bot to.
[[nodiscard]] uint32_t clockOf(const WorldState& s, uint8_t team) {
  return (s.rules.modeData[team / 2] >> ((team % 2) * 16)) & 0xFFFFu;
}
[[nodiscard]] uint8_t nibbleTeam(const WorldState& s, uint8_t word, uint8_t zone) {
  const uint32_t v = (s.rules.modeData[word] >> (4u * zone)) & 0xFu;
  return v == 0 ? kNobody : static_cast<uint8_t>(v - 1);
}
[[nodiscard]] uint8_t ownerOf(const WorldState& s, uint8_t zone) {
  return nibbleTeam(s, 2, zone);
}
[[nodiscard]] uint8_t challengerOf(const WorldState& s, uint8_t zone) {
  return nibbleTeam(s, 3, zone);
}
[[nodiscard]] uint32_t progressOf(const WorldState& s, uint8_t zone) {
  return (s.rules.modeData[4 + (zone / 2)] >> ((zone % 2) * 16)) & 0xFFFFu;
}

// The ScoreChanged events in the log from `from` on.
std::vector<SimEvent> scoresSince(const EventLog& log, size_t from = 0) {
  std::vector<SimEvent> out;
  for (size_t i = from; i < log.size(); ++i) {
    if (log[i].kind == EventKind::ScoreChanged) out.push_back(log[i]);
  }
  return out;
}

// Team 0 takes `zone` with kP2 standing in it for captureTicks, then kP2
// leaves it for its parking spot.
void captureWithP2(WorldState& s, const MatchContent& m, uint8_t zone, EventLog& log) {
  placePlayer(s, kP2, m.arena.zones[zone].centre());
  runTicks(s, frameFor(s), kCaptureTicks, m, log);
  REQUIRE(ownerOf(s, zone) == 0);
  placePlayer(s, kP2, at(-8, -6));
}

}  // namespace

TEST_CASE("control: a strict majority captures after capture_ticks", "[scenario][control]") {
  const MatchConfig config = controlConfig(kControl);
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  // Alone in the centre: the challenger, a tick of progress per tick.
  placePlayer(s, kP2, at(0, 0));
  runTicks(s, frameFor(s), kCaptureTicks - 1, m, log);
  CHECK(countEvents(log, EventKind::ZoneCaptured) == 0);
  CHECK(ownerOf(s, 0) == kNobody);
  CHECK(challengerOf(s, 0) == 0);
  CHECK(progressOf(s, 0) == kCaptureTicks - 1);
  CHECK(s.rules.modeData[3] == 0x1);  // zone 0's nibble: team 0 + 1
  CHECK(s.rules.modeData[4] == kCaptureTicks - 1);
  runTicks(s, frameFor(s), 1, m, log);
  REQUIRE(countEvents(log, EventKind::ZoneCaptured) == 1);
  const SimEvent& taken = firstEvent(log, EventKind::ZoneCaptured);
  CHECK(taken.a == 0);  // the team
  CHECK(taken.b == 0);  // the zone
  CHECK_FALSE(taken.player.isValid());
  CHECK(ownerOf(s, 0) == 0);
  CHECK(challengerOf(s, 0) == kNobody);
  CHECK(progressOf(s, 0) == 0);
  CHECK(s.rules.modeData[2] == 0x1);
  CHECK(s.rules.modeData[3] == 0);
  CHECK(s.rules.modeData[4] == 0);
  // The zone pays from the tick it is taken.
  CHECK(clockOf(s, 0) == 1);
  CHECK(s.rules.modeData[0] == 1);
  CHECK(scoresSince(log).empty());

  // Two of team 1 against one of team 0 in the east zone (zone 2): team 1
  // has the majority, and it captures.
  placePlayer(s, kP0, at(13, 0));
  placePlayer(s, kP1, at(12, 1));
  placePlayer(s, kP3, at(14, -1));
  log.clear();
  runTicks(s, frameFor(s), kCaptureTicks, m, log);
  REQUIRE(countEvents(log, EventKind::ZoneCaptured) == 1);
  CHECK(firstEvent(log, EventKind::ZoneCaptured).a == 1);
  CHECK(firstEvent(log, EventKind::ZoneCaptured).b == 2);
  CHECK(ownerOf(s, 2) == 1);
  CHECK(s.rules.modeData[2] == ((0x2u << 8) | 0x1u));  // zone 2: team 1 + 1, zone 0: team 0 + 1
  CHECK(clockOf(s, 1) == 1);
  CHECK((s.rules.modeData[0] >> 16) == 1);  // team 1's clock in the high half of word 0
  // Staying in its own zone keeps it.
  runTicks(s, frameFor(s), 30, m, log);
  CHECK(countEvents(log, EventKind::ZoneCaptured) == 1);
  CHECK(ownerOf(s, 2) == 1);
  CHECK(progressOf(s, 2) == 0);
}

TEST_CASE("control: a tie holds the progress, a new challenger starts from zero",
          "[scenario][control]") {
  const MatchConfig config = controlConfig(kControl);
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  placePlayer(s, kP0, at(0, 0));
  runTicks(s, frameFor(s), 5, m, log);
  CHECK(challengerOf(s, 0) == 0);
  CHECK(progressOf(s, 0) == 5);
  // One of each team: a tie for the most holds it where it is.
  placePlayer(s, kP1, at(1, 1));
  runTicks(s, frameFor(s), 40, m, log);
  CHECK(challengerOf(s, 0) == 0);
  CHECK(progressOf(s, 0) == 5);
  CHECK(ownerOf(s, 0) == kNobody);
  // Team 1 out-numbers team 0: a new challenger, from zero.
  placePlayer(s, kP3, at(-1, -1));
  runTicks(s, frameFor(s), 1, m, log);
  CHECK(challengerOf(s, 0) == 1);
  CHECK(progressOf(s, 0) == 1);
  runTicks(s, frameFor(s), 2, m, log);
  CHECK(progressOf(s, 0) == 3);
  // Back to team 0 alone: team 0 starts over too, its old 5 is gone.
  placePlayer(s, kP1, at(8, 6));
  placePlayer(s, kP3, at(8, -6));
  runTicks(s, frameFor(s), 1, m, log);
  CHECK(challengerOf(s, 0) == 0);
  CHECK(progressOf(s, 0) == 1);
  // An empty zone clears it.
  placePlayer(s, kP0, at(-8, 6));
  runTicks(s, frameFor(s), 1, m, log);
  CHECK(challengerOf(s, 0) == kNobody);
  CHECK(progressOf(s, 0) == 0);
  CHECK(countEvents(log, EventKind::ZoneCaptured) == 0);
}

TEST_CASE("control: the owner with the most clears the progress, a tie with it holds",
          "[scenario][control]") {
  const MatchConfig config = controlConfig(kControl);
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  captureWithP2(s, m, 0, log);
  // Team 1 alone in team 0's zone: a capture under way.
  placePlayer(s, kP1, at(0, 0));
  runTicks(s, frameFor(s), 4, m, log);
  CHECK(challengerOf(s, 0) == 1);
  CHECK(progressOf(s, 0) == 4);
  // The owner comes back one to one: a tie, held.
  placePlayer(s, kP0, at(1, 0));
  runTicks(s, frameFor(s), 20, m, log);
  CHECK(challengerOf(s, 0) == 1);
  CHECK(progressOf(s, 0) == 4);
  CHECK(ownerOf(s, 0) == 0);
  // Two of the owner against one: the capture is off.
  placePlayer(s, kP2, at(-1, 0));
  runTicks(s, frameFor(s), 1, m, log);
  CHECK(challengerOf(s, 0) == kNobody);
  CHECK(progressOf(s, 0) == 0);
  CHECK(ownerOf(s, 0) == 0);
  // The owner alone keeps it clear; team 1 alone then starts from zero and
  // takes the zone after the full captureTicks.
  placePlayer(s, kP1, at(8, 6));
  runTicks(s, frameFor(s), 5, m, log);
  CHECK(progressOf(s, 0) == 0);
  placePlayer(s, kP0, at(-8, 6));
  placePlayer(s, kP2, at(-8, -6));
  placePlayer(s, kP1, at(0, 0));
  log.clear();
  runTicks(s, frameFor(s), kCaptureTicks - 1, m, log);
  CHECK(ownerOf(s, 0) == 0);
  runTicks(s, frameFor(s), 1, m, log);
  CHECK(ownerOf(s, 0) == 1);
  REQUIRE(countEvents(log, EventKind::ZoneCaptured) == 1);
  CHECK(firstEvent(log, EventKind::ZoneCaptured).a == 1);
}

TEST_CASE("control: an owned zone pays with nobody on it, two pay twice as fast",
          "[scenario][control]") {
  const MatchConfig config = controlConfig(kControl);
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  captureWithP2(s, m, 0, log);  // the capture tick paid 1
  CHECK(clockOf(s, 0) == 1);
  // Nobody in the zone: it still pays a tick per tick. The point goes to
  // the team's lowest present slot (kP0), not to kP2 who took the zone.
  runTicks(s, frameFor(s), kPointTicks - 2, m, log);
  CHECK(scoresSince(log).empty());
  CHECK(clockOf(s, 0) == kPointTicks - 1);
  runTicks(s, frameFor(s), 1, m, log);
  std::vector<SimEvent> scores = scoresSince(log);
  REQUIRE(scores.size() == 1);
  CHECK(scores[0].player == kP0);
  CHECK(scores[0].a == 1);
  CHECK(scores[0].b == static_cast<uint32_t>(ScoreReason::Objective));
  CHECK(clockOf(s, 0) == 0);
  CHECK(clockOf(s, 1) == 0);  // team 1 owns nothing

  // Take the west zone too: 9 ticks with one zone, then the capture tick pays two.
  size_t before = log.size();
  captureWithP2(s, m, 1, log);
  CHECK(clockOf(s, 0) == (kCaptureTicks - 1) + 2);
  CHECK(scoresSince(log, before).empty());
  // Two zones: 20 zone-ticks every 10 ticks, the remainder carried.
  runTicks(s, frameFor(s), 4, m, log);  // 11 + 8 = 19
  CHECK(scoresSince(log, before).empty());
  runTicks(s, frameFor(s), 1, m, log);  // 21: a point, 1 carried
  CHECK(scoresSince(log, before).size() == 1);
  CHECK(clockOf(s, 0) == 1);
  before = log.size();
  runTicks(s, frameFor(s), 9, m, log);  // 19
  CHECK(scoresSince(log, before).empty());
  runTicks(s, frameFor(s), 1, m, log);  // 21 again
  scores = scoresSince(log, before);
  REQUIRE(scores.size() == 1);
  CHECK(scores[0].player == kP0);
  CHECK(clockOf(s, 0) == 1);
  CHECK(m.teamScore(s.rules.scores, 0) == 3);

  // kP0 leaves: the points go to the team's lowest present slot, kP2.
  InputFrame f = frameFor(s);
  f.present = 0b1110;
  before = log.size();
  runTicks(s, f, 10, m, log);
  scores = scoresSince(log, before);
  REQUIRE(scores.size() == 1);
  CHECK(scores[0].player == kP2);
  CHECK(m.teamScore(s.rules.scores, 0) == 4);  // a leaver's points stay the team's
}

TEST_CASE("control: score_to_win ends the match", "[scenario][control]") {
  const MatchConfig config = controlConfig(kControlWin);
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  placePlayer(s, kP3, m.arena.zones[2].centre());
  runTicks(s, frameFor(s), kCaptureTicks, m, log);
  REQUIRE(ownerOf(s, 2) == 1);
  CHECK(clockOf(s, 1) == 1);
  runTicks(s, frameFor(s), 8, m, log);  // 9: one point, 4 carried
  CHECK(m.teamScore(s.rules.scores, 1) == 1);
  CHECK(s.rules.phase == MatchPhase::Playing);
  runTicks(s, frameFor(s), 1, m, log);  // 10: the second point
  CHECK(s.rules.phase == MatchPhase::MatchEnded);
  CHECK(m.teamScore(s.rules.scores, 1) == 2);
  const SimEvent& ended = firstEvent(log, EventKind::MatchEnded);
  CHECK(ended.player.isValid());
  CHECK(m.teamOf(ended.player) == 1);
}

TEST_CASE("control: the clock ends the match with the leader, a tied lead is a draw",
          "[scenario][control]") {
  const MatchConfig config = controlConfig(kControlClock);
  const MatchContent m = matchContent(content(), config);
  {
    // Team 1 owns a zone for most of the clock: it leads and wins.
    EventLog log;
    WorldState s = playing(config, log);
    placePlayer(s, kP1, m.arena.zones[0].centre());
    runTicks(s, frameFor(s), 100, m, log);
    CHECK(s.rules.phase == MatchPhase::MatchEnded);
    CHECK(m.teamScore(s.rules.scores, 1) > 0);
    CHECK(m.teamScore(s.rules.scores, 0) == 0);
    const SimEvent& ended = firstEvent(log, EventKind::MatchEnded);
    CHECK(ended.player.isValid());
    CHECK(m.teamOf(ended.player) == 1);
  }
  {
    // Each team owns a zone from the same tick: the same points, a draw.
    EventLog log;
    WorldState s = playing(config, log);
    placePlayer(s, kP0, m.arena.zones[1].centre());
    placePlayer(s, kP1, m.arena.zones[2].centre());
    runTicks(s, frameFor(s), 100, m, log);
    CHECK(s.rules.phase == MatchPhase::MatchEnded);
    CHECK(m.teamScore(s.rules.scores, 0) > 0);
    CHECK(m.teamScore(s.rules.scores, 0) == m.teamScore(s.rules.scores, 1));
    CHECK_FALSE(firstEvent(log, EventKind::MatchEnded).player.isValid());
  }
  {
    // Nobody takes anything: a draw at 0 to 0.
    EventLog log;
    WorldState s = playing(config, log);
    runTicks(s, frameFor(s), 100, m, log);
    CHECK(s.rules.phase == MatchPhase::MatchEnded);
    CHECK_FALSE(firstEvent(log, EventKind::MatchEnded).player.isValid());
  }
}

TEST_CASE("control: eliminations score nothing, the eliminated respawn", "[scenario][control]") {
  const MatchConfig config = controlConfig(kControl);
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  const Vec2 victim = playerAt(s, kP1).transform.pos;
  spawnBanana(s, kP0, victim - Vec2{Fixed::one(), Fixed::zero()},
              Vec2{Fixed::fromRatio(1, 2), Fixed::zero()}, BoomerangPhase::Outbound, m.db);
  runTicks(s, frameFor(s), 5, m, log);
  CHECK(firstEvent(log, EventKind::PlayerEliminated).player == kP1);
  CHECK(scoresSince(log).empty());
  CHECK(s.rules.deaths[kP1.value] == 1);
  runTicks(s, frameFor(s), 20, m, log);
  CHECK(countEvents(log, EventKind::PlayerRespawned) == 1);
  CHECK(s.playerBySlot(kP1) != nullptr);
}

TEST_CASE("control: the round start clears modeData", "[scenario][control]") {
  const MatchConfig config = controlConfig(kControl);
  const MatchContent m = matchContent(content(), config);
  WorldState s = initialWorld(config);
  s.rules.modeData.fill(0xFFFFFFFFu);
  EventLog log;
  for (int i = 0; i < 1000 && s.rules.phase == MatchPhase::Lobby; ++i) {
    runTicks(s, frameFor(s), 1, m, log);
  }
  REQUIRE(s.rules.phase == MatchPhase::RoundStarting);
  for (const uint32_t word : s.rules.modeData)
    CHECK(word == 0);
}

TEST_CASE("compatible: control needs an arena with a zone", "[scenario][zones][control]") {
  const ContentDB& c = content();
  CHECK_FALSE(c.compatible(0, kControl));  // the dojo: no zone, one spawn group
  CHECK(c.compatible(kZoned, kControl));
  CHECK(c.firstCompatibleArena(kControl) == std::optional<uint8_t>{kZoned});
  ContentDB oneZone = c;
  oneZone.arenas[kZoned].zoneCount = 1;
  CHECK(oneZone.compatible(kZoned, kControl));
  ContentDB noZone = c;
  noZone.arenas[kZoned].zoneCount = 0;
  CHECK_FALSE(noZone.compatible(kZoned, kControl));
  CHECK_FALSE(noZone.firstCompatibleArena(kControl).has_value());
}

// The scripted generator (the corpus's inputs) walks a control monkey to the
// nearest zone its team does not own, and draws from the Rng exactly what it
// draws in a mode without an objective, so no other entry moves.
TEST_CASE("control: the scripted generator steers to the nearest zone not owned, Rng-free",
          "[scenario][control][scripted]") {
  const MatchConfig config = controlConfig(kControl);
  const MatchContent control = matchContent(content(), config);
  const MatchContent plain(content(), kZoned, kTeamDm, kAllPowerUps, 0, config.teams);
  EventLog log;
  WorldState s = playing(config, log);
  REQUIRE(playerAt(s, kP0).ammo > 0);  // hands full: the objective, not a banana
  // kP0 (team 0) between the centre (6 away) and the west zone (7 away).
  placePlayer(s, kP0, at(-6, 0));
  const auto inputs = [&](const MatchContent& m, Rng& rng) {
    return scriptedInput(rng, s, m, kP0);
  };
  const auto expectSteer = [&](uint8_t moveDir) {
    Rng a = Rng::seeded(7);
    Rng b = Rng::seeded(7);
    const PlayerInput steered = inputs(control, a);
    const PlayerInput wandered = inputs(plain, b);
    CHECK(a.s == b.s);  // the same draws
    CHECK(steered.moveDir == moveDir);
    CHECK(steered.moveMag == wandered.moveMag);
    CHECK(steered.aimDir == wandered.aimDir);
    CHECK(steered.buttons == wandered.buttons);
    CHECK(steered.subTick == wandered.subTick);
  };
  // Nobody owns anything: the centre, straight east (+x).
  expectSteer(0);
  // Team 1 owns the centre: still a zone team 0 does not own.
  s.rules.modeData[2] = 0x2u;
  expectSteer(0);
  // Team 0 owns the centre: the west zone, straight west.
  s.rules.modeData[2] = 0x1u;
  expectSteer(128);
  // Team 0 owns every zone, or kP0 stands in one it does not own: no
  // target, it wanders exactly as without an objective.
  const auto expectWander = [&] {
    Rng a = Rng::seeded(9);
    Rng b = Rng::seeded(9);
    const PlayerInput steered = inputs(control, a);
    const PlayerInput wandered = inputs(plain, b);
    CHECK(a.s == b.s);
    CHECK(steered.moveDir == wandered.moveDir);
    CHECK(steered.aimDir == wandered.aimDir);
    CHECK(steered.buttons == wandered.buttons);
  };
  s.rules.modeData[2] = 0x111u;
  expectWander();
  s.rules.modeData[2] = 0;
  placePlayer(s, kP0, at(0, 1));
  expectWander();
}
