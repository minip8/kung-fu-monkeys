// Game modes (architecture §5.3, §5.10, v0.23): the RulesSystem mode table.
// Deathmatch scores eliminations, respawns the eliminated after respawnTicks
// with spawn protection and ends on scoreToWin or the clock; survival is
// rounds on a clock with a timeout tie-break; rounds stays the M1 flow and
// never sets a mode counter.
#include "kfm/sim/scripted_inputs.hpp"
#include "kfm/sim/visit.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kDeathmatch = 1;
constexpr uint8_t kSurvival = 2;
constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};
constexpr PlayerId kP2{2};

// The default content plus a deathmatch and a survival mode (built in code:
// the sim tests do not depend on the loader).
ContentDB modeContent() {
  ContentDB c = defaultContent();
  ModeDef& dm = c.modes[kDeathmatch];
  dm.name = ContentName::of("deathmatch");
  dm.kind = ModeKind::Deathmatch;
  dm.roundStartTicks = 10;
  dm.roundEndTicks = 30;
  dm.scoreToWin = 3;
  dm.timeLimitTicks = 6000;
  dm.respawnTicks = 40;
  dm.respawnInvulnTicks = 25;
  ModeDef& sv = c.modes[kSurvival];
  sv.name = ContentName::of("survival");
  sv.kind = ModeKind::Survival;
  sv.roundStartTicks = 10;
  sv.roundEndTicks = 30;
  sv.roundsToWin = 2;
  sv.timeLimitTicks = 900;
  c.modeCount = 3;
  return c;
}

const ContentDB& content() {
  static const ContentDB c = modeContent();
  return c;
}

MatchContent modeMatch(uint8_t mode) {
  return MatchContent{content(), 0, mode};
}

WorldState playing(uint8_t mode, PlayerMask players, uint64_t seed = 0xD3A7) {
  WorldState s = initialWorld(MatchConfig{.seed = seed, .players = players, .mode = mode});
  EventLog log;
  for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i) {
    runTicks(s, frameFor(s), 1, modeMatch(mode), log);
  }
  REQUIRE(s.rules.phase == MatchPhase::Playing);
  return s;
}

// Fires a banana owned by `killer` into `victim` from one unit to its left
// and steps until it lands (or `maxTicks` pass). Returns whether it hit.
bool shoot(WorldState& s, const MatchContent& m, PlayerId killer, PlayerId victim, EventLog& log,
           int maxTicks = 10) {
  const Vec2 at = playerAt(s, victim).transform.pos;
  spawnBanana(s, killer, at - Vec2{Fixed::one(), Fixed::zero()},
              Vec2{Fixed::fromRatio(1, 2), Fixed::zero()}, BoomerangPhase::Outbound, m.db);
  const size_t before = log.size();
  for (int t = 0; t < maxTicks; ++t) {
    runTicks(s, frameFor(s), 1, m, log);
    for (size_t i = before; i < log.size(); ++i) {
      if (log[i].kind == EventKind::PlayerHit && log[i].player == victim) return true;
    }
  }
  return false;
}

// Moves the players apart so a banana for one cannot touch another.
void spreadOut(WorldState& s) {
  int x = -12;
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    if (s.playerBySlot(PlayerId{i}) == nullptr) continue;
    placePlayer(s, PlayerId{i}, Vec2{Fixed::fromInt(x), Fixed::zero()});
    x += 8;
  }
}

}  // namespace

