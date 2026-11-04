// Call behaviour hooks (architecture §5.5, §8, v0.35). The symbol is the aim
// byte of the press tick (Intent::aim, the frame's aimDir exactly), not the
// facing: on the grid the facing only follows the aim at a turn's latch.
// The phases and the cooldown are the def's like any ability's; nothing else
// in the world changes.
#include "kfm/sim/abilities/call.hpp"

namespace kfm::sim {

bool callCanActivate(const WorldState& /*s*/, const Player& /*p*/, const AbilityDef& /*def*/) {
  return true;
}

// NOLINTNEXTLINE(misc-const-correctness): parameter types fixed by AbilityBehavior
void callOnActivate(WorldState& s, Player& p, AbilityState& /*st*/, const AbilityDef& def,
                    const MatchContent& /*content*/, EventSink& sink) {
  const EntityId self = s.players.idAt(static_cast<uint16_t>(p.id.value));
  const auto symbol = static_cast<uint32_t>(p.intent.aim.bam >> 8);
  sink.push(SimEvent{EventKind::MonkeyCalled, s.tick, p.id, self, symbol,
                     static_cast<uint32_t>(def.param0.raw)});
}

void callOnTick(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
                const MatchContent& /*content*/, EventSink& /*sink*/) {}

void callOnPhaseChange(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/,
                       const AbilityDef& /*def*/, AbilityPhase /*from*/, AbilityPhase /*to*/,
                       const MatchContent& /*content*/, EventSink& /*sink*/) {}

void callOnEnd(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
               const MatchContent& /*content*/, EventSink& /*sink*/) {}

}  // namespace kfm::sim
