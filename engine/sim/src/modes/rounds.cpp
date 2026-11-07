// Rounds mode (architecture §5.3): the M1 free-for-all. A round ends when at
// most one monkey is left and at least two slots are present; the survivor
// (if any) wins it, and the first to roundsToWin round wins takes the match.
// v0.57: not while a monkey is falling down a pit (anyFalling); a round with
// no banana left anywhere ends as a draw (roundRanDry, mode_flow.hpp).
// Never touches the per-player mode counters (deaths, respawn, spawn protection).
#include "kfm/sim/modes/modes.hpp"
#include "modes/mode_flow.hpp"

#include <bit>
#include <cstdint>

namespace kfm::sim {

void roundsOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink) {
  if (std::popcount(static_cast<uint32_t>(s.rules.present)) < 2) return;
  if (anyFalling(s)) return;
  if (s.players.size() <= 1) {
    endRound(s, content, soleSurvivor(s), sink);
  } else if (roundRanDry(s, content)) {
    endRound(s, content, PlayerId::invalid(), sink);  // v0.57: no banana left, a draw
  }
}

void roundsOnEliminated(WorldState& /*s*/, const MatchContent& /*content*/, PlayerId /*victim*/,
                        PlayerId /*killer*/, EventSink& /*sink*/) {}

}  // namespace kfm::sim
