// The end of a pit fall (architecture §5.6, v0.57), shared by the pit row
// (rules/hazard.cpp) and LifecycleSystem, which calls it on the tick a
// falling monkey's Player::fallTicks runs out, and the faller's bananas that
// go down with it. Internal to engine/sim/src.
#pragma once

#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/world_state.hpp"

namespace kfm::sim {

// Eliminates the falling monkey p by nobody: hp 0, Tag::Eliminated,
// PlayerEliminated with killer 0xFF, every banana it still owns lost
// (loseBanana), then the mode's onEliminated with an invalid killer.
// LifecycleSystem despawns it in the same tick.
void eliminateFallen(WorldState& s, const MatchContent& content, Player& p, EventSink& sink);

// A falling monkey's banana gone down the pit where it is: despawned (never a
// pickup), BoomerangFell{owner, banana, Lost}.
void loseBanana(WorldState& s, EntityId banana, EventSink& sink);

}  // namespace kfm::sim
