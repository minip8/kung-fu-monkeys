// Slam ability (architecture §5.5, §5.6, v0.21): after its startup the monkey
// lands a radial impulse. The first Active tick grants Tag::Slamming, which
// widens the player's contact radius to the def's param0 for CollisionSystem;
// rule rows then push players (param1 units over the active ticks) and ground
// non-own bananas inside the radius.
#pragma once

#include "kfm/sim/abilities/registry.hpp"

namespace kfm::sim {

bool slamCanActivate(const WorldState& s, const Player& p, const AbilityDef& def);
void slamOnActivate(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                    const MatchContent& content, EventSink& sink);
void slamOnTick(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                const MatchContent& content, EventSink& sink);
void slamOnPhaseChange(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                       AbilityPhase from, AbilityPhase to, const MatchContent& content,
                       EventSink& sink);
void slamOnEnd(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
               const MatchContent& content, EventSink& sink);
Tags slamKindTags(const AbilityState& st, const AbilityDef& def);

[[nodiscard]] constexpr AbilityBehavior slamBehavior() {
  return AbilityBehavior{&slamCanActivate,   &slamOnActivate, &slamOnTick,
                         &slamOnPhaseChange, &slamOnEnd,      &slamKindTags};
}

}  // namespace kfm::sim
