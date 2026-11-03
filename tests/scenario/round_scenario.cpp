// End-to-end FFA round flow through step() (architecture §5.3, §9): spawn,
// countdown, eliminations, round end, pause, respawn. Eliminations are
// applied with the damage rule on a hand-built contact, so this scenario does
// not depend on throwing, flight or collision.
#include "kfm/sim/rules/handlers.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

// A banana thrown by `attacker` lands on `victim` (one-hit kill by default).
void eliminate(WorldState& s, PlayerId victim, PlayerId attacker, EventLog& log) {
  const Player& p = playerAt(s, victim);
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

}  // namespace

TEST_CASE("a three-player round: last one standing wins, then everyone respawns",
          "[scenario][round]") {
  const ContentDB& content = defaultContent();
  WorldState s = initialWorld(MatchConfig{41, 0b111});
  EventLog log;

  // Lobby -> countdown -> Playing.
  runTicks(s, frameFor(s), content.modes[0].roundStartTicks, content, log);
  CHECK(s.rules.phase == MatchPhase::RoundStarting);
  runTicks(s, frameFor(s), 1, content, log);
  REQUIRE(s.rules.phase == MatchPhase::Playing);
  CHECK(countEvents(log, EventKind::MatchStarted) == 1);
  CHECK(countEvents(log, EventKind::PlayerSpawned) == 3);
  CHECK(countEvents(log, EventKind::RoundStarted) == 1);
  log.clear();

  // Player 2 falls: the round goes on with two players.
  eliminate(s, PlayerId{2}, PlayerId{0}, log);
  runTicks(s, frameFor(s), 1, content, log);
  CHECK(s.players.size() == 2);
  CHECK(s.playerBySlot(PlayerId{2}) == nullptr);
  CHECK(s.rules.phase == MatchPhase::Playing);
  CHECK(countEvents(log, EventKind::PlayerEliminated) == 1);
  CHECK(countEvents(log, EventKind::BoomerangGrounded) == 1);
  CHECK(countEvents(log, EventKind::PickupSpawned) == 1);
  CHECK(s.pickups.size() == 1);
  log.clear();

  // Player 1 falls: player 0 wins the round.
  eliminate(s, PlayerId{1}, PlayerId{0}, log);
  const Tick endTick = s.tick;
  runTicks(s, frameFor(s), 1, content, log);
  CHECK(s.rules.phase == MatchPhase::RoundEnded);
  CHECK(s.rules.lastRoundWinner == PlayerId{0});
  CHECK(s.rules.scores[0] == 1);
  const SimEvent* ended = findEvent(log, EventKind::RoundEnded);
  REQUIRE(ended != nullptr);
  CHECK(ended->player == PlayerId{0});
  CHECK(ended->a == 1);
  CHECK(ended->tick == endTick);
  log.clear();

  // The pause lasts roundEndTicks, then round 2 starts with everyone back.
  runTicks(s, frameFor(s), content.modes[0].roundEndTicks, content, log);
  CHECK(log.empty());
  CHECK(s.rules.phase == MatchPhase::RoundEnded);
  runTicks(s, frameFor(s), 1, content, log);
  CHECK(s.rules.phase == MatchPhase::RoundStarting);
  CHECK(s.rules.round == 2);
  CHECK(s.players.size() == 3);
  CHECK(s.pickups.empty());
  CHECK(s.boomerangs.empty());
  CHECK(countEvents(log, EventKind::PlayerSpawned) == 3);
  REQUIRE(findEvent(log, EventKind::RoundStarted) != nullptr);
  CHECK(firstEvent(log, EventKind::RoundStarted).a == 2);
  for (uint8_t i = 0; i < 3; ++i) {
    CHECK(playerAt(s, PlayerId{i}).hp == content.player.maxHp);
    CHECK(playerAt(s, PlayerId{i}).tags == Tags{});
  }
  runTicks(s, frameFor(s), content.modes[0].roundStartTicks, content, log);
  CHECK(s.rules.phase == MatchPhase::Playing);
}

TEST_CASE("a round where the last two fall in the same tick is a draw", "[scenario][round]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(42, 0b11);
  EventLog log;
  eliminate(s, PlayerId{0}, PlayerId{1}, log);
  eliminate(s, PlayerId{1}, PlayerId{0}, log);
  runTicks(s, frameFor(s), 1, content, log);
  CHECK(s.rules.phase == MatchPhase::RoundEnded);
  CHECK(s.players.empty());
  CHECK_FALSE(s.rules.lastRoundWinner.isValid());
  CHECK(s.rules.scores[0] == 0);
  CHECK(s.rules.scores[1] == 0);
  const SimEvent* ended = findEvent(log, EventKind::RoundEnded);
  REQUIRE(ended != nullptr);
  CHECK_FALSE(ended->player.isValid());

  // Nobody scored, so play resumes with round 2.
  runTicks(s, frameFor(s), content.modes[0].roundEndTicks + 1, content, log);
  CHECK(s.rules.phase == MatchPhase::RoundStarting);
  CHECK(s.rules.round == 2);
  CHECK(s.players.size() == 2);
}
