// LifecycleSystem (architecture §5.3 step 9, §5.7): despawns players tagged
// Eliminated (already reported by InteractionSystem) and turns grounded
// boomerangs into banana pickups at their position.
#include "kfm/sim/systems.hpp"

namespace kfm::sim {

void lifecycleSystem(WorldState& s, const MatchContent& /*content*/, EventSink& sink) {
  s.players.forEachAlive([&](EntityId id, const Player& p) {
    if (p.tags.has(Tag::Eliminated)) s.players.despawn(id);
  });

  // Iterates boomerangs and spawns pickups: a different pool, so the
  // forEachAlive no-spawn rule holds.
  s.boomerangs.forEachAlive([&](EntityId id, const Boomerang& b) {
    if (b.phase != BoomerangPhase::Grounded) return;
    const Vec2 pos = b.transform.pos;
    s.boomerangs.despawn(id);
    sink.push(SimEvent{EventKind::BoomerangGrounded, s.tick, PlayerId::invalid(), id, 0, 0});
    const EntityId pickupId = s.pickups.spawn();
    if (!pickupId.isValid()) return;  // pickup pool full: the banana vanishes
    Pickup& pickup = s.pickups.items[pickupId.index];
    pickup.kind = PickupKind::Banana;
    pickup.pos = pos;
    pickup.powerUpIndex = 0;
    sink.push(SimEvent{EventKind::PickupSpawned, s.tick, PlayerId::invalid(), pickupId, 0, 0});
  });
}

}  // namespace kfm::sim
