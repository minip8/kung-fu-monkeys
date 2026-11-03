// ModifierSystem (architecture §5.3 step 8, §5.6, §5.8): derived Stats, HP
// clamp on maxHp change, modifier timers, HitStun expiry.
//
// Per player, in this order:
//   1. Stats from content.db.player, then every live modifier in slot order:
//      moveSpeed = base × Π mul × hazard + Σ add (never negative), where
//      hazard is the product of the speedMul of every Slow hazard holding the
//      player's centre (v0.22, statics::speedMulAt; outside them it is 1 and
//      not multiplied in at all); maxHp and ammoMax are
//      integer stats, add-only (mul is ignored), clamped to [1, 255] and
//      [0, 255].
//   2. HP clamp against the previous tick's maxHp: a rise raises hp by the
//      same amount; a fall gives hp = max(1, min(hp, maxHp)), so losing a
//      modifier never eliminates. A player already at hp 0 is left alone.
//      ammo is clamped to ammoMax.
//   3. Modifier timers: every modifier contributed to this tick's stats, then
//      ticksLeft-- (saturating); the ones that reach 0 are dropped, compacting
//      the list in slot order. A modifier with ticksLeft N therefore applies
//      on exactly max(N, 1) ticks.
//   4. hitStunTicks-- when positive; Tag::HitStun is cleared whenever the
//      counter is 0 afterwards.
#include "kfm/sim/systems.hpp"
#include "systems/statics.hpp"

#include <algorithm>
#include <cstdint>

namespace kfm::sim {
namespace {

[[nodiscard]] Stats deriveStats(const PlayerDef& base, const ModifierList& mods,
                                Fixed hazardSpeedMul) {
  Fixed mul = Fixed::one();
  Fixed add{};
  int32_t maxHp = base.maxHp;
  int32_t ammoMax = base.ammoMax;
  for (uint8_t i = 0; i < mods.count && i < kMaxModifiers; ++i) {
    const StatModifier& m = mods.items[i];
    switch (m.stat) {
      case StatKind::MoveSpeed:
        mul = mul * m.mul;
        add = add + m.add;
        break;
      case StatKind::MaxHp:
        maxHp += m.add.floorToInt();
        break;
      case StatKind::AmmoMax:
        ammoMax += m.add.floorToInt();
        break;
    }
  }
  if (hazardSpeedMul != Fixed::one()) mul = mul * hazardSpeedMul;
  Stats out;
  out.moveSpeed = max(base.moveSpeed * mul + add, Fixed::zero());
  out.maxHp = static_cast<uint8_t>(std::clamp<int32_t>(maxHp, 1, UINT8_MAX));
  out.ammoMax = static_cast<uint8_t>(std::clamp<int32_t>(ammoMax, 0, UINT8_MAX));
  return out;
}

void clampHp(Player& p, uint8_t oldMaxHp, uint8_t newMaxHp) {
  if (p.hp == 0) return;  // eliminated this tick; a stat change never revives
  if (newMaxHp > oldMaxHp) {
    const int32_t raised = int32_t{p.hp} + (int32_t{newMaxHp} - int32_t{oldMaxHp});
    p.hp = static_cast<uint8_t>(std::min<int32_t>(raised, newMaxHp));
  } else if (newMaxHp < oldMaxHp) {
    p.hp = std::max<uint8_t>(1, std::min(p.hp, newMaxHp));
  }
}

void tickModifiers(ModifierList& mods) {
  uint8_t kept = 0;
  for (uint8_t i = 0; i < mods.count && i < kMaxModifiers; ++i) {
    StatModifier m = mods.items[i];
    if (m.ticksLeft > 0) --m.ticksLeft;
    if (m.ticksLeft > 0) mods.items[kept++] = m;
  }
  for (uint8_t i = kept; i < kMaxModifiers; ++i) {
    mods.items[i] = StatModifier{};  // canonical empty slots, as a decoded snapshot has
  }
  mods.count = kept;
}

}  // namespace

void modifierSystem(WorldState& s, const MatchContent& content) {
  s.players.forEachAlive([&](EntityId /*id*/, Player& p) {
    const uint8_t oldMaxHp = p.stats.maxHp;
    p.stats = deriveStats(content.db.player, p.modifiers,
                          statics::speedMulAt(content.arena, p.transform.pos));
    clampHp(p, oldMaxHp, p.stats.maxHp);
    p.ammo = std::min(p.ammo, p.stats.ammoMax);
    tickModifiers(p.modifiers);
    if (p.hitStunTicks > 0) --p.hitStunTicks;
    if (p.hitStunTicks == 0) p.tags.clear(Tag::HitStun);
  });
}

}  // namespace kfm::sim
