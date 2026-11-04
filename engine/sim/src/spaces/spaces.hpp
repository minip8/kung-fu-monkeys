// The space behaviours of the SpaceBehavior table (architecture §5.12), one
// source file each under src/spaces/. Internal to engine/sim/src.
#pragma once

#include "kfm/sim/spaces/registry.hpp"

namespace kfm::sim {

// Continuous (spaces/continuous.cpp): the sim as built, bit for bit.
void continuousLatch(WorldState& s, const MatchContent& content, const InputFrame& frame);
void continuousMove(WorldState& s, const MatchContent& content);

[[nodiscard]] constexpr SpaceBehavior continuousSpace() {
  return SpaceBehavior{&continuousLatch, &continuousMove};
}

}  // namespace kfm::sim
