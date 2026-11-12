// ModifierSystem pieces shared with the spawn (architecture §5.8, v0.32).
// Internal to engine/sim/src.
#pragma once

#include "kfm/sim/content.hpp"
#include "kfm/sim/world_state.hpp"

namespace kfm::sim {

// Gives a freshly reset monkey its mode's baseline power-up (ModeDef::
// baseline) as a permanent ModifierList entry (kPermanentModifierTicks) and
// applies it at once, as ModifierSystem would: derived stats with hp at the
// new maxHp, ammo capped at the new ammoMax (v0.81: never topped up, so an
// ammo_max baseline raises the capacity without filling the hand), ability
// overrides, granted tags.
// Nothing happens for a mode without a baseline, so a free-for-all spawn is
// untouched.
void grantBaseline(Player& p, const MatchContent& content);

}  // namespace kfm::sim
