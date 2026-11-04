// ModifierSystem (architecture §5.3 step 8, §5.6, §5.8): power-up timers,
// derived Stats, HP clamp on maxHp change, ability overrides, granted tags,
// HitStun expiry.
//
// Per player, in this order:
//   1. Expiry: entries whose ticksLeft is 0 are dropped, compacting the list
//      in slot order (step 7 below counted them down on earlier ticks).
//   2. Stats from content.db.player, then every grant of every live entry in
//      slot order: moveSpeed = base × Π mul × hazard + Σ add (never
//      negative), where hazard is the product of the speedMul of every Slow
//      hazard holding the player's centre (v0.22, statics::speedMulAt;
//      outside them it is 1 and not multiplied in at all); maxHp and ammoMax
//      are integer stats, add-only (mul is ignored), clamped to [1, 255] and
//      [0, 255].
//   3. HP clamp against the previous tick's maxHp: a rise raises hp by the
//      same amount; a fall gives hp = max(1, min(hp, maxHp)), so losing a
//      modifier never eliminates. A player already at hp 0 is left alone.
//      ammo is clamped to ammoMax.
//   4. Ability overrides: each slot's def is the loadout's (the mode's when it
//      has one, v0.21), unless a live
//      AbilityOverride names another (the last one in slot order wins). A
//      slot switches only while Idle (a running dash or throw finishes on
//      the def it started with); on the switch its remaining cooldown is
//      capped at the new def's cooldownTicks.
//   5. Tags: the bits granted last tick (Player::grantedTags) that are no
//      longer granted are cleared, unless an ability phase or the mode's
//      spawn protection holds them now; this tick's Tag grants are set and
//      recorded in grantedTags. Tag sources therefore compose.
//   6. hitStunTicks-- when positive; Tag::HitStun is cleared whenever the
//      counter is 0 afterwards.
//   7. Timers: every live entry's ticksLeft-- (saturating), except a mode's
//      baseline (v0.32: kPermanentModifierTicks, never counted down, so
//      never dropped; grantBaseline() adds it at the spawn).
// An entry added on tick T (InteractionSystem, step 7 of the pipeline) with
// ticksLeft D (the def's durationTicks, >= 1) thus shapes the stats, tags
// and slots of the state after ticks T .. T+D-1 and is dropped on tick T+D;
// the list, the stats and the tags always agree between ticks.
#include "kfm/sim/systems.hpp"
#include "systems/modifier_internal.hpp"
#include "systems/statics.hpp"

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
    const uint16_t left = mods.items[i].ticksLeft;
    if (left > 0 && left != kPermanentModifierTicks) --mods.items[i].ticksLeft;
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

[[nodiscard]] Stats deriveStats(const ContentDB& db, const ModifierList& mods,
                                Fixed hazardSpeedMul) {
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

void applyAbilityOverrides(Player& p, const MatchContent& content) {
  const ContentDB& db = content.db;
  std::array<uint8_t, kAbilitySlots> want = content.loadout(p.id);
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

// Step 5. ModifierSystem owns exactly the bits it set last time
// (Player::grantedTags): it clears those it no longer grants, unless another
// source holds them right now (an ability phase, or a mode's spawn
// protection for Invulnerable), sets this tick's grants and records them.
void applyTags(Player& p, const WorldState& s, const MatchContent& content) {
  Tags granted{};
  forEachGrant(content.db, p.modifiers, [&](const PowerUpGrant& g) {
    if (g.kind == GrantKind::Tag) granted.set(g.tag);
  });
  granted.clear(Tag::HitStun);  // system-owned, never granted (the loader rejects them too)
  granted.clear(Tag::Eliminated);
  Tags held = abilityPhaseTags(p, content);
  if (p.id.value < kMaxPlayers && s.rules.invulnTicks[p.id.value] > 0) {
    held.set(Tag::Invulnerable);
  }
  const uint32_t dropped = p.grantedTags.bits & ~granted.bits & ~held.bits;
  p.tags.bits = (p.tags.bits & ~dropped) | granted.bits;
  p.grantedTags = granted;
}

}  // namespace

void grantBaseline(Player& p, const MatchContent& content) {
  const ModeDef& mode = content.mode;
  const ContentDB& db = content.db;
  if (!mode.hasBaseline() || mode.baseline >= db.powerUpCount) return;
  p.modifiers = ModifierList{};
  p.modifiers.items[0] = ActiveModifier{mode.baseline, kPermanentModifierTicks};
  p.modifiers.count = 1;
  p.stats = deriveStats(db, p.modifiers, statics::speedMulAt(content.arena, p.transform.pos));
  p.hp = p.stats.maxHp;
  int32_t ammo = p.ammo;
  forEachGrant(db, p.modifiers, [&](const PowerUpGrant& g) {
    if (g.kind == GrantKind::Stat && g.stat.stat == StatKind::AmmoMax) {
      ammo += std::max(g.stat.add.floorToInt(), int32_t{0});
    }
  });
  p.ammo = static_cast<uint8_t>(std::clamp<int32_t>(ammo, 0, p.stats.ammoMax));
  applyAbilityOverrides(p, content);
  Tags granted{};
  forEachGrant(db, p.modifiers, [&](const PowerUpGrant& g) {
    if (g.kind == GrantKind::Tag) granted.set(g.tag);
  });
  granted.clear(Tag::HitStun);
  granted.clear(Tag::Eliminated);
  p.tags.bits |= granted.bits;
  p.grantedTags = granted;
}

void modifierSystem(WorldState& s, const MatchContent& content) {
  const ContentDB& db = content.db;
  s.players.forEachAlive([&](EntityId /*id*/, Player& p) {
    dropExpired(p.modifiers);
    const uint8_t oldMaxHp = p.stats.maxHp;
    p.stats = deriveStats(db, p.modifiers, statics::speedMulAt(content.arena, p.transform.pos));
    clampHp(p, oldMaxHp, p.stats.maxHp);
    p.ammo = std::min(p.ammo, p.stats.ammoMax);
    applyAbilityOverrides(p, content);
    applyTags(p, s, content);
    if (p.hitStunTicks > 0) --p.hitStunTicks;
    if (p.hitStunTicks == 0) p.tags.clear(Tag::HitStun);
    countDown(p.modifiers);
  });
}

}  // namespace kfm::sim
