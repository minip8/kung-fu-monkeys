// MovementSystem (architecture §5.3 step 4): the mode's space moves the
// monkeys (§5.12). The continuous space (spaces/continuous.cpp) is intent ×
// stats -> velocity integrated against the arena's statics; the grid
// (spaces/grid.cpp) slides each committed monkey toward its target cell.
#include "kfm/sim/spaces/registry.hpp"
#include "kfm/sim/systems.hpp"

namespace kfm::sim {

void movementSystem(WorldState& s, const MatchContent& content) {
  spaceBehaviorFor(content.mode.space).move(s, content);
}

}  // namespace kfm::sim
