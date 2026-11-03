// Rounds mode (architecture §5.3): the M1 free-for-all. A round ends when at
// most one monkey is left and at least two slots are present; the survivor
// (if any) wins it, and the first to roundsToWin round wins takes the match.
// Never touches the per-player mode counters (deaths, respawn, spawn protection).
#include "kfm/sim/modes/modes.hpp"
#include "modes/mode_flow.hpp"

#include <bit>
#include <cstdint>

namespace kfm::sim {

void roundsOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink) {
  if (std::popcount(static_cast<uint32_t>(s.rules.present)) < 2 || s.players.size() > 1) return;
  endRound(s, content, soleSurvivor(s), sink);
}

void roundsOnEliminated(WorldState& /*s*/, const MatchContent& /*content*/, PlayerId /*victim*/,
                        PlayerId /*killer*/, EventSink& /*sink*/) {}

}  // namespace kfm::sim
