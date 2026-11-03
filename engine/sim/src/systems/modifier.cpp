// ModifierSystem (architecture §5.3 step 8, §5.6, §5.8): power-up timers,
// derived Stats, HP clamp on maxHp change, ability overrides, granted tags,
// HitStun expiry.
//
// Per player, in this order:
//   1. Expiry: entries whose ticksLeft is 0 are dropped, compacting the list
//      in slot order (step 7 below counted them down on earlier ticks).
//   2. Stats from content.db.player, then every grant of every live entry in
//      slot order: moveSpeed = base × Π mul + Σ add (never negative); maxHp
//      and ammoMax are integer stats, add-only (mul is ignored), clamped to
//      [1, 255] and [0, 255].
//   3. HP clamp against the previous tick's maxHp: a rise raises hp by the
//      same amount; a fall gives hp = max(1, min(hp, maxHp)), so losing a
//      modifier never eliminates. A player already at hp 0 is left alone.
//      ammo is clamped to ammoMax.
//   4. Ability overrides: each slot's def is the loadout's, unless a live
//      AbilityOverride names another (the last one in slot order wins). A
//      slot switches only while Idle (a running dash or throw finishes on
//      the def it started with); on the switch its remaining cooldown is
//      capped at the new def's cooldownTicks.
//   5. Tags: every tag some power-up def can grant is cleared, then the live
//      entries' Tag grants are set.
//   6. hitStunTicks-- when positive; Tag::HitStun is cleared whenever the
//      counter is 0 afterwards.
//   7. Timers: every live entry's ticksLeft-- (saturating).
// An entry added on tick T (InteractionSystem, step 7 of the pipeline) with
// ticksLeft D (the def's durationTicks, >= 1) thus shapes the stats, tags
// and slots of the state after ticks T .. T+D-1 and is dropped on tick T+D;
// the list, the stats and the tags always agree between ticks.
#include "kfm/sim/systems.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace kfm::sim {
namespace {

void dropExpired(ModifierList& mods) {
  uint8_t kept = 0;
  for (uint8_t i = 0; i < mods.count && i < kMaxModifiers; ++i) {
    if (mods.items[i].ticksLeft > 0) mods.items[kept++] = mods.items[i];
  }
  for (uint8_t i = kept; i < kMaxModifiers; ++i) {
    mods.items[i] = ActiveModifier{};  // canonical empty slots, as a decoded snapshot has
  }
  mods.count = kept;
}

void countDown(ModifierList& mods) {
  for (uint8_t i = 0; i < mods.count && i < kMaxModifiers; ++i) {
    if (mods.items[i].ticksLeft > 0) --mods.items[i].ticksLeft;
  }
}

// Calls f(grant) for every grant of every live entry, in slot order.
template <class F>
void forEachGrant(const ContentDB& db, const ModifierList& mods, F&& f) {
  for (uint8_t i = 0; i < mods.count && i < kMaxModifiers; ++i) {
    const PowerUpDef& def = db.powerUpAt(mods.items[i].powerUp);
    for (uint8_t g = 0; g < def.grantCount && g < kMaxPowerUpGrants; ++g)
      f(def.grants[g]);
  }
}

[[nodiscard]] Stats deriveStats(const ContentDB& db, const ModifierList& mods) {
  const PlayerDef& base = db.player;
  Fixed mul = Fixed::one();
  Fixed add{};
  int32_t maxHp = base.maxHp;
  int32_t ammoMax = base.ammoMax;
  forEachGrant(db, mods, [&](const PowerUpGrant& g) {
    if (g.kind != GrantKind::Stat) return;
    switch (g.stat.stat) {
      case StatKind::MoveSpeed:
        mul = mul * g.stat.mul;
        add = add + g.stat.add;
        break;
      case StatKind::MaxHp:
        maxHp += g.stat.add.floorToInt();
        break;
      case StatKind::AmmoMax:
        ammoMax += g.stat.add.floorToInt();
        break;
    }
  });
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

void applyAbilityOverrides(Player& p, const ContentDB& db) {
  std::array<uint8_t, kAbilitySlots> want = db.player.abilities;
  forEachGrant(db, p.modifiers, [&](const PowerUpGrant& g) {
    if (g.kind == GrantKind::AbilityOverride && g.slot < kAbilitySlots) want[g.slot] = g.ability;
  });
  for (uint8_t slot = 0; slot < kAbilitySlots; ++slot) {
    AbilityState& st = p.abilities[slot];
    if (st.defIndex == want[slot] || st.phase != AbilityPhase::Idle) continue;
    st.defIndex = want[slot];
    st.cooldownTicks = std::min(st.cooldownTicks, db.abilityAt(want[slot]).cooldownTicks);
  }
}

// Every tag a power-up in the DB can grant (what step 5 owns).
[[nodiscard]] Tags grantableTags(const ContentDB& db) {
  Tags owned{};
  for (uint8_t i = 0; i < db.powerUpCount && i < kMaxPowerUps; ++i) {
    const PowerUpDef& def = db.powerUps[i];
    for (uint8_t g = 0; g < def.grantCount && g < kMaxPowerUpGrants; ++g) {
      if (def.grants[g].kind == GrantKind::Tag) owned.set(def.grants[g].tag);
    }
  }
  owned.clear(Tag::HitStun);  // system-owned, never granted (the loader rejects them too)
  owned.clear(Tag::Eliminated);
  return owned;
}

void applyTags(Player& p, const ContentDB& db, Tags owned) {
  Tags granted{};
  forEachGrant(db, p.modifiers, [&](const PowerUpGrant& g) {
    if (g.kind == GrantKind::Tag) granted.set(g.tag);
  });
  p.tags.bits = (p.tags.bits & ~owned.bits) | (granted.bits & owned.bits);
}

}  // namespace

void modifierSystem(WorldState& s, const MatchContent& content) {
  const ContentDB& db = content.db;
  const Tags owned = grantableTags(db);
  s.players.forEachAlive([&](EntityId /*id*/, Player& p) {
    dropExpired(p.modifiers);
    const uint8_t oldMaxHp = p.stats.maxHp;
    p.stats = deriveStats(db, p.modifiers);
    clampHp(p, oldMaxHp, p.stats.maxHp);
    p.ammo = std::min(p.ammo, p.stats.ammoMax);
    applyAbilityOverrides(p, db);
    applyTags(p, db, owned);
    if (p.hitStunTicks > 0) --p.hitStunTicks;
    if (p.hitStunTicks == 0) p.tags.clear(Tag::HitStun);
    countDown(p.modifiers);
  });
}

}  // namespace kfm::sim
