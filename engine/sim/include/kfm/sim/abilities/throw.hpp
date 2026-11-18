// Throw ability (architecture §5.5, §5.7): spends one ammo and spawns a
// boomerang banana on the Startup -> Active transition. v0.48, the charged
// throw: a def with a charge rate (AbilityDef::speed > 0) keeps its Startup
// going while the button is held; the banana leaves on the release (once the
// def's startup has run) along the aim of that tick, its speed, and so its
// reach over the fixed outbound ticks, scaled by throwChargeMul().
#pragma once

#include "kfm/sim/abilities/registry.hpp"

namespace kfm::sim {

bool throwCanActivate(const WorldState& s, const Player& p, const AbilityDef& def);
void throwOnActivate(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                     const MatchContent& content, EventSink& sink);
void throwOnTick(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                 const MatchContent& content, EventSink& sink);
void throwOnPhaseChange(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                        AbilityPhase from, AbilityPhase to, const MatchContent& content,
                        EventSink& sink);
void throwOnEnd(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                const MatchContent& content, EventSink& sink);

// A def charges when its rate is positive: param0 is the speed multiplier of
// a tap, param1 that of a full charge, speed the multiplier gained per tick
// held past the startup (TOML charge_min, charge_max, charge_ticks).
[[nodiscard]] constexpr bool throwCharges(const AbilityDef& def) {
  return def.kind == AbilityKind::Throw && def.speed > Fixed{};
}

// The speed multiplier of a banana thrown after `chargedTicks` ticks held
// past the startup: param0 + speed x chargedTicks, at most param1; 1 for a
// def that does not charge.
[[nodiscard]] constexpr Fixed throwChargeMul(const AbilityDef& def, uint32_t chargedTicks) {
  if (!throwCharges(def)) return Fixed::one();
  const uint32_t cap = 65535;
  const Fixed mul =
      def.param0 + (def.speed * static_cast<int32_t>(chargedTicks < cap ? chargedTicks : cap));
  return mul < def.param1 ? mul : def.param1;
}

bool throwHoldsPhase(const AbilityState& st, const AbilityDef& def, bool buttonHeld);

// v0.89, the recall. A Throw def with a recall speed recalls: holding its
// slot's button with the slot Idle and an empty hand lifts the monkey's
// nearest banana lying on the floor (a banana pickup whose last owner it is,
// bananaLastOwner(), within the def's recallRange when that is set) as a
// Recalled banana (BananaRecallStarted); ProjectileSystem pulls it home while
// the button stays held (heldRecallDef) and drops it when it is let go. One
// at a time: the hand is empty only until the pull arrives, and a monkey
// with a banana of its own Recalled lifts no other. The pull consumes no
// press: a banana pulled into the hand while the button is still held
// starts no throw (a throw needs a fresh press edge).
[[nodiscard]] constexpr bool throwRecalls(const AbilityDef& def) {
  return def.kind == AbilityKind::Throw && def.recallSpeed > Fixed{};
}

// The def of p's first slot that recalls (throwRecalls), or nullptr; its
// slot index in *slot when given.
[[nodiscard]] const AbilityDef* recallDefOf(const Player& p, const MatchContent& content,
                                            uint8_t* slot = nullptr);

// The recalling def when p is pulling now: alive and standing (not
// Eliminated, not Falling), the recalling slot Idle with its button held.
// nullptr otherwise.
[[nodiscard]] const AbilityDef* heldRecallDef(const Player& p, const MatchContent& content);

void throwOnIdleHold(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                     const MatchContent& content, EventSink& sink);

[[nodiscard]] constexpr AbilityBehavior throwBehavior() {
  return AbilityBehavior{&throwCanActivate, &throwOnActivate, &throwOnTick,     &throwOnPhaseChange,
                         &throwOnEnd,       &noKindTags,      &throwHoldsPhase, &throwOnIdleHold};
}

}  // namespace kfm::sim
