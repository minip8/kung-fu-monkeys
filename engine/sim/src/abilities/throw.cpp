// Throw behaviour hooks (architecture §5.5, §5.7). M1 stub: the parallel ability
// work fills these in (contract: abilitySystem, Throw).
#include "kfm/sim/abilities/throw.hpp"

namespace kfm::sim {

bool throwCanActivate(const WorldState& /*s*/, const Player& p, const AbilityDef& /*def*/) {
  return p.ammo > 0;
}

void throwOnActivate(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/,
                     const AbilityDef& /*def*/, const ContentDB& /*content*/, EventSink& /*sink*/) {
}

void throwOnTick(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
                 const ContentDB& /*content*/, EventSink& /*sink*/) {}

void throwOnPhaseChange(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/,
                        const AbilityDef& /*def*/, AbilityPhase /*from*/, AbilityPhase /*to*/,
                        const ContentDB& /*content*/, EventSink& /*sink*/) {}

void throwOnEnd(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
                const ContentDB& /*content*/, EventSink& /*sink*/) {}

}  // namespace kfm::sim
