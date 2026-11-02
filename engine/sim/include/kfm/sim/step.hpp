// One simulation tick (architecture §5.3). Systems are stateless free
// functions run in a fixed order; nothing else mutates WorldState.
#pragma once

#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/world_state.hpp"

#include <cstdint>

namespace kfm::sim {

// Advances the world by exactly one tick using the inputs for that tick.
// Precondition: in.tick == s.tick. Events for the tick are appended to sink
// (the caller clears it).
void step(WorldState& s, const InputFrame& in, EventSink& sink);

// Per-tick state checksum through the visitor.
[[nodiscard]] uint32_t checksum(const WorldState& s);

}  // namespace kfm::sim
