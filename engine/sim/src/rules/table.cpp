// The interaction rules table (architecture §5.6). New twist = new row
// (plus a tag or handler); rows stay sorted by priority.
#include "kfm/sim/rules/handlers.hpp"
#include "kfm/sim/rules/interaction.hpp"

#include <algorithm>
#include <array>

namespace kfm::sim {
namespace {

// A player eliminated earlier in the same tick is still in the pool until
// LifecycleSystem runs; no row may touch it again.
constexpr Tags kGone = Tags::of(Tag::Eliminated);

// The perfect catch is two rows: any banana in flight that is not the
// catcher's, and the catcher's own banana only once it is returning. An own
// outbound banana passes through its thrower (as it does without a dash), so
// throwing while dashing never catches the banana just thrown. The handler
// declines when the catcher is already at ammoMax (v0.16): row 30 then hits a
// full-ammo dasher with a non-own banana, row 40 absorbs an own returning one.
// The dash attack (row 25) keys on (Player, Player) with the attacker as A;
// InteractionSystem tries each player contact both ways round. A stunned
// attacker cannot strike, so the loser of a mutual dash cannot retaliate in
// the same tick.
constexpr std::array<InteractionRule, 6> kRules = {{
    // Player[PerfectDash] x Boomerang[InFlight, !own]: perfect catch.
    {10, EntityKind::Player, Tags::of(Tag::PerfectDash), kGone, EntityKind::Boomerang, Tags{},
     Tags{}, kPhaseInFlight, Ownership::NotOwn, &catchBoomerang},
    // Player[PerfectDash] x Boomerang[Returning, own]: perfect catch of one's own.
    {11, EntityKind::Player, Tags::of(Tag::PerfectDash), kGone, EntityKind::Boomerang, Tags{},
     Tags{}, kPhaseReturning, Ownership::Own, &catchBoomerang},
    // Player[Dashing, !HitStun] x Player[!HitStun, !Invulnerable]: dash attack.
    {25, EntityKind::Player, Tags::of(Tag::Dashing), kGone | Tag::HitStun, EntityKind::Player,
     Tags{}, kGone | Tag::HitStun | Tag::Invulnerable, kPhaseAny, Ownership::Any, &dashAttack},
    // Player[!HitStun, !Invulnerable] x Boomerang[InFlight, !own]: damage.
    {30, EntityKind::Player, Tags{}, kGone | Tag::HitStun | Tag::Invulnerable,
     EntityKind::Boomerang, Tags{}, Tags{}, kPhaseInFlight, Ownership::NotOwn, &damagePlayer},
    // Player[] x Boomerang[Returning, own]: auto catch.
    {40, EntityKind::Player, Tags{}, kGone, EntityKind::Boomerang, Tags{}, Tags{}, kPhaseReturning,
     Ownership::Own, &autoCatch},
    // Player[] x Pickup[]: take pickup.
    {50, EntityKind::Player, Tags{}, kGone, EntityKind::Pickup, Tags{}, Tags{}, kPhaseAny,
     Ownership::Any, &takePickup},
}};

static_assert(std::ranges::is_sorted(kRules, {}, &InteractionRule::priority));

}  // namespace

std::span<const InteractionRule> interactionRules() {
  return kRules;
}

}  // namespace kfm::sim
