// Damage on hit (prio 30, architecture §5.6 HP semantics, §5.7). hp and damage
// count hits; the victim gets HitStun so a banana overlapping it across ticks
// lands once, and is tagged Eliminated at 0 hp (LifecycleSystem despawns it at
// the end of the tick). The banana always flies on through its victim (v0.28).
#include "kfm/sim/rules/handlers.hpp"
#include "modes/mode_flow.hpp"
#include "rules/apply_damage.hpp"

namespace kfm::sim {

void applyDamage(WorldState& s, const MatchContent& content, Player& victim, EntityId victimEntity,
                 uint8_t damage, const HitSource& source, EventSink& sink) {
  victim.hp = victim.hp > damage ? static_cast<uint8_t>(victim.hp - damage) : 0;
  victim.hitStunTicks = content.db.player.hitStunTicks;
  victim.tags.set(Tag::HitStun);
  sink.push(
      SimEvent{source.event, s.tick, victim.id, source.entity, source.attackerSlot, victim.hp});
  if (victim.hp == 0) {
    victim.tags.set(Tag::Eliminated);
    sink.push(SimEvent{EventKind::PlayerEliminated, s.tick, victim.id, victimEntity,
                       source.attackerSlot, 0});
    const PlayerId killer = source.attackerSlot < kMaxPlayers
                                ? PlayerId{static_cast<uint8_t>(source.attackerSlot)}
                                : PlayerId::invalid();
    reportElimination(s, content, victim.id, killer, sink);
  }
}

bool damagePlayer(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                  const Contact& /*contact*/, EventSink& sink) {
  Player* victim = s.players.get(a);
  const Boomerang* banana = s.boomerangs.get(b);
  if (victim == nullptr || banana == nullptr) return false;

  applyDamage(s, content, *victim, a, banana->damage,
              HitSource{EventKind::PlayerHit, b, banana->owner.value}, sink);
  return true;
}

}  // namespace kfm::sim
