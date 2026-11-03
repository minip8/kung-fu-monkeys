// Pickup taken (prio 50, architecture §5.6). A grounded banana restores one
// ammo; a player already at ammoMax leaves it lying where it is.
#include "kfm/sim/rules/handlers.hpp"

namespace kfm::sim {

bool takePickup(WorldState& s, const ContentDB& /*content*/, EntityId a, EntityId b,
                const Contact& /*contact*/, EventSink& sink) {
  Player* p = s.players.get(a);
  if (p == nullptr || !s.pickups.isAlive(b)) return false;
  if (p->ammo >= p->stats.ammoMax) return false;
  ++p->ammo;
  s.pickups.despawn(b);
  sink.push(SimEvent{EventKind::PickupTaken, s.tick, p->id, b, 0, 0});
  return true;
}

}  // namespace kfm::sim
