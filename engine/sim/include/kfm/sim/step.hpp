// One simulation tick (architecture §5.3). Systems are stateless free
// functions run in a fixed order; nothing else mutates WorldState.
#pragma once

#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/world_state.hpp"

#include <cstdint>

namespace kfm::sim {

// Advances the world by exactly one tick using the inputs for that tick.
// Precondition: in.tick == s.tick. Content is explicit so tests can pass
// custom defs and hosts the loaded ones with the arena and mode of their
// MatchConfig (matchContent()); it must be the same for every tick of a
// match. Events for the tick are appended to sink (the caller clears it).
void step(WorldState& s, const InputFrame& in, const MatchContent& content, EventSink& sink);

// What one tick's fixed-capacity scratch had to drop (v0.70): derived like
// the scratch itself, never part of WorldState, never read by the sim. The
// contact list (kMaxContacts) is the one list a dense enough world can
// overflow; a dropped contact is a hit, catch or pickup that silently did
// not happen. Tools and tests read it; the drop itself is deterministic.
struct StepStats {
  uint16_t contactsDropped{0};
};

// step() that also reports the tick's StepStats (overwritten, not summed).
void step(WorldState& s, const InputFrame& in, const MatchContent& content, EventSink& sink,
          StepStats& stats);

// Per-tick state checksum through the visitor.
[[nodiscard]] uint32_t checksum(const WorldState& s);

}  // namespace kfm::sim
