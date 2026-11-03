// Blink ability (architecture §5.5, v0.21): a teleport dash. After its
// startup the monkey moves instantly by speed × activeTicks along the
// direction captured at the press, stopped by walls and the arena box.
#pragma once

#include "kfm/sim/abilities/registry.hpp"

namespace kfm::sim {

bool blinkCanActivate(const WorldState& s, const Player& p, const AbilityDef& def);
void blinkOnActivate(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                     const MatchContent& content, EventSink& sink);
void blinkOnTick(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                 const MatchContent& content, EventSink& sink);
void blinkOnPhaseChange(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                        AbilityPhase from, AbilityPhase to, const MatchContent& content,
                        EventSink& sink);
void blinkOnEnd(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                const MatchContent& content, EventSink& sink);

[[nodiscard]] constexpr AbilityBehavior blinkBehavior() {
  return AbilityBehavior{&blinkCanActivate,   &blinkOnActivate, &blinkOnTick,
                         &blinkOnPhaseChange, &blinkOnEnd,      &noKindTags};
}

}  // namespace kfm::sim
