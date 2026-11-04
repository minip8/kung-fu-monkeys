// Call ability (architecture §5.5, §8, v0.35): communication through the
// game. On the press the monkey shouts its aim byte, one of 256 symbols,
// with no change to the 5-byte PlayerInput: the sim emits MonkeyCalled{player,
// a = symbol, b = range} and does nothing else. Who hears it (every monkey
// within the def's range, param0, ally or enemy) is the bot hosts' business
// (ai::AgentHost folds it into the observation); the client draws a shout.
#pragma once

#include "kfm/sim/abilities/registry.hpp"

namespace kfm::sim {

bool callCanActivate(const WorldState& s, const Player& p, const AbilityDef& def);
void callOnActivate(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                    const MatchContent& content, EventSink& sink);
void callOnTick(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                const MatchContent& content, EventSink& sink);
void callOnPhaseChange(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                       AbilityPhase from, AbilityPhase to, const MatchContent& content,
                       EventSink& sink);
void callOnEnd(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
               const MatchContent& content, EventSink& sink);

[[nodiscard]] constexpr AbilityBehavior callBehavior() {
  return AbilityBehavior{&callCanActivate,   &callOnActivate, &callOnTick,
                         &callOnPhaseChange, &callOnEnd,      &noKindTags};
}

}  // namespace kfm::sim
