// Throw behaviour hooks (architecture §5.5, §5.7). The aim direction is
// captured at activation; the banana spawns on the Startup -> Active
// transition (with a startup of 2, on the second tick after the press),
// spending one ammo. A full boomerang pool makes the throw fizzle without
// spending ammo or emitting an event. v0.48: a charging def (throwCharges)
// holds its Startup while the button is down, takes the aim again on the
// release and scales the banana's speed by throwChargeMul of the ticks held
// past the startup.
#include "kfm/sim/abilities/throw.hpp"

#include "systems/angle_of.hpp"
#include "systems/statics.hpp"

namespace kfm::sim {
namespace {

// The thrower's live Projectile grants, in slot order, snapshotted onto the
// banana (§5.8): muls multiply, damage adds (saturating).
void applyProjectileModifiers(Boomerang& b, const Player& p, const ContentDB& db) {
  int32_t damage = b.damage;
  for (uint8_t i = 0; i < p.modifiers.count && i < kMaxModifiers; ++i) {
    const PowerUpDef& def = db.powerUpAt(p.modifiers.items[i].powerUp);
    for (uint8_t g = 0; g < def.grantCount && g < kMaxPowerUpGrants; ++g) {
      const PowerUpGrant& grant = def.grants[g];
      if (grant.kind != GrantKind::Projectile) continue;
      b.speedMul = b.speedMul * grant.projectile.speedMul;
      b.radiusMul = b.radiusMul * grant.projectile.radiusMul;
      damage += grant.projectile.damageAdd;
    }
  }
  b.damage = static_cast<uint8_t>(damage < UINT8_MAX ? damage : UINT8_MAX);
}

void spawnBanana(WorldState& s, Player& p, const AbilityState& st, Fixed chargeMul,
                 const MatchContent& content, EventSink& sink) {
  const EntityId id = s.boomerangs.spawn();
  if (!id.isValid()) return;  // pool full: the throw fizzles, ammo is kept
  const BoomerangDef& def = content.db.boomerang;
  Boomerang& b = s.boomerangs.items[id.index];
  b.owner = p.id;
  b.phase = BoomerangPhase::Outbound;
  b.damage = def.damage;
  applyProjectileModifiers(b, p, content.db);
  b.speedMul = b.speedMul * chargeMul;
  // The banana travels from the thrower's centre to its spawn point through
  // the interior walls (v0.22), so a throw at a wall bounces off it instead
  // of spawning inside or behind it. The outer box is left to
  // ProjectileSystem as in M1. Speed and radius carry the thrower's
  // projectile modifiers (v0.20, §5.8).
  b.transform.vel = st.dir * (def.speed * b.speedMul);
  const statics::MoveResult spawned =
      statics::moveCircle(content.arena, p.transform.pos, st.dir * def.spawnOffset, b.transform.vel,
                          statics::MoveParams{.radius = def.radius * b.radiusMul,
                                              .response = statics::Response::Bounce,
                                              .restitution = def.restitution,
                                              .outerBox = false});
  b.transform.pos = spawned.pos;
  b.transform.prevPos = b.transform.pos;
  b.transform.facing = b.transform.vel != Vec2::zero() && spawned.hitWall ? angleOf(b.transform.vel)
                                                                          : angleOf(st.dir);
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
void throwOnPhaseChange(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                        AbilityPhase from, AbilityPhase to, const MatchContent& content,
                        EventSink& sink) {
  if (from == AbilityPhase::Startup && to == AbilityPhase::Active) {
    Fixed chargeMul = Fixed::one();
    if (throwCharges(def)) {
      // The Startup ran from the activation tick to this one; settle has
      // already reset phaseTicks, so the hold is measured from the press.
      const int64_t ran = s.tick.minus(st.activatedTick);
      const int64_t charged = ran - int64_t{def.startupTicks};
      chargeMul = throwChargeMul(def, charged > 0 ? static_cast<uint32_t>(charged) : 0u);
      st.dir = Vec2::fromAngle(p.intent.aim);  // aimed while charging: the release's aim
    }
    spawnBanana(s, p, st, chargeMul, content, sink);
  }
}

bool throwHoldsPhase(const AbilityState& st, const AbilityDef& def, bool buttonHeld) {
  return buttonHeld && st.phase == AbilityPhase::Startup && throwCharges(def);
}

void throwOnEnd(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
                const MatchContent& /*content*/, EventSink& /*sink*/) {}

}  // namespace kfm::sim
