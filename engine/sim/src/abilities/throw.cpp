// Throw behaviour hooks (architecture §5.5, §5.7). The aim direction is
// captured at activation; the banana spawns on the Startup -> Active
// transition (with the default startup of 2, on the second tick after the
// press), spending one ammo. A full boomerang pool makes the throw fizzle
// without spending ammo or emitting an event.
#include "kfm/sim/abilities/throw.hpp"

#include "systems/angle_of.hpp"

namespace kfm::sim {
namespace {

// The thrower's live Projectile grants, in slot order, snapshotted onto the
// banana (§5.8): muls multiply, damage adds (saturating), pierce ors.
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
      b.pierce = static_cast<uint8_t>(b.pierce | grant.projectile.pierce);
    }
  }
  b.damage = static_cast<uint8_t>(damage < UINT8_MAX ? damage : UINT8_MAX);
}

void spawnBanana(WorldState& s, Player& p, const AbilityState& st, const MatchContent& content,
                 EventSink& sink) {
  const EntityId id = s.boomerangs.spawn();
  if (!id.isValid()) return;  // pool full: the throw fizzles, ammo is kept
  const BoomerangDef& def = content.db.boomerang;
  Boomerang& b = s.boomerangs.items[id.index];
  b.owner = p.id;
  b.phase = BoomerangPhase::Outbound;
  b.transform.pos = p.transform.pos + st.dir * def.spawnOffset;
  b.transform.prevPos = b.transform.pos;
  b.damage = def.damage;
  b.pierce = def.pierce;
  applyProjectileModifiers(b, p, content.db);
  b.transform.vel = st.dir * (def.speed * b.speedMul);
  b.transform.facing = angleOf(st.dir);
  if (p.ammo > 0) --p.ammo;
  sink.push(SimEvent{EventKind::BoomerangThrown, s.tick, p.id, id, 0, 0});
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
