// SpaceBehavior registry (architecture §5.12): one constexpr row per Space,
// in enum order.
#include "kfm/sim/spaces/registry.hpp"

#include "spaces/spaces.hpp"

#include <array>
#include <cstddef>

namespace kfm::sim {
namespace {

constexpr std::array<SpaceBehavior, static_cast<size_t>(Space::kCount)> kBehaviors = {
    continuousSpace(),  // Space::Continuous
    gridSpace(),        // Space::Grid (v0.34)
};

}  // namespace

const SpaceBehavior& spaceBehaviorFor(Space space) {
  const auto index = static_cast<size_t>(space);
  if (index >= kBehaviors.size()) return kBehaviors[0];
  return kBehaviors[index];
}

}  // namespace kfm::sim
