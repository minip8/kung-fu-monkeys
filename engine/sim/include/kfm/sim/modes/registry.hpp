// ModeBehavior table indexed by ModeKind (architecture §5.3, v0.23), the
// mode counterpart of the ability registry. Every mode shares RulesSystem's
// phase machine (Lobby -> RoundStarting -> Playing -> RoundEnded ->
// MatchEnded), the round start that spawns the roster and the respawn
// timers LifecycleSystem runs; a mode only decides what an elimination means
// and when a round or the match ends. The table is constexpr. New mode =
// behaviour file + ModeKind + loader name + one registry line + a scenario
// test.
#pragma once

#include "kfm/core/types.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/world_state.hpp"

namespace kfm::sim {

// Hooks of one mode. Every pointer is non-null.
struct ModeBehavior {
  // RulesSystem post-tick, every Playing tick after the clock (ticksInPhase)
  // advanced: roster changes, timer warnings and the win conditions. Ends a
  // round or the match through the shared helpers (modes/mode_flow.hpp).
  void (*onPlaying)(WorldState& s, const MatchContent& content, EventSink& sink);
  // The damage step (rules/apply_damage.hpp), right after it tagged `victim`
  // Eliminated and pushed PlayerEliminated. killer is the attacker's slot,
  // or invalid when the source has none.
  void (*onEliminated)(WorldState& s, const MatchContent& content, PlayerId victim, PlayerId killer,
                       EventSink& sink);
};

// Behaviour for a kind; out-of-range kinds get the Rounds behaviour.
[[nodiscard]] const ModeBehavior& modeBehaviorFor(ModeKind kind);

}  // namespace kfm::sim
