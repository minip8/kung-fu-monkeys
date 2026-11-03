// Slam behaviour hooks (architecture §5.5, §5.6, v0.21). The behaviour only
// marks the impact: Tag::Slamming on the first Active tick (kindTags) and a
// SlamLanded event on the Startup -> Active transition. What the impact does
// lives in the rules table (rules/slam.cpp): CollisionSystem widens a
// Slamming player's contact radius to def.param0, and rows 16 and 26 ground
// the bananas and push the monkeys the widened contacts reach. The slammer
// keeps walking during the startup (no Dashing: that would arm the dash
// attack), so the jump is a short, readable telegraph.
#include "kfm/sim/abilities/slam.hpp"

namespace kfm::sim {

bool slamCanActivate(const WorldState& /*s*/, const Player& /*p*/, const AbilityDef& /*def*/) {
  return true;
}

// NOLINTNEXTLINE(misc-const-correctness): parameter types fixed by AbilityBehavior
void slamOnActivate(WorldState& /*s*/, Player& p, AbilityState& st, const AbilityDef& /*def*/,
                    const MatchContent& /*content*/, EventSink& /*sink*/) {
  st.dir = Vec2::fromAngle(p.transform.facing);  // the push direction for a monkey dead centre
}

void slamOnTick(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
                const MatchContent& /*content*/, EventSink& /*sink*/) {}

// NOLINTNEXTLINE(misc-const-correctness): parameter types fixed by AbilityBehavior
void slamOnPhaseChange(WorldState& s, Player& p, AbilityState& /*st*/, const AbilityDef& def,
                       AbilityPhase from, AbilityPhase to, const MatchContent& /*content*/,
                       EventSink& sink) {
  if (from != AbilityPhase::Startup || to != AbilityPhase::Active) return;
  const EntityId self = s.players.idAt(static_cast<uint16_t>(p.id.value));
  sink.push(SimEvent{EventKind::SlamLanded, s.tick, p.id, self,
                     static_cast<uint32_t>(def.param0.raw), 0});
}

void slamOnEnd(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
               const MatchContent& /*content*/, EventSink& /*sink*/) {}

// The impact tick: the first Active tick only, so each slam pushes and
// grounds once however long its active phase lasts.
Tags slamKindTags(const AbilityState& st, const AbilityDef& /*def*/) {
  return st.phase == AbilityPhase::Active && st.phaseTicks == 0 ? Tags::of(Tag::Slamming) : Tags{};
}

}  // namespace kfm::sim
