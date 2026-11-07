// Team rounds (architecture §5.3, v0.32): rounds between teams
// (MatchConfig::teams). A round ends when monkeys of at most one team are
// left while slots of two or more teams are present; that team wins it (none
// left: a draw). The round win is credited to the team's lowest surviving
// slot (MatchRules::scores stays per slot, RoundEnded names that slot), and
// the first team whose slots' round wins sum to roundsToWin takes the match
// (endRound judges content.teamScore). Never touches the per-player mode
// counters. With every slot its own team it plays exactly like rounds.
// v0.57: no round ends while a monkey is falling down a pit (anyFalling).
#include "kfm/sim/modes/modes.hpp"
#include "modes/mode_flow.hpp"

#include <bit>
#include <cstdint>

namespace kfm::sim {

void teamRoundsOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink) {
  if (std::popcount(static_cast<uint32_t>(teamsPresent(s, content))) < 2) return;
  const uint16_t alive = teamsAlive(s, content);
  if (std::popcount(static_cast<uint32_t>(alive)) > 1) return;
  if (anyFalling(s)) return;
  const PlayerId winner =
      alive != 0 ? lowestAliveOf(s, content, static_cast<uint8_t>(std::countr_zero(alive)))
                 : PlayerId::invalid();
  endRound(s, content, winner, sink);
}

void teamRoundsOnEliminated(WorldState& /*s*/, const MatchContent& /*content*/, PlayerId /*victim*/,
                            PlayerId /*killer*/, EventSink& /*sink*/) {}

}  // namespace kfm::sim
