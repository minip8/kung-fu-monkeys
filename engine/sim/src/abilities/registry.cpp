// AbilityBehavior registry (architecture §5.5): one constexpr row per
// AbilityKind, in enum order.
#include "kfm/sim/abilities/registry.hpp"

#include "kfm/sim/abilities/blink.hpp"
#include "kfm/sim/abilities/dash.hpp"
#include "kfm/sim/abilities/slam.hpp"
#include "kfm/sim/abilities/throw.hpp"

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
};

}  // namespace

const AbilityBehavior& behaviorFor(AbilityKind kind) {
  const auto index = static_cast<size_t>(kind);
  if (index >= kBehaviors.size()) return kNoneBehavior;
  return kBehaviors[index];
}

}  // namespace kfm::sim