TEST_CASE("deathmatch: an elimination scores and the victim respawns after respawnTicks",
          "[scenario][modes][deathmatch]") {
  const MatchContent m = modeMatch(kDeathmatch);
  WorldState s = playing(kDeathmatch, 0b111);
  spreadOut(s);
  EventLog log;
  REQUIRE(shoot(s, m, kP0, kP1, log));
  const SimEvent& elim = firstEvent(log, EventKind::PlayerEliminated);
  const SimEvent& scored = firstEvent(log, EventKind::ScoreChanged);
  CHECK(scored.player == kP0);
  CHECK(scored.a == 1);
  CHECK(scored.tick == elim.tick);
  CHECK(s.rules.scores[0] == 1);
  CHECK(s.rules.deaths[1] == 1);
  CHECK(s.rules.phase == MatchPhase::Playing);  // no round end: the match goes on
  CHECK(countEvents(log, EventKind::RoundEnded) == 0);
  CHECK(s.playerBySlot(kP1) == nullptr);

  // Back exactly respawnTicks after the elimination tick, not one tick sooner.
  const uint32_t due = elim.tick.value + m.mode.respawnTicks;
  while (s.tick.value <= due) {
    CHECK(s.playerBySlot(kP1) == nullptr);
    runTicks(s, frameFor(s), 1, m, log);
  }
  const SimEvent& back = firstEvent(log, EventKind::PlayerRespawned);
  CHECK(back.player == kP1);
  CHECK(back.tick.value == due);
  CHECK(back.a < m.arena.spawnPointCount);
  const Player& p = playerAt(s, kP1);
  CHECK(p.transform.pos == m.arena.spawnPoints[back.a]);
  CHECK(p.hp == content().player.maxHp);
  CHECK(p.tags.has(Tag::Invulnerable));
  CHECK(s.rules.respawnTicks[1] == 0);
}

TEST_CASE("deathmatch: spawn protection blocks hits for respawnInvulnTicks",
          "[scenario][modes][deathmatch]") {
  const MatchContent m = modeMatch(kDeathmatch);
  WorldState s = playing(kDeathmatch, 0b11);
  spreadOut(s);
  EventLog log;
  REQUIRE(shoot(s, m, kP0, kP1, log));
  for (uint16_t t = 0; t < m.mode.respawnTicks && s.playerBySlot(kP1) == nullptr; ++t)
    runTicks(s, frameFor(s), 1, m, log);
  const SimEvent& back = firstEvent(log, EventKind::PlayerRespawned);
  REQUIRE(playerAt(s, kP1).tags.has(Tag::Invulnerable));

  // Protected: a banana passes through without a hit.
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-12), Fixed::fromInt(5)});
  placePlayer(s, kP1, Vec2{Fixed::zero(), Fixed::zero()});
  CHECK_FALSE(shoot(s, m, kP0, kP1, log, 6));
  CHECK(s.rules.scores[0] == 1);

  // The tag clears after respawnInvulnTicks of interactions...
  while (s.tick.value <= back.tick.value + m.mode.respawnInvulnTicks) {
    CHECK(playerAt(s, kP1).tags.has(Tag::Invulnerable));
    runTicks(s, frameFor(s), 1, m, log);
  }
  CHECK_FALSE(playerAt(s, kP1).tags.has(Tag::Invulnerable));
  CHECK(s.rules.invulnTicks[1] == 0);
  // ...and the next banana lands.
  s.boomerangs.clear();
  placePlayer(s, kP1, Vec2{Fixed::zero(), Fixed::fromInt(-4)});
  CHECK(shoot(s, m, kP0, kP1, log));
  CHECK(s.rules.scores[0] == 2);
}

TEST_CASE("deathmatch: reaching scoreToWin ends the match", "[scenario][modes][deathmatch]") {
  const MatchContent m = modeMatch(kDeathmatch);
  WorldState s = playing(kDeathmatch, 0b111);
  spreadOut(s);
  s.rules.scores[2] = static_cast<int8_t>(m.mode.scoreToWin - 1);
  EventLog log;
  REQUIRE(shoot(s, m, kP2, kP0, log));
  CHECK(s.rules.phase == MatchPhase::MatchEnded);
  CHECK(s.rules.scores[2] == m.mode.scoreToWin);
  CHECK(s.rules.lastRoundWinner == kP2);
  REQUIRE(countEvents(log, EventKind::MatchEnded) == 1);
  CHECK(firstEvent(log, EventKind::MatchEnded).player == kP2);
  // RoundEnded then MatchEnded close the match in the elimination's tick.
  CHECK(log.back().kind == EventKind::MatchEnded);
  CHECK(log[log.size() - 2].kind == EventKind::RoundEnded);
  CHECK(log.back().tick == firstEvent(log, EventKind::PlayerEliminated).tick);

  // MatchEnded is terminal: no respawn, nothing restarts.
  runTicks(s, frameFor(s), 200, m, log);
  CHECK(s.rules.phase == MatchPhase::MatchEnded);
  CHECK(countEvents(log, EventKind::PlayerRespawned) == 0);
}

