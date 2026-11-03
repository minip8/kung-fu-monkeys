// Parry ability (architecture §5.5, §5.6, v0.21): a short timed window. The
// slot grants Tag::Parrying while its active window may be open; rule row 15
// reflects a non-own banana that reaches the parrier inside the exact window.
#pragma once

#include "kfm/sim/abilities/registry.hpp"

namespace kfm::sim {

bool parryCanActivate(const WorldState& s, const Player& p, const AbilityDef& def);
void parryOnActivate(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                     const MatchContent& content, EventSink& sink);
void parryOnTick(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                 const MatchContent& content, EventSink& sink);
void parryOnPhaseChange(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                        AbilityPhase from, AbilityPhase to, const MatchContent& content,
                        EventSink& sink);
void parryOnEnd(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                const MatchContent& content, EventSink& sink);
Tags parryKindTags(const AbilityState& st, const AbilityDef& def);

[[nodiscard]] constexpr AbilityBehavior parryBehavior() {
  return AbilityBehavior{&parryCanActivate,   &parryOnActivate, &parryOnTick,
                         &parryOnPhaseChange, &parryOnEnd,      &parryKindTags};
}

}  // namespace kfm::sim
