// ModeBehavior registry (architecture §5.3, v0.23): one constexpr row per
// ModeKind, in enum order.
#include "kfm/sim/modes/registry.hpp"

#include "kfm/sim/modes/modes.hpp"

#include <array>
#include <cstddef>

namespace kfm::sim {
namespace {

constexpr std::array<ModeBehavior, static_cast<size_t>(ModeKind::kCount)> kBehaviors = {
    roundsBehavior(),      // ModeKind::Rounds
    deathmatchBehavior(),  // ModeKind::Deathmatch
    survivalBehavior(),    // ModeKind::Survival
};

}  // namespace

const ModeBehavior& modeBehaviorFor(ModeKind kind) {
  const auto index = static_cast<size_t>(kind);
  if (index >= kBehaviors.size()) return kBehaviors[0];
  return kBehaviors[index];
}

}  // namespace kfm::sim