TEST_CASE("deathmatch: the clock ends the match, the leader wins, a tied lead draws",
          "[scenario][modes][deathmatch]") {
  ContentDB c = content();
  c.modes[kDeathmatch].timeLimitTicks = 700;  // 11.7 s: every warning fires
  const MatchContent m{c, 0, kDeathmatch};
  const bool tied = GENERATE(false, true);
  WorldState s = initialWorld(MatchConfig{.seed = 0x71C, .players = 0b111, .mode = kDeathmatch});
  EventLog log;
  runTicks(s, frameFor(s), 1, m, log);
  s.rules.scores = {2, 1, 0};
  if (tied) s.rules.scores[1] = 2;

  while (s.rules.phase != MatchPhase::MatchEnded && s.tick.value < 2000)
    runTicks(s, frameFor(s), 1, m, log);
  REQUIRE(s.rules.phase == MatchPhase::MatchEnded);
  const SimEvent& ended = firstEvent(log, EventKind::MatchEnded);
  // Playing starts on tick roundStartTicks (the round started on tick 0) and
  // lasts exactly timeLimitTicks ticks.
  CHECK(ended.tick.value == m.mode.roundStartTicks + 700u - 1u);
  if (tied) {
    CHECK_FALSE(ended.player.isValid());
    CHECK_FALSE(s.rules.lastRoundWinner.isValid());
  } else {
    CHECK(ended.player == kP0);
  }

  std::vector<uint32_t> warnings;
  for (const SimEvent& e : log) {
    if (e.kind != EventKind::TimerWarning) continue;
    warnings.push_back(e.a);
    // Fired when exactly that many seconds are left.
    CHECK(ended.tick.value - e.tick.value == e.a * kTickHz);
  }
  CHECK(warnings == std::vector<uint32_t>{10, 3, 2, 1});
}

TEST_CASE("deathmatch: late joiners spawn, leavers forfeit", "[scenario][modes][deathmatch]") {
  const MatchContent m = modeMatch(kDeathmatch);
  WorldState s = playing(kDeathmatch, 0b011);
  spreadOut(s);
  EventLog log;
  // Slot 2 joins mid-match: it spawns respawnTicks later.
  InputFrame f = frameFor(s);
  f.present = 0b111;
  const uint32_t joined = s.tick.value;
  runTicks(s, f, m.mode.respawnTicks + 1, m, log);
  const SimEvent& in = firstEvent(log, EventKind::PlayerRespawned);
  CHECK(in.player == kP2);
  CHECK(in.tick.value == joined + m.mode.respawnTicks);
  CHECK(s.playerBySlot(kP2) != nullptr);

  // Slot 1 leaves: its idle monkey stays until eliminated, then it neither
  // respawns nor keeps a score.
  s.rules.scores[1] = 2;
  f.present = 0b101;
  spreadOut(s);
  runTicks(s, f, 1, m, log);
  CHECK(s.playerBySlot(kP1) != nullptr);
  spawnBanana(s, kP0, playerAt(s, kP1).transform.pos - Vec2{Fixed::one(), Fixed::zero()},
              Vec2{Fixed::fromRatio(1, 2), Fixed::zero()}, BoomerangPhase::Outbound, m.db);
  log.clear();
  runTicks(s, f, 4 * m.mode.respawnTicks, m, log);
  CHECK(countEvents(log, EventKind::PlayerEliminated) == 1);
  CHECK(countEvents(log, EventKind::PlayerRespawned) == 0);
  CHECK(s.playerBySlot(kP1) == nullptr);
  CHECK(s.rules.scores[1] == 0);
  CHECK(s.rules.respawnTicks[1] == 0);
}

