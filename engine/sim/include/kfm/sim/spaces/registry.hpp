// SpaceBehavior table indexed by Space (architecture §5.12, Appendix A,
// v0.29): how monkeys move, and nothing else. The continuous space is the
// sim as built; the grid turns the arena into cells for the monkeys only
// (bananas, collision, the rules table and snapshots are the same in both).
// The table is constexpr like the ability and mode tables: one file per
// space under src/spaces/, one row per Space in src/spaces/registry.cpp.
#pragma once

#include "kfm/sim/content.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/world_state.hpp"

namespace kfm::sim {

// Hooks of one space. Every pointer is non-null.
struct SpaceBehavior {
  // InputSystem (§5.3 step 2), after every alive monkey's Intent was decoded
  // from the frame: sets Transform::facing and, in the grid, takes the turn's
  // commitments on a latch tick (§5.4, §5.12).
  void (*latch)(WorldState& s, const MatchContent& content, const InputFrame& frame);
  // MovementSystem (§5.3 step 4): prevPos, velocity and the integration
  // against the arena's statics for every alive monkey.
  void (*move)(WorldState& s, const MatchContent& content);
};

// Behaviour for a space; an out-of-range value gets the continuous one.
[[nodiscard]] const SpaceBehavior& spaceBehaviorFor(Space space);

}  // namespace kfm::sim
