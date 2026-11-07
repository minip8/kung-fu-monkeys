// Deathmatch mode (architecture §5.3, v0.23): one round on a clock. Every
// elimination scores a point for the attacker (ScoreChanged) and a death for
// the victim, whose respawn LifecycleSystem runs respawnTicks later. The
// first to scoreToWin wins; when timeLimitTicks of play have passed the
// leader wins, and a tie for the lead is a draw.
//
// Roster (§5.4): a slot present without a monkey and without a pending
// respawn (it joined mid-match) is scheduled like a respawn; a slot that left
// keeps its monkey until it is eliminated, then forfeits its score and stays
// out.
#include "kfm/sim/modes/modes.hpp"
#include "modes/mode_flow.hpp"

#include <cstdint>
#include <utility>

namespace kfm::sim {

void deathmatchOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink) {
  const ModeDef& mode = content.mode;
  deathmatchRoster(s, content, /*forfeitScore=*/true);
  clockWarnings(s, mode.timeLimitTicks, sink);
  int8_t best = 0;
  const PlayerId leader = scoreLeader(s.rules, best);
  if (std::cmp_greater_equal(best, mode.scoreToWin) || clockExpired(s.rules, mode.timeLimitTicks)) {
    endMatch(s, leader, sink);
  }
}

void deathmatchOnEliminated(WorldState& s, const MatchContent& content, PlayerId victim,
                            PlayerId killer, EventSink& sink) {
  MatchRules& r = s.rules;
  if (victim.isValid() && victim.value < kMaxPlayers) {
    if (r.deaths[victim.value] < UINT8_MAX) ++r.deaths[victim.value];
    r.respawnTicks[victim.value] = respawnDelay(content.mode);
  }
  if (!killer.isValid() || killer.value >= kMaxPlayers || killer == victim) return;
  addScore(s, killer, 1, ScoreReason::Objective, sink);
}

}  // namespace kfm::sim
