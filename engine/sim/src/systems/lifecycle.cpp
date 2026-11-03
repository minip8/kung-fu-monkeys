// LifecycleSystem (architecture §5.3 step 9, §5.7, §5.8): despawns players
// tagged Eliminated (already reported by InteractionSystem), turns grounded
// boomerangs into banana pickups at their position, and runs the arena's
// power-up spawners.
//
// Spawners (§5.8) fire only while the match phase is Playing, on the ticks
// where ticksInPhase (0 on the first Playing tick) equals firstTicks +
// k × intervalTicks (k >= 0; intervalTicks 0 = only once), in arena order,
// after the bananas. A spawner with maxAlive of its own pickups lying in the
// arena skips that beat. The power-up is drawn from its set with the world
// RNG (one rng.below() per spawn, none otherwise), so an arena without
// spawners never touches the RNG here. The spawner's timeline is a function
// of ticksInPhase, not state of its own; a pickup remembers its spawner.
#include "kfm/sim/systems.hpp"

#include <bit>
#include <cstdint>

namespace kfm::sim {
namespace {

[[nodiscard]] bool spawnerBeat(const PickupSpawner& sp, uint32_t t) {
  if (t < sp.firstTicks) return false;
  const uint32_t since = t - sp.firstTicks;
  return sp.intervalTicks == 0 ? since == 0 : since % sp.intervalTicks == 0;
}

[[nodiscard]] uint32_t aliveFrom(const WorldState& s, uint8_t spawner) {
  uint32_t n = 0;
  s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& k) {
    if (k.kind == PickupKind::PowerUp && k.spawner == spawner) ++n;
  });
  return n;
}

// The power-ups a spawner may draw: its set (0 = all) within the DB.
[[nodiscard]] uint32_t candidates(const PickupSpawner& sp, const ContentDB& db) {
  const uint32_t all = db.powerUpCount >= 32 ? UINT32_MAX : (uint32_t{1} << db.powerUpCount) - 1u;
  return sp.powerUpSet == 0 ? all : (uint32_t{sp.powerUpSet} & all);
}

// Index of the n-th set bit of `bits` (n < popcount(bits)).
[[nodiscard]] uint8_t nthSetBit(uint32_t bits, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    bits &= bits - 1u;  // drop the lowest set bit
  return static_cast<uint8_t>(std::countr_zero(bits));
}

void runSpawners(WorldState& s, const MatchContent& content, EventSink& sink) {
  if (s.rules.phase != MatchPhase::Playing) return;
  const ArenaDef& arena = content.arena;
  const uint32_t t = s.rules.ticksInPhase;
  for (uint8_t i = 0; i < arena.pickupSpawnerCount && i < kMaxPickupSpawners; ++i) {
    const PickupSpawner& sp = arena.pickupSpawners[i];
    if (!spawnerBeat(sp, t) || aliveFrom(s, i) >= sp.maxAlive) continue;
    const uint32_t set = candidates(sp, content.db);
    if (set == 0) continue;
    const EntityId id = s.pickups.spawn();
    if (!id.isValid()) return;  // pickup pool full: no spawner fires this tick
    const uint8_t def = nthSetBit(set, s.rng.below(static_cast<uint32_t>(std::popcount(set))));
    Pickup& pickup = s.pickups.items[id.index];
    pickup.kind = PickupKind::PowerUp;
    pickup.pos = sp.pos;
    pickup.powerUpIndex = def;
    pickup.spawner = i;
    sink.push(SimEvent{EventKind::PickupSpawned, s.tick, PlayerId::invalid(), id,
                       static_cast<uint32_t>(PickupKind::PowerUp), def});
  }
}

}  // namespace

void lifecycleSystem(WorldState& s, const MatchContent& content, EventSink& sink) {
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

  runSpawners(s, content, sink);
}

}  // namespace kfm::sim
