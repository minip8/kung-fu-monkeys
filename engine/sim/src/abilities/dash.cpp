// Dash behaviour hooks (architecture §5.5). M1 stub: the parallel ability work
// fills these in (contract: abilitySystem, Dash).
#include "kfm/sim/abilities/dash.hpp"

namespace kfm::sim {

bool dashCanActivate(const WorldState& /*s*/, const Player& /*p*/, const AbilityDef& /*def*/) {
  return true;
}

void dashOnActivate(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/,
                    const AbilityDef& /*def*/, const ContentDB& /*content*/, EventSink& /*sink*/) {}

void dashOnTick(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
                const ContentDB& /*content*/, EventSink& /*sink*/) {}

void dashOnPhaseChange(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/,
                       const AbilityDef& /*def*/, AbilityPhase /*from*/, AbilityPhase /*to*/,
                       const ContentDB& /*content*/, EventSink& /*sink*/) {}

void dashOnEnd(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
               const ContentDB& /*content*/, EventSink& /*sink*/) {}

}  // namespace kfm::sim
