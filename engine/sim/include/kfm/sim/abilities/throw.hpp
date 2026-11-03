// Throw ability (architecture §5.5, §5.7): spends one ammo and spawns a
// boomerang banana on the Startup -> Active transition.
#pragma once

#include "kfm/sim/abilities/registry.hpp"

namespace kfm::sim {

bool throwCanActivate(const WorldState& s, const Player& p, const AbilityDef& def);
void throwOnActivate(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                     const MatchContent& content, EventSink& sink);
void throwOnTick(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                 const MatchContent& content, EventSink& sink);
void throwOnPhaseChange(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                        AbilityPhase from, AbilityPhase to, const MatchContent& content,
                        EventSink& sink);
void throwOnEnd(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                const MatchContent& content, EventSink& sink);

[[nodiscard]] constexpr AbilityBehavior throwBehavior() {
  return AbilityBehavior{&throwCanActivate, &throwOnActivate, &throwOnTick, &throwOnPhaseChange,
                         &throwOnEnd};
}

}  // namespace kfm::sim
