// ModeBehavior table indexed by ModeKind (architecture §5.3, v0.23, v0.31),
// the mode counterpart of the ability registry. Every mode shares
// RulesSystem's phase machine (Lobby -> RoundStarting -> Playing ->
// RoundEnded -> MatchEnded), the round start that spawns the roster and the
// respawn timers LifecycleSystem runs; a mode decides what happens at a round
// start, what an elimination and a pickup mean and when a round or the match
// ends, and keeps whatever state it needs in MatchRules::modeData. The table
// is constexpr. New mode = behaviour file + ModeKind + loader name + one
// registry line + a scenario test.
#pragma once

#include "kfm/core/types.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/world_state.hpp"

namespace kfm::sim {

// Hooks of one mode. Every pointer is non-null; a kind with nothing to do
// in a hook uses the shared no-ops (modes/modes.hpp).
struct ModeBehavior {
  // RulesSystem pre-tick, at every round start (v0.31): after the shared
  // spawn of the present slots and RoundStarted, before the countdown runs
  // (phase RoundStarting, ticksInPhase 0). Objective placement, per-round
  // counters in modeData.
  void (*onRoundStart)(WorldState& s, const MatchContent& content, EventSink& sink);
  // RulesSystem post-tick, every Playing tick after the clock (ticksInPhase)
  // advanced: roster changes, timer warnings and the win conditions. Ends a
  // round or the match through the shared helpers (modes/mode_flow.hpp).
  void (*onPlaying)(WorldState& s, const MatchContent& content, EventSink& sink);
  // The damage step (rules/apply_damage.hpp), right after it tagged `victim`
  // Eliminated and pushed PlayerEliminated. killer is the attacker's slot,
  // or invalid when the source has none.
  void (*onEliminated)(WorldState& s, const MatchContent& content, PlayerId victim, PlayerId killer,
                       EventSink& sink);
  // InteractionSystem (v0.31), right after a pickup rule row applied (rows
  // 50 and 51: the banana or power-up is taken, despawned and PickupTaken
  // pushed). `pickup` is the taken pickup as it was.
  void (*onPickupTaken)(WorldState& s, const MatchContent& content, PlayerId player,
                        const Pickup& pickup, EventSink& sink);
};

// Behaviour for a kind; out-of-range kinds get the Rounds behaviour.
[[nodiscard]] const ModeBehavior& modeBehaviorFor(ModeKind kind);

// The behaviour a match runs: content.modeBehavior when a test set one,
// else modeBehaviorFor(content.mode.kind). Every hook call site uses it.
[[nodiscard]] inline const ModeBehavior& modeBehaviorOf(const MatchContent& content) {
  return content.modeBehavior != nullptr ? *content.modeBehavior
                                         : modeBehaviorFor(content.mode.kind);
}

}  // namespace kfm::sim
