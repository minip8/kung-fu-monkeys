// AbilityBehavior registry (architecture §5.5): one constexpr row per
// AbilityKind, in enum order.
#include "kfm/sim/abilities/registry.hpp"

#include "kfm/core/assert.hpp"
#include "kfm/core/field_count.hpp"
#include "kfm/sim/abilities/blink.hpp"
#include "kfm/sim/abilities/call.hpp"
#include "kfm/sim/abilities/dash.hpp"
#include "kfm/sim/abilities/parry.hpp"
#include "kfm/sim/abilities/slam.hpp"
#include "kfm/sim/abilities/throw.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

namespace kfm::sim {
namespace {

bool noneCanActivate(const WorldState& /*s*/, const Player& /*p*/, const AbilityDef& /*def*/) {
  return false;
}
void noneHook(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/, const AbilityDef& /*def*/,
              const MatchContent& /*content*/, EventSink& /*sink*/) {}
void nonePhaseChange(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/,
                     const AbilityDef& /*def*/, AbilityPhase /*from*/, AbilityPhase /*to*/,
                     const MatchContent& /*content*/, EventSink& /*sink*/) {}

constexpr AbilityBehavior kNoneBehavior{&noneCanActivate, &noneHook, &noneHook,
                                        &nonePhaseChange, &noneHook, &noKindTags};

constexpr std::array<AbilityBehavior, static_cast<size_t>(AbilityKind::kCount)> kBehaviors = {
    kNoneBehavior,    // AbilityKind::None
    dashBehavior(),   // AbilityKind::Dash
    throwBehavior(),  // AbilityKind::Throw
    blinkBehavior(),  // AbilityKind::Blink
    slamBehavior(),   // AbilityKind::Slam
    parryBehavior(),  // AbilityKind::Parry
    callBehavior(),   // AbilityKind::Call (v0.35)
};

// AbilitySystem calls every hook unconditionally: a row missing from the
// list above (a new AbilityKind) or a behaviour that leaves a hook unset is
// a null call at the first dispatch, so it is a compile error instead.
constexpr bool rowComplete(const AbilityBehavior& b) {
  return b.canActivate != nullptr && b.onActivate != nullptr && b.onTick != nullptr &&
         b.onPhaseChange != nullptr && b.onEnd != nullptr && b.kindTags != nullptr &&
         b.holdsPhase != nullptr;
}
static_assert(fieldCount<AbilityBehavior>() == 7, "check the new hook in rowComplete");
KFM_STATIC_ASSERT_HOOKS(std::ranges::all_of(kBehaviors, rowComplete),
                        "every AbilityKind needs a row with every hook set");

}  // namespace

const AbilityBehavior& behaviorFor(AbilityKind kind) {
  const auto index = static_cast<size_t>(kind);
  if (index >= kBehaviors.size()) return kNoneBehavior;
  return kBehaviors[index];
}

}  // namespace kfm::sim
