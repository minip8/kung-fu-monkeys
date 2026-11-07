// Bananas never disappear (rules/bananas.hpp, architecture §5.6, v0.57).
#include "rules/bananas.hpp"

#include "systems/statics.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace kfm::sim {

EntityId layBananaPickup(WorldState& s, Vec2 pos, EventSink& sink) {
  const EntityId id = s.pickups.spawn();
  if (id.isValid()) {
    Pickup& pickup = s.pickups.items[id.index];
    pickup.kind = PickupKind::Banana;
    pickup.pos = pos;
    pickup.powerUpIndex = 0;
    pickup.count = 1;
    sink.push(SimEvent{EventKind::PickupSpawned, s.tick, PlayerId::invalid(), id, 0, 0});
    return id;
  }
  // The pool is full: stack it on the nearest banana pickup.
  EntityId nearest = EntityId::invalid();
  uint64_t best = std::numeric_limits<uint64_t>::max();
  s.pickups.forEachAlive([&](EntityId kid, const Pickup& k) {
    if (k.kind != PickupKind::Banana || k.count == UINT8_MAX) return;
    const uint64_t d = lengthSqRaw(k.pos - pos);
    if (d < best) {
      best = d;
      nearest = kid;
    }
  });
  if (Pickup* k = s.pickups.get(nearest)) ++k->count;
  return nearest;
}

void dropBanana(WorldState& s, const MatchContent& content, Vec2 pos, PlayerId owner,
                EventSink& sink) {
  const ArenaDef& arena = content.arena;
  const Fixed keep = content.db.pickup.radius;
  const Vec2 lim{max(arena.halfExtents.x - keep, Fixed::zero()),
                 max(arena.halfExtents.y - keep, Fixed::zero())};
  const Vec2 at{clamp(pos.x, -lim.x, lim.x), clamp(pos.y, -lim.y, lim.y)};
  if (discOverPits(arena, at, content.db.boomerang.radius)) {
    const EntityId id = s.boomerangs.spawn();
    if (!id.isValid()) return;  // the boomerang pool full: it is in the pit all the same
    Boomerang& b = s.boomerangs.items[id.index];
    b.owner = owner;
    b.phase = BoomerangPhase::Sinking;
    b.transform.pos = at;
    b.transform.prevPos = at;
    b.damage = content.db.boomerang.damage;
    sink.push(SimEvent{EventKind::BoomerangFell, s.tick, owner, id, 0, 0});
    return;
  }
  const EntityId pickup = layBananaPickup(s, at, sink);
  sink.push(SimEvent{EventKind::BananaDropped, s.tick, owner, pickup, 0, 0});
}

void dropHeldBananas(WorldState& s, const MatchContent& content, Player& p, uint8_t n,
                     EventSink& sink) {
  n = std::min(n, p.ammo);
  p.ammo = static_cast<uint8_t>(p.ammo - n);
  const Vec2 centre = p.transform.pos;
  for (uint8_t k = 0; k < n; ++k) {
    Vec2 at = centre;
    if (n > 1) {
      const auto bam = static_cast<uint16_t>((uint32_t{k} * 65536u) / n);
      at = centre + Vec2::fromAngle(Angle::fromBam(bam), kBananaScatterRadius);
      if (statics::overlapsAnyWall(content.arena, at, content.db.pickup.radius)) at = centre;
    }
    dropBanana(s, content, at, p.id, sink);
  }
}

void dropBananasOf(WorldState& s, PlayerId owner) {
  s.boomerangs.forEachAlive([&](EntityId /*id*/, Boomerang& banana) {
    if (banana.owner != owner) return;
    if (banana.phase != BoomerangPhase::Outbound && banana.phase != BoomerangPhase::Returning) {
      return;
    }
    banana.phase = BoomerangPhase::Grounded;
    banana.ticksInPhase = 0;
    banana.transform.vel = Vec2::zero();
  });
}

}  // namespace kfm::sim
