// Arena zones (v0.33, architecture §5.3 "Mode seams", §5.10): objective
// regions that block nothing and emit nothing. A mode's onPlaying asks who
// stands in one; nothing in CollisionSystem or the statics path reads them.
#pragma once

#include "kfm/core/types.hpp"
#include "kfm/sim/content/arena.hpp"
#include "kfm/sim/world_state.hpp"

#include <cstdint>

namespace kfm::sim {

// The monkeys still in play (alive, not Eliminated) whose centre lies in
// zone k of the arena (the closed box), as a mask over their slots; 0 for a
// zone the arena does not have.
[[nodiscard]] PlayerMask zoneOccupants(const WorldState& s, const ArenaDef& arena, uint8_t k);

}  // namespace kfm::sim
