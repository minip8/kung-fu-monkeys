// Flag taken (prio 52, architecture §5.3, §5.6, v0.33). A monkey that holds
// no flag (the row forbids Tag::Carrying) touching a flag of another team
// takes it: the pickup despawns, PickupTaken (a = Flag, b = the flag's team)
// and the mode's onPickupTaken, where capture makes the taker the carrier.
// A flag of the monkey's own team declines and stays where it lies. Like
// the other pickup rows it ignores HitStun and dashing.
#include "kfm/sim/modes/registry.hpp"
#include "kfm/sim/rules/handlers.hpp"

namespace kfm::sim {

bool takeFlag(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
              const Contact& /*contact*/, EventSink& sink) {
  Player* p = s.players.get(a);
  const Pickup* pickup = s.pickups.get(b);
  if (p == nullptr || pickup == nullptr || pickup->kind != PickupKind::Flag) return false;
  if (p->tags.has(Tag::Carrying) || content.teamOf(p->id) == pickup->spawner) return false;
  const Pickup taken = *pickup;  // the hook sees it after the despawn
  const PlayerId player = p->id;
  s.pickups.despawn(b);
  sink.push(SimEvent{EventKind::PickupTaken, s.tick, player, b,
                     static_cast<uint32_t>(PickupKind::Flag), taken.spawner});
  modeBehaviorOf(content).onPickupTaken(s, content, player, taken, sink);
  return true;
}

}  // namespace kfm::sim
