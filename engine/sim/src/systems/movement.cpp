// MovementSystem (architecture §5.3 step 4): intent × stats -> velocity,
// integrate, arena clamp.
// M1: implemented in a parallel M1 branch (contract step 4). The stub keeps
// the M0 integration and records prevPos for swept collision.
#include "kfm/sim/systems.hpp"

namespace kfm::sim {

void movementSystem(WorldState& s, const ContentDB& /*content*/) {
  s.players.forEachAlive([](EntityId /*id*/, Player& p) {
    p.transform.prevPos = p.transform.pos;
    p.transform.pos += p.transform.vel;
  });
}

}  // namespace kfm::sim
