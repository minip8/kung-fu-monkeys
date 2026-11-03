// Throw behaviour hooks (architecture §5.5, §5.7). The aim direction is
// captured at activation; the banana spawns on the Startup -> Active
// transition (with the default startup of 2, on the second tick after the
// press), spending one ammo. A full boomerang pool makes the throw fizzle
// without spending ammo or emitting an event.
#include "kfm/sim/abilities/throw.hpp"

namespace kfm::sim {
namespace {

// Heading of a non-zero vector as a binary angle, by successive approximation
// against the sine table (no atan2 in fixed point). Accurate to a few BAM,
// which is far below anything visible or gameplay-relevant.
[[nodiscard]] Angle headingOf(Vec2 v) {
  if (v == Vec2::zero()) return Angle{};
  // Pick the half-turn containing v, then halve the bracket 15 times.
  uint16_t a = (v.y < Fixed::zero() || (v.y == Fixed::zero() && v.x < Fixed::zero())) ? Angle::kHalf
                                                                                      : uint16_t{0};
  for (uint16_t stepBam = Angle::kQuarter; stepBam != 0; stepBam >>= 1) {
    const Angle mid = Angle::fromBam(static_cast<uint16_t>(a + stepBam));
    if (cross(Vec2::fromAngle(mid), v) >= Fixed::zero()) a = mid.bam;
  }
  return Angle::fromBam(a);
}

void spawnBanana(WorldState& s, Player& p, const AbilityState& st, const ContentDB& content,
                 EventSink& sink) {
  const EntityId id = s.boomerangs.spawn();
  if (!id.isValid()) return;  // pool full: the throw fizzles, ammo is kept
  const BoomerangDef& def = content.boomerang;
  Boomerang& b = s.boomerangs.items[id.index];
  b.owner = p.id;
  b.phase = BoomerangPhase::Outbound;
  b.transform.pos = p.transform.pos + st.dir * def.spawnOffset;
  b.transform.prevPos = b.transform.pos;
  b.transform.vel = st.dir * def.speed;
  b.transform.facing = headingOf(st.dir);
  b.damage = def.damage;
  b.pierce = def.pierce;
  if (p.ammo > 0) --p.ammo;
  sink.push(SimEvent{EventKind::BoomerangThrown, s.tick, p.id, id, 0, 0});
}

}  // namespace

bool throwCanActivate(const WorldState& /*s*/, const Player& p, const AbilityDef& /*def*/) {
  return p.ammo > 0;
}

// NOLINTNEXTLINE(misc-const-correctness): parameter types fixed by AbilityBehavior
void throwOnActivate(WorldState& /*s*/, Player& p, AbilityState& st, const AbilityDef& /*def*/,
                     const ContentDB& /*content*/, EventSink& /*sink*/) {
  st.dir = Vec2::fromAngle(p.intent.aim);
}

void throwOnTick(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
                 const ContentDB& /*content*/, EventSink& /*sink*/) {}

// NOLINTNEXTLINE(misc-const-correctness): parameter types fixed by AbilityBehavior
void throwOnPhaseChange(WorldState& s, Player& p, AbilityState& st, const AbilityDef& /*def*/,
                        AbilityPhase from, AbilityPhase to, const ContentDB& content,
                        EventSink& sink) {
  if (from == AbilityPhase::Startup && to == AbilityPhase::Active) {
    spawnBanana(s, p, st, content, sink);
  }
}

void throwOnEnd(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
                const ContentDB& /*content*/, EventSink& /*sink*/) {}

}  // namespace kfm::sim
