// LifecycleSystem (architecture §5.3 step 9, §5.7): spawn protection and
// respawns (v0.23), despawns players tagged Eliminated (already reported by
// InteractionSystem) and turns grounded boomerangs into banana pickups at
// their position.
//
// Respawns are driven by MatchRules' timers, which only modes that respawn
// (deathmatch) set, so the code is the same for every mode: during Playing a
// dead present slot with a pending respawnTicks counts down and respawns at
// 0 (modes/mode_flow.hpp: farthest spawn point from the other monkeys,
// Tag::Invulnerable for respawnInvulnTicks); an alive player's invulnTicks
// counts down and clears the tag at 0. The countdown runs before this tick's
// despawns, so a player eliminated on tick T respawns on tick
// T + respawnTicks and is protected for the interactions of the next
// respawnInvulnTicks ticks.
#include "kfm/sim/systems.hpp"
#include "modes/mode_flow.hpp"

namespace kfm::sim {
namespace {

void respawnTimers(WorldState& s, const MatchContent& content, EventSink& sink) {
  MatchRules& r = s.rules;
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const PlayerId id{i};
    if (Player* p = s.playerBySlot(id)) {
      if (r.invulnTicks[i] == 0) continue;
      if (--r.invulnTicks[i] == 0) p->tags.clear(Tag::Invulnerable);
      continue;
    }
    if (r.respawnTicks[i] == 0) continue;
    if (!maskHas(r.present, id)) {
      r.respawnTicks[i] = 0;  // left while dead: no respawn
      continue;
    }
    if (--r.respawnTicks[i] == 0) respawnPlayer(s, content, id, sink);
  }
}

}  // namespace

void lifecycleSystem(WorldState& s, const MatchContent& content, EventSink& sink) {
  if (s.rules.phase == MatchPhase::Playing) respawnTimers(s, content, sink);

  s.players.forEachAlive([&](EntityId id, const Player& p) {
    if (!p.tags.has(Tag::Eliminated)) return;
    s.rules.invulnTicks[id.index] = 0;
    s.players.despawn(id);
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
