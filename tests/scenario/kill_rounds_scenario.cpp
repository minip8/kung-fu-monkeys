// Kill rounds (architecture §5.3, v0.90): free-for-all rounds scored by
// eliminations. A kill is a point to the attacker on the tick it happens,
// surviving a round scores nothing, a death with no killer scores nobody, and
// the match is judged only at a round's end: the strict leader at
// scoreToWin or more takes it, a tied lead plays another round.
#include "kfm/sim/modes/registry.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kKillRounds = 1;
constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};
constexpr PlayerId kP2{2};

// The default content plus a kill rounds mode to two kills (built in code:
// the sim tests do not depend on the loader).
ContentDB killContent() {
  ContentDB c = defaultContent();
  ModeDef& kr = c.modes[kKillRounds];
  kr.name = ContentName::of("kill_rounds");
  kr.kind = ModeKind::KillRounds;
  kr.roundStartTicks = 10;
  kr.roundEndTicks = 30;
  kr.scoreToWin = 2;
  c.modeCount = 2;
  return c;
}

const ContentDB& content() {
  static const ContentDB c = killContent();
  return c;
}

MatchContent killMatch() {
  return MatchContent{content(), 0, kKillRounds};
}

// Steps until the next Playing phase (the match start or the next round).
void untilPlaying(WorldState& s, const MatchContent& m, EventLog& log) {
  for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i) {
    runTicks(s, frameFor(s), 1, m, log);
  }
  REQUIRE(s.rules.phase == MatchPhase::Playing);
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

