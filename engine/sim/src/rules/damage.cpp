// Damage on hit (prio 30, architecture §5.6 HP semantics, §5.7). hp and damage
// count hits; the victim gets HitStun so a piercing banana overlapping across
// ticks lands once, and is tagged Eliminated at 0 hp (LifecycleSystem despawns
// it at the end of the tick). The banana grounds at the contact point unless
// it pierces.
#include "kfm/sim/rules/handlers.hpp"

namespace kfm::sim {

bool damagePlayer(WorldState& s, const ContentDB& content, EntityId a, EntityId b,
                  const Contact& contact, EventSink& sink) {
  Player* victim = s.players.get(a);
  Boomerang* banana = s.boomerangs.get(b);
  if (victim == nullptr || banana == nullptr) return false;

  victim->hp = victim->hp > banana->damage ? static_cast<uint8_t>(victim->hp - banana->damage) : 0;
  victim->hitStunTicks = content.player.hitStunTicks;
  victim->tags.set(Tag::HitStun);
  const uint32_t attacker = banana->owner.value;
  sink.push(SimEvent{EventKind::PlayerHit, s.tick, victim->id, b, attacker, victim->hp});
  if (victim->hp == 0) {
    victim->tags.set(Tag::Eliminated);
    sink.push(SimEvent{EventKind::PlayerEliminated, s.tick, victim->id, a, attacker, 0});
  }

  if (banana->pierce == 0) {
    banana->phase = BoomerangPhase::Grounded;
    banana->ticksInPhase = 0;
    banana->transform.pos = contact.point;
    banana->transform.prevPos = contact.point;
    banana->transform.vel = Vec2::zero();
  }
  return true;
}

}  // namespace kfm::sim
