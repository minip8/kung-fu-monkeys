// Kill rounds (architecture §5.3, v0.90): free-for-all rounds scored by
// eliminations. A round flows exactly like a rounds-mode round (no respawn
// within it; it ends when at most one monkey is left with two or more slots
// present, never while one is falling, and a dry round ends at once or after
// dry_round_ticks on an arena with pits), but surviving it scores nothing:
// every elimination credited to an enemy attacker is +1 to that attacker on
// the tick it happens (ScoreChanged, Objective). A death with no killer
// scores nobody (suicideScore, catchScore as in every point kind). The
// match is judged only at a round's end: when the strict leader has reached
// scoreToWin it takes the match; a tied lead plays another round.
#include "kfm/sim/modes/modes.hpp"
#include "modes/mode_flow.hpp"

#include <bit>
#include <cstdint>
#include <utility>

namespace kfm::sim {

void killRoundsOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink) {
  if (std::popcount(static_cast<uint32_t>(s.rules.present)) < 2) return;
  if (anyFalling(s)) return;
  if (s.players.size() <= 1) {
    endRoundOnScore(s, content, soleSurvivor(s), sink);
  } else if (roundRanDry(s, content)) {
    endRoundOnScore(s, content, PlayerId::invalid(), sink);  // nobody survived it alone
  }
}

void killRoundsOnEliminated(WorldState& s, const MatchContent& content, PlayerId victim,
                            PlayerId killer, EventSink& sink) {
  if (victim.isValid() && victim.value < kMaxPlayers) {
    uint8_t& deaths = s.rules.deaths[victim.value];
    if (deaths < UINT8_MAX) ++deaths;
  }
  if (!killer.isValid() || killer.value >= kMaxPlayers || killer == victim) return;
  if (content.relation(killer, victim) != Relation::Enemy) return;
  addScore(s, killer, 1, ScoreReason::Objective, sink);
}

}  // namespace kfm::sim
