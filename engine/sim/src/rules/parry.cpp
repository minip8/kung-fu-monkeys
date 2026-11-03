// Parry (prio 15, architecture §5.6, §5.11, v0.21):
// Player[Parrying, !HitStun] × Boomerang[InFlight, !own]. Judged against the
// press sub-tick: parried iff 0 <= hitTime - (pressTime + startupTicks) <
// activeTicks, where pressTime = activatedTick + activatedSubTick / 256 and
// hitTime = tick + tHit. Outside the window the handler declines and the scan
// falls through (to the shield, then damage): a mistimed parry is a hit.
//
// A parried banana becomes the parrier's: owner swapped, phase Outbound with
// a fresh outbound timer, at the contact point, heading for the thrower's
// current position (straight back along its path when the thrower is gone or
// stands on the contact point), at the banana's own speed (its projectile
// modifiers stay). As the parrier's own outbound banana it passes through the
// parrier and returns to it later, so a parry gains a banana.
#include "kfm/sim/rules/handlers.hpp"
#include "kfm/sim/systems.hpp"
#include "systems/angle_of.hpp"

namespace kfm::sim {

bool parryBoomerang(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                    const Contact& contact, EventSink& sink) {
  Player* p = s.players.get(a);
  Boomerang* banana = s.boomerangs.get(b);
  if (p == nullptr || banana == nullptr) return false;
  const AbilityState* slot = slotGranting(*p, content, Tag::Parrying);
  if (slot == nullptr) return false;
  const AbilityDef& def = content.db.abilityAt(slot->defIndex);
  const Fixed delta = Fixed::fromInt(s.tick.minus(slot->activatedTick)) + contact.tHit -
                      Fixed::fromRatio(slot->activatedSubTick, 256) -
                      Fixed::fromInt(def.startupTicks);
  if (delta < Fixed::zero() || delta >= Fixed::fromInt(def.activeTicks)) return false;

  const PlayerId thrower = banana->owner;
  Vec2 heading{};
  if (const Player* t = s.playerBySlot(thrower); t != nullptr && t != p) {
    heading = normalized(t->transform.pos - contact.point);
  }
  if (heading == Vec2::zero()) heading = normalized(-banana->transform.vel);
  if (heading == Vec2::zero()) heading = contact.normal;  // away from the parrier
  banana->owner = p->id;
  banana->phase = BoomerangPhase::Outbound;
  banana->ticksInPhase = 0;
  banana->transform.pos = contact.point;
  banana->transform.prevPos = contact.point;
  banana->transform.vel = heading * (content.db.boomerang.speed * banana->speedMul);
  if (banana->transform.vel != Vec2::zero()) banana->transform.facing = angleOf(heading);
  sink.push(SimEvent{EventKind::BoomerangParried, s.tick, p->id, b, thrower.value, 0});
  return true;
}

}  // namespace kfm::sim
