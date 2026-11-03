// Dash ability (architecture §5.5): the base movement verb. Active phase grants
// Dashing, its first perfectWindowTicks grant PerfectDash (the catch window).
#pragma once

#include "kfm/sim/abilities/registry.hpp"

namespace kfm::sim {

bool dashCanActivate(const WorldState& s, const Player& p, const AbilityDef& def);
void dashOnActivate(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                    const MatchContent& content, EventSink& sink);
void dashOnTick(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                const MatchContent& content, EventSink& sink);
void dashOnPhaseChange(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                       AbilityPhase from, AbilityPhase to, const MatchContent& content,
                       EventSink& sink);
void dashOnEnd(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
               const MatchContent& content, EventSink& sink);
Tags dashKindTags(const AbilityState& st, const AbilityDef& def);

[[nodiscard]] constexpr AbilityBehavior dashBehavior() {
  return AbilityBehavior{&dashCanActivate,   &dashOnActivate, &dashOnTick,
                         &dashOnPhaseChange, &dashOnEnd,      &dashKindTags};
}

}  // namespace kfm::sim
