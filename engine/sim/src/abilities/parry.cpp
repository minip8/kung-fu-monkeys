// Parry behaviour hooks (architecture §5.5, §5.11, v0.21). The window is
// [press + startupTicks, press + startupTicks + activeTicks) measured from
// the press instant (activatedTick + activatedSubTick / 256), like the
// perfect dash's. Tag::Parrying says "the window may be open this tick": the
// Active ticks, plus the first Recovery tick when the press had a non-zero
// sub-tick (the window closes inside it). The parry rule (rules/parry.cpp)
// judges the exact instant against the contact's tHit. PlayerParryStarted on
// the press, for the stance effect.
#include "kfm/sim/abilities/parry.hpp"

namespace kfm::sim {

bool parryCanActivate(const WorldState& /*s*/, const Player& /*p*/, const AbilityDef& /*def*/) {
  return true;
}

// NOLINTNEXTLINE(misc-const-correctness): parameter types fixed by AbilityBehavior
void parryOnActivate(WorldState& s, Player& p, AbilityState& st, const AbilityDef& /*def*/,
                     const MatchContent& /*content*/, EventSink& sink) {
  st.dir = Vec2::fromAngle(p.transform.facing);
  const EntityId self = s.players.idAt(static_cast<uint16_t>(p.id.value));
  sink.push(SimEvent{EventKind::PlayerParryStarted, s.tick, p.id, self, 0, 0});
}

void parryOnTick(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
                 const MatchContent& /*content*/, EventSink& /*sink*/) {}

void parryOnPhaseChange(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/,
                        const AbilityDef& /*def*/, AbilityPhase /*from*/, AbilityPhase /*to*/,
                        const MatchContent& /*content*/, EventSink& /*sink*/) {}

void parryOnEnd(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
                const MatchContent& /*content*/, EventSink& /*sink*/) {}

Tags parryKindTags(const AbilityState& st, const AbilityDef& /*def*/) {
  const bool open =
      st.phase == AbilityPhase::Active ||
      (st.phase == AbilityPhase::Recovery && st.phaseTicks == 0 && st.activatedSubTick != 0);
  return open ? Tags::of(Tag::Parrying) : Tags{};
}

}  // namespace kfm::sim
