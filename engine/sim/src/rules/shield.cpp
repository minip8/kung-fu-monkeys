// Shield (prio 20 and 24, architecture §5.6, §5.8). Tag::Shielded, granted
// by a power-up's Tag grant, negates one hit entirely and is consumed:
//
// - Row 20, Player[Shielded] × Boomerang[InFlight, !own] (the same tags the
//   damage row 30 needs, so a shield only breaks on a hit that would land):
//   no damage; the banana grounds at the contact point.
// - Row 24, Player[Dashing] × Player[Shielded] (the dash-attack row 25's
//   tags): declines below DashAttackDef::minAmmo like the attack itself;
//   otherwise no damage, the attacker's dash stops (its slot goes to
//   Recovery) and it spends a banana only when consumesAmmo is set.
//
// Either way the victim gets HitStun for hitStunTicks, as from a hit: the
// stagger keeps the same dash, or the next banana in a volley, from landing
// on the following ticks.
//
// Consuming the shield removes the first ModifierList entry (in slot order)
// whose def grants Shielded, ending that whole power-up, and clears the tag;
// ModifierSystem re-derives the tags from what is left in the same tick (a
// second shield power-up keeps the player shielded). Shielded set by anything
// else (a test, a future mode) is just cleared.
#include "kfm/sim/rules/handlers.hpp"
#include "rules/bananas.hpp"

#include <cstdint>

namespace kfm::sim {
namespace {

[[nodiscard]] bool grantsShield(const PowerUpDef& def) {
  for (uint8_t g = 0; g < def.grantCount && g < kMaxPowerUpGrants; ++g) {
    if (def.grants[g].kind == GrantKind::Tag && def.grants[g].tag == Tag::Shielded) return true;
  }
  return false;
}

// Returns the index of the power-up whose shield broke (0xFF when none).
uint8_t consumeShield(Player& p, const ContentDB& db) {
  ModifierList& mods = p.modifiers;
  uint8_t broken = 0xFF;
  for (uint8_t i = 0; i < mods.count && i < kMaxModifiers; ++i) {
    if (!grantsShield(db.powerUpAt(mods.items[i].powerUp))) continue;
    broken = mods.items[i].powerUp;
    for (uint8_t j = i; j + 1 < mods.count; ++j)
      mods.items[j] = mods.items[j + 1];
    mods.items[mods.count - 1] = ActiveModifier{};
    --mods.count;
    break;
  }
  p.tags.clear(Tag::Shielded);
  // The block staggers like a hit, so the same dash or a banana
  // still overlapping on the next ticks cannot land straight through.
  p.hitStunTicks = db.player.hitStunTicks;
  if (p.hitStunTicks > 0) p.tags.set(Tag::HitStun);
  return broken;
}

// The dash runs into the shield and stops: every slot whose Active phase
// grants Dashing jumps to Recovery (as if Active had run out; no phase hook
// fires, AbilitySystem carries on from Recovery next tick), the dash tags
// drop now and the attacker stands still. Without this the rest of the dash
// would land on the next ticks once the victim's stagger ran out.
void stopDash(Player& p, const ContentDB& db) {
  for (AbilityState& st : p.abilities) {
    if (st.phase != AbilityPhase::Active ||
        !db.abilityAt(st.defIndex).tagsActive.has(Tag::Dashing)) {
      continue;
    }
    st.phase = AbilityPhase::Recovery;
    st.phaseTicks = 0;
  }
  p.tags.clear(Tag::Dashing);
  p.tags.clear(Tag::PerfectDash);
  p.transform.vel = Vec2::zero();
}

}  // namespace

bool shieldBlocksBanana(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                        const Contact& contact, EventSink& sink) {
  Player* victim = s.players.get(a);
  Boomerang* banana = s.boomerangs.get(b);
  if (victim == nullptr || banana == nullptr) return false;
  const uint8_t broken = consumeShield(*victim, content.db);
  banana->phase = BoomerangPhase::Grounded;
  banana->ticksInPhase = 0;
  banana->transform.pos = contact.point;
  banana->transform.prevPos = contact.point;
  banana->transform.vel = Vec2::zero();
  sink.push(SimEvent{EventKind::ShieldBroken, s.tick, victim->id, b, banana->owner.value, broken});
  return true;
}

bool shieldBlocksDash(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                      const Contact& contact, EventSink& sink) {
  Player* attacker = s.players.get(a);
  Player* victim = s.players.get(b);
  if (attacker == nullptr || victim == nullptr || a == b) return false;
  const DashAttackDef& def = content.db.dashAttack;
  if (attacker->ammo < def.minAmmo) return false;
  // v0.57: a consumed banana drops at the contact rather than vanishing.
  if (def.consumesAmmo != 0 && attacker->ammo > 0) {
    --attacker->ammo;
    dropBanana(s, content, contact.point, attacker->id, sink);
  }
  stopDash(*attacker, content.db);
  const uint8_t broken = consumeShield(*victim, content.db);
  sink.push(SimEvent{EventKind::ShieldBroken, s.tick, victim->id, a, attacker->id.value, broken});
  return true;
}

}  // namespace kfm::sim