// Fires a banana owned by `killer` into `victim` from one unit to its left
// and steps until it is eliminated (or `maxTicks` pass).
bool kill(WorldState& s, const MatchContent& m, PlayerId killer, PlayerId victim, EventLog& log,
          int maxTicks = 10) {
  const Vec2 at = playerAt(s, victim).transform.pos;
  spawnBanana(s, killer, at - Vec2{Fixed::one(), Fixed::zero()},
              Vec2{Fixed::fromRatio(1, 2), Fixed::zero()}, BoomerangPhase::Outbound, m.db);
  const size_t before = log.size();
  for (int t = 0; t < maxTicks; ++t) {
    runTicks(s, frameFor(s), 1, m, log);
    for (size_t i = before; i < log.size(); ++i) {
      if (log[i].kind == EventKind::PlayerEliminated && log[i].player == victim) return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("kill rounds: a kill scores at once and surviving scores nothing",
          "[scenario][modes][kill_rounds]") {
  const MatchContent m = killMatch();
  WorldState s = initialWorld(MatchConfig{.seed = 0x4B11, .players = 0b111, .mode = kKillRounds});
  EventLog log;
  untilPlaying(s, m, log);
  spreadOut(s);

  log.clear();
  REQUIRE(kill(s, m, kP0, kP1, log));
  const SimEvent& elim = firstEvent(log, EventKind::PlayerEliminated);
  const SimEvent& scored = firstEvent(log, EventKind::ScoreChanged);
  CHECK(scored.player == kP0);
  CHECK(scored.a == 1);
  CHECK(scored.b == static_cast<uint32_t>(ScoreReason::Objective));
  CHECK(scored.tick == elim.tick);
  CHECK(s.rules.phase == MatchPhase::Playing);  // two monkeys are left
  CHECK(s.rules.deaths[1] == 1);

  // P2 takes P0 out: one monkey is left, the round ends with P2 as its
  // survivor, and the survival itself is worth nothing.
  log.clear();
  REQUIRE(kill(s, m, kP2, kP0, log));
  runTicks(s, frameFor(s), 1, m, log);
  CHECK(s.rules.phase == MatchPhase::RoundEnded);
  CHECK(firstEvent(log, EventKind::RoundEnded).player == kP2);
  CHECK(countEvents(log, EventKind::MatchEnded) == 0);  // 1 : 0 : 1, nobody has two
  CHECK(s.rules.scores[0] == 1);
  CHECK(s.rules.scores[1] == 0);
  CHECK(s.rules.scores[2] == 1);
  CHECK(s.rules.respawnTicks[0] == 0);  // nobody respawns within a round
}

TEST_CASE("kill rounds: a death with no killer scores nobody", "[scenario][modes][kill_rounds]") {
  const MatchContent m = killMatch();
  WorldState s = initialWorld(MatchConfig{.seed = 0x4B12, .players = 0b111, .mode = kKillRounds});
  EventLog log;
  untilPlaying(s, m, log);
  const ModeBehavior& b = modeBehaviorFor(ModeKind::KillRounds);
  EventSink sink;
  b.onEliminated(s, m, kP1, PlayerId::invalid(), sink);  // a pit fall
  b.onEliminated(s, m, kP2, kP2, sink);                  // its own banana
  CHECK(countEvents(sink, EventKind::ScoreChanged) == 0);
  CHECK(s.rules.scores == std::array<int8_t, kMaxPlayers>{});
  CHECK(s.rules.deaths[1] == 1);
  CHECK(s.rules.deaths[2] == 1);
}

TEST_CASE("kill rounds: a tied lead plays another round, the match ends at a round's end",
          "[scenario][modes][kill_rounds]") {
  const MatchContent m = killMatch();
  WorldState s = initialWorld(MatchConfig{.seed = 0x4B13, .players = 0b111, .mode = kKillRounds});
  EventLog log;
  untilPlaying(s, m, log);
  spreadOut(s);

  // Round 1: P0 kills P1, P2 kills P0: 1 : 0 : 1, under the target of two.
  REQUIRE(kill(s, m, kP0, kP1, log));
  REQUIRE(kill(s, m, kP2, kP0, log));
  runTicks(s, frameFor(s), 1, m, log);
  REQUIRE(s.rules.phase == MatchPhase::RoundEnded);

  // Round 2: P2 kills P1 (2 for P2 mid-round: the match goes on), P0 kills
  // P2: 2 : 0 : 2, a tied lead at the target, so a third round is played.
  untilPlaying(s, m, log);
  CHECK(s.rules.round == 2);
  spreadOut(s);
  log.clear();
  REQUIRE(kill(s, m, kP2, kP1, log));
  CHECK(s.rules.scores[2] == 2);
  CHECK(s.rules.phase == MatchPhase::Playing);  // never judged mid-round
  REQUIRE(kill(s, m, kP0, kP2, log));
  runTicks(s, frameFor(s), 1, m, log);
  CHECK(s.rules.scores[0] == 2);
  CHECK(s.rules.scores[2] == 2);
  CHECK(s.rules.phase == MatchPhase::RoundEnded);
  CHECK(countEvents(log, EventKind::MatchEnded) == 0);

  // Round 3: P0 kills P1 and P2 and is the strict leader at 4 : 0 : 2.
  untilPlaying(s, m, log);
  CHECK(s.rules.round == 3);
  spreadOut(s);
  log.clear();
  REQUIRE(kill(s, m, kP0, kP1, log));
  REQUIRE(kill(s, m, kP0, kP2, log));
  runTicks(s, frameFor(s), 1, m, log);
  CHECK(s.rules.phase == MatchPhase::MatchEnded);
  CHECK(firstEvent(log, EventKind::RoundEnded).player == kP0);
  CHECK(firstEvent(log, EventKind::MatchEnded).player == kP0);
  CHECK(s.rules.lastRoundWinner == kP0);
  CHECK(s.rules.scores[0] == 4);
}

TEST_CASE("kill rounds: the match winner need not be the last round's survivor",
          "[scenario][modes][kill_rounds]") {
  const MatchContent m = killMatch();
  WorldState s = initialWorld(MatchConfig{.seed = 0x4B14, .players = 0b111, .mode = kKillRounds});
  EventLog log;
  untilPlaying(s, m, log);
  spreadOut(s);
  // Round 1: P0 kills P1, P2 kills P0: 1 : 0 : 1.
  REQUIRE(kill(s, m, kP0, kP1, log));
  REQUIRE(kill(s, m, kP2, kP0, log));
  runTicks(s, frameFor(s), 1, m, log);
  REQUIRE(s.rules.phase == MatchPhase::RoundEnded);
  // Round 2: P0 kills P2 (2 kills), P1 kills P0 and survives: P0 leads 2 : 1
  // : 1 at the round's end and takes the match; the round names P1.
  untilPlaying(s, m, log);
  spreadOut(s);
  log.clear();
  REQUIRE(kill(s, m, kP0, kP2, log));
  REQUIRE(kill(s, m, kP1, kP0, log));
  runTicks(s, frameFor(s), 1, m, log);
  CHECK(s.rules.phase == MatchPhase::MatchEnded);
  CHECK(firstEvent(log, EventKind::RoundEnded).player == kP1);
  CHECK(firstEvent(log, EventKind::MatchEnded).player == kP0);
  CHECK(s.rules.lastRoundWinner == kP0);  // the HUD's match winner
}
