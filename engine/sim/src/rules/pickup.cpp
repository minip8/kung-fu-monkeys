// Pickup taken (prio 50, architecture §5.6). A grounded banana restores one
// ammo; a player already at ammoMax leaves it lying where it is. v0.57: a
// stacked pickup (Pickup::count, a full pool's overflow) gives as many as fit
// and keeps the rest lying there. Power-up pickups are row 51
// (rules/powerup.cpp). canTakePickup is the row's contend test: monkeys with
// room that touch one pickup on one tick take it in a drawn order
// (InteractionSystem), a stack as far as it fits for each in turn.
#include "kfm/sim/modes/registry.hpp"
#include "kfm/sim/rules/handlers.hpp"

#include <algorithm>

namespace kfm::sim {

bool canTakePickup(const WorldState& s, const MatchContent& /*content*/, EntityId a, EntityId b) {
  const Player* p = s.players.get(a);
  const Pickup* pickup = s.pickups.get(b);
  return p != nullptr && pickup != nullptr && pickup->kind == PickupKind::Banana &&
         p->ammo < p->stats.ammoMax;
}

bool takePickup(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                const Contact& /*contact*/, EventSink& sink) {
  if (!canTakePickup(s, content, a, b)) return false;
  Player* p = s.players.get(a);
  const Pickup* pickup = s.pickups.get(b);
  const uint8_t count = std::max<uint8_t>(pickup->count, 1);
  const auto take = std::min<uint8_t>(count, static_cast<uint8_t>(p->stats.ammoMax - p->ammo));
  p->ammo = static_cast<uint8_t>(p->ammo + take);
  const Pickup taken = *pickup;  // the hook sees it after the despawn
  const PlayerId player = p->id;
  if (take < count) {
    s.pickups.items[b.index].count = static_cast<uint8_t>(count - take);  // the rest lie on
  } else {
    s.pickups.despawn(b);
  }
  sink.push(SimEvent{EventKind::PickupTaken, s.tick, player, b, 0, 0});
  modeBehaviorOf(content).onPickupTaken(s, content, player, taken, sink);  // v0.31
  return true;
}

}  // namespace kfm::sim
