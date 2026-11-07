// Team deathmatch (architecture §5.3, v0.32): deathmatch between teams
// (MatchConfig::teams). One round on a clock with deathmatch's respawns and
// roster; an elimination of an enemy scores a point for the attacker's slot
// (ScoreChanged, per slot as in deathmatch), an ally's (friendly fire) or
// one's own scores nothing. The first team whose slots' scores sum to
// scoreToWin wins, else the leading team when timeLimitTicks runs out (a
// tied lead is a draw); MatchEnded names the team's present top scorer.
// Unlike deathmatch, a slot that leaves keeps its score: it is the team's.
#include "kfm/sim/modes/modes.hpp"
#include "modes/mode_flow.hpp"

#include <cstdint>
#include <utility>

namespace kfm::sim {

void teamDeathmatchOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink) {
  const ModeDef& mode = content.mode;
  deathmatchRoster(s, content, /*forfeitScore=*/false);
  clockWarnings(s, mode.timeLimitTicks, sink);
  int16_t best = 0;
  const PlayerId leader = teamScoreLeader(s, content, best);
  if (std::cmp_greater_equal(best, mode.scoreToWin) || clockExpired(s.rules, mode.timeLimitTicks)) {
    endMatch(s, leader, sink);
  }
}

void teamDeathmatchOnEliminated(WorldState& s, const MatchContent& content, PlayerId victim,
                                PlayerId killer, EventSink& sink) {
  MatchRules& r = s.rules;
  if (victim.isValid() && victim.value < kMaxPlayers) {
    if (r.deaths[victim.value] < UINT8_MAX) ++r.deaths[victim.value];
    r.respawnTicks[victim.value] = respawnDelay(content.mode);
  }
  if (!killer.isValid() || killer.value >= kMaxPlayers) return;
  if (content.relation(killer, victim) != Relation::Enemy) return;
  addScore(s, killer, 1, ScoreReason::Objective, sink);
}

}  // namespace kfm::sim
