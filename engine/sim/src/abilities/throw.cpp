// Throw behaviour hooks (architecture §5.5, §5.7). The aim direction is
// captured at activation; the banana spawns on the Startup -> Active
// transition (with the default startup of 2, on the second tick after the
// press), spending one ammo. A full boomerang pool makes the throw fizzle
// without spending ammo or emitting an event.
#include "kfm/sim/abilities/throw.hpp"

#include "systems/angle_of.hpp"
#include "systems/statics.hpp"

namespace kfm::sim {
namespace {

void spawnBanana(WorldState& s, Player& p, const AbilityState& st, const MatchContent& content,
                 EventSink& sink) {
  const EntityId id = s.boomerangs.spawn();
  if (!id.isValid()) return;  // pool full: the throw fizzles, ammo is kept
  const BoomerangDef& def = content.db.boomerang;
  Boomerang& b = s.boomerangs.items[id.index];
  b.owner = p.id;
  b.phase = BoomerangPhase::Outbound;
  // The banana travels from the thrower's centre to its spawn point through
  // the interior walls (v0.22), so a throw at a wall bounces off it instead
  // of spawning inside or behind it. The outer box is left to
  // ProjectileSystem as in M1.
  b.transform.vel = st.dir * def.speed;
  const statics::MoveResult spawned =
      statics::moveCircle(content.arena, p.transform.pos, st.dir * def.spawnOffset, b.transform.vel,
                          statics::MoveParams{.radius = def.radius,
                                              .response = statics::Response::Bounce,
                                              .restitution = def.restitution,
                                              .outerBox = false});
  b.transform.pos = spawned.pos;
  b.transform.prevPos = b.transform.pos;
  b.transform.facing = b.transform.vel != Vec2::zero() && spawned.hitWall ? angleOf(b.transform.vel)
                                                                          : angleOf(st.dir);
  b.damage = def.damage;
  b.pierce = def.pierce;
  if (p.ammo > 0) --p.ammo;
  sink.push(SimEvent{EventKind::BoomerangThrown, s.tick, p.id, id, 0, 0});
  if (spawned.hitWall) {
    sink.push(SimEvent{.kind = EventKind::BoomerangBounced, .tick = s.tick, .entity = id});
  }
}

}  // namespace

bool throwCanActivate(const WorldState& /*s*/, const Player& p, const AbilityDef& /*def*/) {
  return p.ammo > 0;
}

// NOLINTNEXTLINE(misc-const-correctness): parameter types fixed by AbilityBehavior
void throwOnActivate(WorldState& /*s*/, Player& p, AbilityState& st, const AbilityDef& /*def*/,
                     const MatchContent& /*content*/, EventSink& /*sink*/) {
  st.dir = Vec2::fromAngle(p.intent.aim);
}

void throwOnTick(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
                 const MatchContent& /*content*/, EventSink& /*sink*/) {}

// NOLINTNEXTLINE(misc-const-correctness): parameter types fixed by AbilityBehavior
void throwOnPhaseChange(WorldState& s, Player& p, AbilityState& st, const AbilityDef& /*def*/,
                        AbilityPhase from, AbilityPhase to, const MatchContent& content,
                        EventSink& sink) {
  if (from == AbilityPhase::Startup && to == AbilityPhase::Active) {
    spawnBanana(s, p, st, content, sink);
  }
}

void throwOnEnd(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
                const MatchContent& /*content*/, EventSink& /*sink*/) {}

}  // namespace kfm::sim
