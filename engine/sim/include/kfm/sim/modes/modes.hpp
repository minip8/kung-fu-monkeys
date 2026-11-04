// The mode behaviours of the ModeBehavior table (architecture §5.3, v0.23),
// one source file each under src/modes/.
#pragma once

#include "kfm/sim/modes/registry.hpp"

namespace kfm::sim {

// The shared no-op hooks (modes/registry.cpp) of a kind with nothing to do
// at a round start or on a pickup (v0.31: rounds, deathmatch, survival).
void modeNoRoundStart(WorldState& s, const MatchContent& content, EventSink& sink);
void modeNoPickupTaken(WorldState& s, const MatchContent& content, PlayerId player,
                       const Pickup& pickup, EventSink& sink);

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

// Team rounds (modes/team_rounds.cpp, v0.32): the last team standing.
void teamRoundsOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink);
void teamRoundsOnEliminated(WorldState& s, const MatchContent& content, PlayerId victim,
                            PlayerId killer, EventSink& sink);

// Team deathmatch (modes/team_deathmatch.cpp, v0.32): team scores, respawns.
void teamDeathmatchOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink);
void teamDeathmatchOnEliminated(WorldState& s, const MatchContent& content, PlayerId victim,
                                PlayerId killer, EventSink& sink);

// Hill (modes/hill.cpp, v0.33): a team scores for holding zone 0 alone.
void hillOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink);
void hillOnEliminated(WorldState& s, const MatchContent& content, PlayerId victim, PlayerId killer,
                      EventSink& sink);
void hillOnRoundStart(WorldState& s, const MatchContent& content, EventSink& sink);

[[nodiscard]] constexpr ModeBehavior roundsBehavior() {
  return ModeBehavior{&modeNoRoundStart, &roundsOnPlaying, &roundsOnEliminated, &modeNoPickupTaken};
}
[[nodiscard]] constexpr ModeBehavior deathmatchBehavior() {
  return ModeBehavior{&modeNoRoundStart, &deathmatchOnPlaying, &deathmatchOnEliminated,
                      &modeNoPickupTaken};
}
[[nodiscard]] constexpr ModeBehavior survivalBehavior() {
  return ModeBehavior{&modeNoRoundStart, &survivalOnPlaying, &survivalOnEliminated,
                      &modeNoPickupTaken};
}
[[nodiscard]] constexpr ModeBehavior teamRoundsBehavior() {
  return ModeBehavior{&modeNoRoundStart, &teamRoundsOnPlaying, &teamRoundsOnEliminated,
                      &modeNoPickupTaken};
}
[[nodiscard]] constexpr ModeBehavior teamDeathmatchBehavior() {
  return ModeBehavior{&modeNoRoundStart, &teamDeathmatchOnPlaying, &teamDeathmatchOnEliminated,
                      &modeNoPickupTaken};
}

[[nodiscard]] constexpr ModeBehavior hillBehavior() {
  return ModeBehavior{&hillOnRoundStart, &hillOnPlaying, &hillOnEliminated, &modeNoPickupTaken};
}

}  // namespace kfm::sim
