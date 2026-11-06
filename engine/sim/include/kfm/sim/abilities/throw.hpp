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

[[nodiscard]] constexpr AbilityBehavior throwBehavior() {
  return AbilityBehavior{&throwCanActivate, &throwOnActivate, &throwOnTick,    &throwOnPhaseChange,
                         &throwOnEnd,       &noKindTags,      &throwHoldsPhase};
}

}  // namespace kfm::sim
