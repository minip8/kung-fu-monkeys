// Blink behaviour hooks (architecture §5.5, v0.21). The direction is captured
// at the press like the dash's (move direction, else facing). On the
// Startup -> Active transition the monkey is displaced by
// dir × speed × activeTicks through statics::moveCircle (Slide, outer box
// included), so walls and the arena box stop it as they stop a walk; the
// rest of Active and Recovery are plain time (the stick keeps working).
//
// The jump happens inside AbilitySystem, before MovementSystem sets prevPos
// for the swept collision, so nothing on the path is touched: a blink passes
// bananas and monkeys alike. Blink grants no tags of its own: Dashing would
// arm the dash attack (a standing blinker would strike whoever walked into
// it), so the startup is a plain telegraph during which a banana still hits.
// PlayerBlinked carries the origin (raw Q16.16 bits of x and y in a and b).
#include "kfm/sim/abilities/blink.hpp"

#include "systems/statics.hpp"

namespace kfm::sim {

bool blinkCanActivate(const WorldState& /*s*/, const Player& /*p*/, const AbilityDef& /*def*/) {
  return true;
}

// NOLINTNEXTLINE(misc-const-correctness): parameter types fixed by AbilityBehavior
void blinkOnActivate(WorldState& /*s*/, Player& p, AbilityState& st, const AbilityDef& /*def*/,
                     const MatchContent& /*content*/, EventSink& /*sink*/) {
  const Vec2 move = p.intent.move;
  st.dir = move == Vec2::zero() ? Vec2::fromAngle(p.transform.facing) : normalized(move);
}

void blinkOnTick(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
                 const MatchContent& /*content*/, EventSink& /*sink*/) {}

// NOLINTNEXTLINE(misc-const-correctness): parameter types fixed by AbilityBehavior
void blinkOnPhaseChange(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                        AbilityPhase from, AbilityPhase to, const MatchContent& content,
                        EventSink& sink) {
  if (from != AbilityPhase::Startup || to != AbilityPhase::Active) return;
  Transform& t = p.transform;
  const Vec2 origin = t.pos;
  const Vec2 delta = st.dir * (def.speed * Fixed::fromInt(def.activeTicks));
  Vec2 vel = delta;  // moveCircle's velocity out-parameter; the blink keeps t.vel
  t.pos = statics::moveCircle(
              content.arena, origin, delta, vel,
              statics::MoveParams{.radius = p.stats.radius, .response = statics::Response::Slide})
              .pos;
  t.prevPos = t.pos;
  const EntityId self = s.players.idAt(static_cast<uint16_t>(p.id.value));
  sink.push(SimEvent{EventKind::PlayerBlinked, s.tick, p.id, self,
                     static_cast<uint32_t>(origin.x.raw), static_cast<uint32_t>(origin.y.raw)});
}

void blinkOnEnd(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
                const MatchContent& /*content*/, EventSink& /*sink*/) {}

}  // namespace kfm::sim