TEST_CASE("deathmatch: a respawn takes the spawn point farthest from the other monkeys",
          "[scenario][modes][deathmatch]") {
  const MatchContent m = modeMatch(kDeathmatch);
  const ArenaDef& arena = m.arena;
  for (uint8_t near = 0; near < arena.spawnPointCount; ++near) {
    WorldState s = playing(kDeathmatch, 0b11, 0x500 + near);
    placePlayer(s, kP0, arena.spawnPoints[near]);
    EventLog log;
    REQUIRE(shoot(s, m, kP0, kP1, log));
    placePlayer(s, kP0, arena.spawnPoints[near]);
    for (uint16_t t = 0; t < m.mode.respawnTicks && s.playerBySlot(kP1) == nullptr; ++t) {
      placePlayer(s, kP0, arena.spawnPoints[near]);  // hold P0 still on its point
      runTicks(s, frameFor(s), 1, m, log);
    }
    const SimEvent& back = firstEvent(log, EventKind::PlayerRespawned);
    uint64_t best = 0;
    for (uint8_t k = 0; k < arena.spawnPointCount; ++k)
      best = std::max(best, lengthSqRaw(arena.spawnPoints[k] - arena.spawnPoints[near]));
    CHECK(lengthSqRaw(arena.spawnPoints[back.a] - arena.spawnPoints[near]) == best);
  }
}

TEST_CASE("survival: the last monkey standing wins the round and deaths are counted",
          "[scenario][modes][survival]") {
  const MatchContent m = modeMatch(kSurvival);
  WorldState s = playing(kSurvival, 0b11);
  spreadOut(s);
  EventLog log;
  REQUIRE(shoot(s, m, kP0, kP1, log));
  CHECK(s.rules.phase == MatchPhase::RoundEnded);
  CHECK(s.rules.lastRoundWinner == kP0);
  CHECK(s.rules.scores[0] == 1);
  CHECK(s.rules.deaths[1] == 1);
  CHECK(countEvents(log, EventKind::ScoreChanged) == 0);  // round wins ride on RoundEnded
  // Nobody respawns within a survival round; the next round brings everyone back.
  runTicks(s, frameFor(s), m.mode.roundEndTicks + 1, m, log);
  CHECK(countEvents(log, EventKind::PlayerRespawned) == 0);
  CHECK(s.rules.phase == MatchPhase::RoundStarting);
  CHECK(s.playerBySlot(kP1) != nullptr);
  CHECK(s.rules.deaths[1] == 1);  // deaths are per match
}

TEST_CASE("survival: on timeout most hp wins, then fewest deaths, else a draw",
          "[scenario][modes][survival]") {
  ContentDB c = content();
  c.player.maxHp = 3;
  const MatchContent m{c, 0, kSurvival};
  struct Case {
    uint8_t hp0, hp1;
    uint8_t deaths0, deaths1;
    PlayerId winner;
  };
  const Case k = GENERATE(Case{3, 2, 5, 0, PlayerId{0}},  // most hp first
                          Case{2, 2, 1, 0, PlayerId{1}},  // then fewest deaths
                          Case{2, 2, 1, 1, PlayerId::invalid()});
  WorldState s = initialWorld(MatchConfig{.seed = 0x5E7, .players = 0b111, .mode = kSurvival});
  EventLog log;
  runTicks(s, frameFor(s), 1, m, log);
  REQUIRE(s.rules.phase == MatchPhase::RoundStarting);
  // Slot 2 is already out; slots 0 and 1 survive with the case's hp.
  s.players.despawn(s.players.idAt(2));
  s.rules.deaths = {k.deaths0, k.deaths1, 0};
  playerAt(s, kP0).hp = k.hp0;
  playerAt(s, kP1).hp = k.hp1;
  spreadOut(s);
  while (s.rules.phase != MatchPhase::RoundEnded && s.tick.value < 3000)
    runTicks(s, frameFor(s), 1, m, log);
  REQUIRE(s.rules.phase == MatchPhase::RoundEnded);
  const SimEvent& ended = firstEvent(log, EventKind::RoundEnded);
  CHECK(ended.player == k.winner);
  CHECK(countEvents(log, EventKind::TimerWarning) == 4);
  if (k.winner.isValid()) {
    CHECK(s.rules.scores[k.winner.value] == 1);
  } else {
    CHECK(s.rules.scores[0] + s.rules.scores[1] == 0);
  }
}

