// Damage on hit (prio 30, architecture §5.6 HP semantics, §5.7). hp and damage
// count hits; the victim gets HitStun so a piercing banana overlapping across
// ticks lands once, and is tagged Eliminated at 0 hp (LifecycleSystem despawns
// it at the end of the tick). The banana grounds at the contact point unless
// it pierces.
#include "kfm/sim/rules/handlers.hpp"
#include "rules/apply_damage.hpp"

namespace kfm::sim {

void applyDamage(Tick tick, const MatchContent& content, Player& victim, EntityId victimEntity,
                 uint8_t damage, const HitSource& source, EventSink& sink) {
  victim.hp = victim.hp > damage ? static_cast<uint8_t>(victim.hp - damage) : 0;
  victim.hitStunTicks = content.db.player.hitStunTicks;
  victim.tags.set(Tag::HitStun);
  sink.push(SimEvent{source.event, tick, victim.id, source.entity, source.attackerSlot, victim.hp});
  if (victim.hp == 0) {
    victim.tags.set(Tag::Eliminated);
    sink.push(SimEvent{EventKind::PlayerEliminated, tick, victim.id, victimEntity,
                       source.attackerSlot, 0});
  }
}

bool damagePlayer(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                  const Contact& contact, EventSink& sink) {
  Player* victim = s.players.get(a);
  Boomerang* banana = s.boomerangs.get(b);
  if (victim == nullptr || banana == nullptr) return false;

  applyDamage(s.tick, content, *victim, a, banana->damage,
              HitSource{EventKind::PlayerHit, b, banana->owner.value}, sink);

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
