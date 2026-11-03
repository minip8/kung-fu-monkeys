// The M1 interaction rules table (architecture §5.6). New twist = new row
// (plus a tag or handler); rows stay sorted by priority.
#include "kfm/sim/rules/handlers.hpp"
#include "kfm/sim/rules/interaction.hpp"

#include <algorithm>
#include <array>

namespace kfm::sim {
namespace {

constexpr std::array<InteractionRule, 4> kRules = {{
    // Player[PerfectDash] x Boomerang[InFlight]: perfect catch (any owner).
    {10, EntityKind::Player, Tags::of(Tag::PerfectDash), Tags{}, EntityKind::Boomerang, Tags{},
     Tags{}, kPhaseInFlight, Ownership::Any, &catchBoomerang},
    // Player[!HitStun, !Invulnerable] x Boomerang[InFlight, !own]: damage.
    {30, EntityKind::Player, Tags{}, Tags::of(Tag::HitStun) | Tag::Invulnerable,
     EntityKind::Boomerang, Tags{}, Tags{}, kPhaseInFlight, Ownership::NotOwn, &damagePlayer},
    // Player[] x Boomerang[Returning, own]: auto catch.
    {40, EntityKind::Player, Tags{}, Tags{}, EntityKind::Boomerang, Tags{}, Tags{}, kPhaseReturning,
     Ownership::Own, &autoCatch},
    // Player[] x Pickup[]: take pickup.
    {50, EntityKind::Player, Tags{}, Tags{}, EntityKind::Pickup, Tags{}, Tags{}, kPhaseAny,
     Ownership::Any, &takePickup},
}};

static_assert(std::ranges::is_sorted(kRules, {}, &InteractionRule::priority));

}  // namespace

std::span<const InteractionRule> interactionRules() {
  return kRules;
}

}  // namespace kfm::sim