TEST_CASE("rounds never sets a death, respawn or spawn-protection counter",
          "[scenario][modes][rounds]") {
  // The rounds flow is the M1 one: the mode counters stay zero and no mode
  // event fires.
  const MatchConfig config{.seed = 0xB10C, .players = 0b1111};
  WorldState s = initialWorld(config);
  Rng inputs = Rng::seeded(0xB10C);
  EventSink sink;
  int scoreEvents = 0;
  for (int t = 0; t < 3600; ++t) {
    const InputFrame f = scriptedFrame(inputs, s, content());
    sink.clear();
    step(s, f, content(), sink);
    scoreEvents += countEvents(sink, EventKind::ScoreChanged) +
                   countEvents(sink, EventKind::PlayerRespawned) +
                   countEvents(sink, EventKind::TimerWarning);
    REQUIRE_FALSE(s.rules.hasModeCounters());
  }
  CHECK(scoreEvents == 0);
}

TEST_CASE("a deathmatch world round-trips through the visitor mid-respawn",
          "[scenario][modes][deathmatch]") {
  const MatchContent m = modeMatch(kDeathmatch);
  WorldState a = playing(kDeathmatch, 0b111);
  spreadOut(a);
  EventLog log;
  REQUIRE(shoot(a, m, kP0, kP1, log));
  runTicks(a, frameFor(a), 5, m, log);
  REQUIRE(a.rules.hasModeCounters());
  REQUIRE(a.rules.respawnTicks[1] > 0);

  std::array<std::byte, 16384> buf{};
  BitWriter w(buf);
  REQUIRE(writeTo(w, a));
  WorldState b{};
  BitReader r(w.written());
  REQUIRE(readFrom(r, b));
  CHECK(r.atEnd());
  CHECK(checksum(a) == checksum(b));
  CHECK(b.rules.respawnTicks == a.rules.respawnTicks);
  CHECK(b.rules.deaths == a.rules.deaths);
  // Both continue identically through the respawn.
  for (int t = 0; t < 120; ++t) {
    runTicks(a, frameFor(a), 1, m, log);
    runTicks(b, frameFor(b), 1, m, log);
    REQUIRE(checksum(a) == checksum(b));
  }
}

TEST_CASE("scripted deathmatch and survival matches are deterministic", "[scenario][modes]") {
  for (const uint8_t mode : {kDeathmatch, kSurvival}) {
    const MatchContent m = modeMatch(mode);
    const MatchConfig config{.seed = 0xD37, .players = 0b1111, .mode = mode};
    WorldState a = initialWorld(config);
    WorldState b = initialWorld(config);
    Rng ia = Rng::seeded(9);
    Rng ib = Rng::seeded(9);
    EventSink sink;
    int respawns = 0;
    for (int t = 0; t < 3600; ++t) {
      const InputFrame fa = scriptedFrame(ia, a, m);
      sink.clear();
      step(a, fa, m, sink);
      respawns += countEvents(sink, EventKind::PlayerRespawned);
      const InputFrame fb = scriptedFrame(ib, b, m);
      sink.clear();
      step(b, fb, m, sink);
      REQUIRE(checksum(a) == checksum(b));
    }
    if (mode == kDeathmatch) CHECK(respawns > 0);
  }
}
