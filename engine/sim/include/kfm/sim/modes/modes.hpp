// The mode behaviours of the ModeBehavior table (architecture §5.3, v0.23),
// one source file each under src/modes/.
#pragma once

#include "kfm/sim/modes/registry.hpp"

namespace kfm::sim {

// Rounds (modes/rounds.cpp): last monkey standing; eliminations need no
// bookkeeping.
void roundsOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink);
void roundsOnEliminated(WorldState& s, const MatchContent& content, PlayerId victim,
                        PlayerId killer, EventSink& sink);

// Deathmatch (modes/deathmatch.cpp): score per elimination, respawns, clock.
void deathmatchOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink);
void deathmatchOnEliminated(WorldState& s, const MatchContent& content, PlayerId victim,
                            PlayerId killer, EventSink& sink);

// Survival (modes/survival.cpp): rounds with a clock and a timeout tie-break.
void survivalOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink);
void survivalOnEliminated(WorldState& s, const MatchContent& content, PlayerId victim,
                          PlayerId killer, EventSink& sink);

[[nodiscard]] constexpr ModeBehavior roundsBehavior() {
  return ModeBehavior{&roundsOnPlaying, &roundsOnEliminated};
}
[[nodiscard]] constexpr ModeBehavior deathmatchBehavior() {
  return ModeBehavior{&deathmatchOnPlaying, &deathmatchOnEliminated};
}
[[nodiscard]] constexpr ModeBehavior survivalBehavior() {
  return ModeBehavior{&survivalOnPlaying, &survivalOnEliminated};
}

}  // namespace kfm::sim
