// SpaceBehavior registry (architecture §5.12): one constexpr row per Space,
// in enum order.
#include "kfm/sim/spaces/registry.hpp"

#include "kfm/core/field_count.hpp"
#include "spaces/spaces.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

namespace kfm::sim {
namespace {

constexpr std::array<SpaceBehavior, static_cast<size_t>(Space::kCount)> kBehaviors = {
    continuousSpace(),  // Space::Continuous
    gridSpace(),        // Space::Grid (v0.34)
};

// Both hooks are called unconditionally: a row missing from the list above (a
// new Space) or a behaviour that leaves a hook unset is a null call at the
// first dispatch, so it is a compile error instead.
constexpr bool rowComplete(const SpaceBehavior& b) {
  return b.latch != nullptr && b.move != nullptr;
}
static_assert(fieldCount<SpaceBehavior>() == 2, "check the new hook in rowComplete");
static_assert(std::ranges::all_of(kBehaviors, rowComplete),
              "every Space needs a row with every hook set");

}  // namespace

const SpaceBehavior& spaceBehaviorFor(Space space) {
  const auto index = static_cast<size_t>(space);
  if (index >= kBehaviors.size()) return kBehaviors[0];
  return kBehaviors[index];
}

}  // namespace kfm::sim
