// Power-up pickup (prio 51, architecture §5.6, §5.8). A player touching a
// power-up pickup gains its def as a ModifierList entry for durationTicks;
// ModifierSystem resolves the grants from the same tick on. Like the banana
// pickup (prio 50) it ignores HitStun and dashing: any player still in the
// round takes it.
//
// - The same power-up again refreshes the existing entry's timer in place
//   (no second entry, slot order unchanged).
// - A full ModifierList (kMaxModifiers) declines: the pickup stays.
// - AmmoMax grants top the ammo up by their add on every pickup;
//   ModifierSystem then caps ammo at the new ammoMax (a refresh at full ammo
//   therefore adds nothing).
#include "kfm/sim/rules/handlers.hpp"

#include <algorithm>
#include <cstdint>

namespace kfm::sim {

bool grantPowerUp(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                  const Contact& /*contact*/, EventSink& sink) {
  Player* p = s.players.get(a);
  const Pickup* pickup = s.pickups.get(b);
  if (p == nullptr || pickup == nullptr || pickup->kind != PickupKind::PowerUp) return false;
  const uint8_t index = pickup->powerUpIndex;
  if (index >= content.db.powerUpCount) return false;
  const PowerUpDef& def = content.db.powerUps[index];

  ModifierList& mods = p->modifiers;
  ActiveModifier* entry = nullptr;
  for (uint8_t i = 0; i < mods.count && i < kMaxModifiers; ++i) {
    if (mods.items[i].powerUp == index) entry = &mods.items[i];
  }
  if (entry == nullptr) {
    if (mods.count >= kMaxModifiers) return false;
    entry = &mods.items[mods.count++];
    entry->powerUp = index;
  }
  entry->ticksLeft = def.durationTicks;

  int32_t ammo = p->ammo;
  for (uint8_t g = 0; g < def.grantCount && g < kMaxPowerUpGrants; ++g) {
    const PowerUpGrant& grant = def.grants[g];
    if (grant.kind == GrantKind::Stat && grant.stat.stat == StatKind::AmmoMax) {
      ammo += std::max(grant.stat.add.floorToInt(), int32_t{0});
    }
  }
  p->ammo = static_cast<uint8_t>(std::min<int32_t>(ammo, UINT8_MAX));

  s.pickups.despawn(b);
  sink.push(SimEvent{EventKind::PickupTaken, s.tick, p->id, b,
                     static_cast<uint32_t>(PickupKind::PowerUp), index});
  return true;
}

}  // namespace kfm::sim
