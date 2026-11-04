// Pickup taken (prio 50, architecture §5.6). A grounded banana restores one
// ammo; a player already at ammoMax leaves it lying where it is. Power-up
// pickups are row 51 (rules/powerup.cpp).
#include "kfm/sim/modes/registry.hpp"
#include "kfm/sim/rules/handlers.hpp"

namespace kfm::sim {

bool takePickup(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                const Contact& /*contact*/, EventSink& sink) {
  Player* p = s.players.get(a);
  const Pickup* pickup = s.pickups.get(b);
  if (p == nullptr || pickup == nullptr || pickup->kind != PickupKind::Banana) return false;
  if (p->ammo >= p->stats.ammoMax) return false;
  ++p->ammo;
  const Pickup taken = *pickup;  // the hook sees it after the despawn
  const PlayerId player = p->id;
  s.pickups.despawn(b);
  sink.push(SimEvent{EventKind::PickupTaken, s.tick, player, b, 0, 0});
  modeBehaviorOf(content).onPickupTaken(s, content, player, taken, sink);  // v0.31
  return true;
}

}  // namespace kfm::sim
