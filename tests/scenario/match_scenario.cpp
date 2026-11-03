// End-to-end FFA match through step() (architecture §5.3, §9): rounds until a
// player reaches roundsToWin, then MatchEnded is terminal. Eliminations are
// applied with the damage rule on a hand-built contact.
#include "kfm/sim/rules/handlers.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

void eliminate(WorldState& s, PlayerId victim, PlayerId attacker, EventLog& log) {
  const Player& p = *s.playerBySlot(victim);
  const EntityId banana = spawnBanana(s, attacker, p.transform.pos, Vec2::zero());
  Contact c;
  c.kindA = EntityKind::Player;
  c.a = s.players.idAt(victim.value);
  c.kindB = EntityKind::Boomerang;
  c.b = banana;
  c.point = p.transform.pos;
  EventSink sink;
  REQUIRE(damagePlayer(s, defaultContent(), c.a, c.b, c, sink));
  log.insert(log.end(), sink.begin(), sink.end());
}

// Plays a two-player match in which round i is won by winners[i]. Returns the
// per-tick checksums; the events go to log.
std::vector<uint32_t> playMatch(WorldState& s, const std::vector<uint8_t>& winners, EventLog& log) {
  const ContentDB& content = defaultContent();
  std::vector<uint32_t> sums;
  for (const uint8_t winner : winners) {
    for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i) {
      runTicks(s, frameFor(s), 1, content, log);
      sums.push_back(checksum(s));
    }
    REQUIRE(s.rules.phase == MatchPhase::Playing);
    const PlayerId w{winner};
    const PlayerId loser{static_cast<uint8_t>(1 - winner)};
    // Play a little first so the round is not decided on its first tick.
    runTicks(s, frameFor(s), 30, content, log);
    eliminate(s, loser, w, log);
    runTicks(s, frameFor(s), 1, content, log);
    sums.push_back(checksum(s));
  }
  return sums;
}

}  // namespace

TEST_CASE("first to roundsToWin takes the match; MatchEnded is terminal", "[scenario][match]") {
  const ContentDB& content = defaultContent();
  REQUIRE(content.mode.roundsToWin == 3);
  WorldState s = initialWorld(MatchConfig{51, 0b11});
  EventLog log;
  static_cast<void>(playMatch(s, {0, 1, 0, 0}, log));

  CHECK(s.rules.phase == MatchPhase::MatchEnded);
  CHECK(s.rules.scores[0] == 3);
  CHECK(s.rules.scores[1] == 1);
  CHECK(s.rules.round == 4);
  CHECK(s.rules.lastRoundWinner == PlayerId{0});
  CHECK(countEvents(log, EventKind::MatchStarted) == 1);
  CHECK(countEvents(log, EventKind::RoundStarted) == 4);
  CHECK(countEvents(log, EventKind::RoundEnded) == 4);
  CHECK(countEvents(log, EventKind::PlayerEliminated) == 4);
  REQUIRE(countEvents(log, EventKind::MatchEnded) == 1);
  const SimEvent* matchEnded = findEvent(log, EventKind::MatchEnded);
  CHECK(matchEnded->player == PlayerId{0});
  // The deciding RoundEnded comes right before MatchEnded, in the same tick.
  REQUIRE(log.size() >= 2);
  CHECK(log.back().kind == EventKind::MatchEnded);
  CHECK(log[log.size() - 2].kind == EventKind::RoundEnded);
  CHECK(log[log.size() - 2].a == 4);
  CHECK(log[log.size() - 2].tick == log.back().tick);

  // Round winners in order.
  std::vector<uint8_t> roundWinners;
  for (const SimEvent& e : log) {
    if (e.kind == EventKind::RoundEnded) roundWinners.push_back(e.player.value);
  }
  CHECK(roundWinners == std::vector<uint8_t>{0, 1, 0, 0});

  // Terminal: ticking on changes nothing but the tick and the phase timer.
  EventLog after;
  runTicks(s, frameFor(s), 600, content, after);
  CHECK(after.empty());
  CHECK(s.rules.phase == MatchPhase::MatchEnded);
  CHECK(s.players.size() == 1);
}

TEST_CASE("a scripted match is deterministic tick by tick", "[scenario][match]") {
  WorldState a = initialWorld(MatchConfig{77, 0b11});
  WorldState b = initialWorld(MatchConfig{77, 0b11});
  EventLog logA;
  EventLog logB;
  const std::vector<uint32_t> sumsA = playMatch(a, {1, 1, 0, 1}, logA);
  const std::vector<uint32_t> sumsB = playMatch(b, {1, 1, 0, 1}, logB);
  CHECK(sumsA == sumsB);
  CHECK(checksum(a) == checksum(b));
  CHECK(logA.size() == logB.size());
  CHECK(a.rules.phase == MatchPhase::MatchEnded);
  CHECK(a.rules.scores[1] == 3);

  // A different seed changes the spawn rotation, hence the state stream.
  WorldState c = initialWorld(MatchConfig{78, 0b11});
  EventLog logC;
  const std::vector<uint32_t> sumsC = playMatch(c, {1, 1, 0, 1}, logC);
  CHECK(sumsA != sumsC);
}
