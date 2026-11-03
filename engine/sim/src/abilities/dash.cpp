// Dash behaviour hooks (architecture §5.5). The direction is captured once at
// activation (move direction, or facing when idle); during Active the player's
// velocity is pinned to dir * def.speed and MovementSystem leaves it alone
// because AbilitySystem grants Tag::Dashing. Leaving Active needs no hook:
// the tag drops and MovementSystem drives velocity from intent again.
#include "kfm/sim/abilities/dash.hpp"

namespace kfm::sim {

bool dashCanActivate(const WorldState& /*s*/, const Player& /*p*/, const AbilityDef& /*def*/) {
  return true;
}

// NOLINTNEXTLINE(misc-const-correctness): parameter types fixed by AbilityBehavior
void dashOnActivate(WorldState& s, Player& p, AbilityState& st, const AbilityDef& /*def*/,
                    const ContentDB& /*content*/, EventSink& sink) {
  const Vec2 move = p.intent.move;
  st.dir = move == Vec2::zero() ? Vec2::fromAngle(p.transform.facing) : normalized(move);
  const EntityId self = s.players.idAt(static_cast<uint16_t>(p.id.value));
  sink.push(SimEvent{EventKind::PlayerDashed, s.tick, p.id, self, 0, 0});
}

// NOLINTNEXTLINE(misc-const-correctness): parameter types fixed by AbilityBehavior
void dashOnTick(WorldState& /*s*/, Player& p, AbilityState& st, const AbilityDef& def,
                const ContentDB& /*content*/, EventSink& /*sink*/) {
  if (st.phase == AbilityPhase::Active) p.transform.vel = st.dir * def.speed;
}

void dashOnPhaseChange(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/,
                       const AbilityDef& /*def*/, AbilityPhase /*from*/, AbilityPhase /*to*/,
                       const ContentDB& /*content*/, EventSink& /*sink*/) {}

void dashOnEnd(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
               const ContentDB& /*content*/, EventSink& /*sink*/) {}

}  // namespace kfm::sim
