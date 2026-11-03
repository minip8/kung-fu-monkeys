// Shared damage step of the rule handlers (architecture §5.6 HP semantics):
// hp loss, HitStun, the hit event and PlayerEliminated at 0 hp. Used by
// damagePlayer (a banana hit) and dashAttack (a dash hit). Internal to
// engine/sim/src/rules.
#pragma once

#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/world_state.hpp"

#include <cstdint>

namespace kfm::sim {

// What landed the hit, for the hit event: its kind (PlayerHit or
// PlayerDashHit), the entity that hit (the banana or the attacking player)
// and the attacker's slot.
struct HitSource {
  EventKind event{EventKind::PlayerHit};
  EntityId entity{};
  uint32_t attackerSlot{0};
};

// Damages `victim` (whose pool id is victimEntity) by `damage` hits on `tick`.
void applyDamage(Tick tick, const MatchContent& content, Player& victim, EntityId victimEntity,
                 uint8_t damage, const HitSource& source, EventSink& sink);

}  // namespace kfm::sim
