// The end of a pit fall (architecture §5.6, v0.57), shared by the pit row
// (rules/hazard.cpp) and LifecycleSystem, which calls it on the tick a
// falling monkey's Player::fallTicks runs out; and the bananas of a monkey
// that dies. Internal to engine/sim/src.
#pragma once

#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/world_state.hpp"

namespace kfm::sim {

// Eliminates the falling monkey p by nobody: hp 0, Tag::Eliminated,
// PlayerEliminated with killer 0xFF, then the mode's onEliminated with an
// invalid killer. LifecycleSystem despawns it in the same tick.
void eliminateFallen(WorldState& s, const MatchContent& content, Player& p, EventSink& sink);

// v0.57: every banana of `owner` still away from it (Outbound or Returning)
// drops where it is (Grounded, velocity zero); LifecycleSystem lays it as a
// pickup this tick, or sinks it if it came down over a pit. Called when a
// monkey starts to fall (it is doomed) and as an eliminated one despawns.
void dropBananasOf(WorldState& s, PlayerId owner);

}  // namespace kfm::sim
