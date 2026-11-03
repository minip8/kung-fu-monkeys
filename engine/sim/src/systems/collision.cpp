// CollisionSystem (architecture §5.3 step 6): uniform-grid broadphase checked
// against brute force, swept circle narrowphase with time of impact.
// M1: implemented in a parallel M1 branch (contract step 6). Stub: no-op.
#include "kfm/sim/systems.hpp"

namespace kfm::sim {

void collisionBroadphaseGrid(const WorldState& /*s*/, const ContentDB& /*content*/,
                             CandidatePairs& out) {
  out.clear();
}

void collisionBroadphaseBruteForce(const WorldState& /*s*/, const ContentDB& /*content*/,
                                   CandidatePairs& out) {
  out.clear();
}

void collisionSystem(const WorldState& /*s*/, const ContentDB& /*content*/, Contacts& out) {
  out.clear();
}

}  // namespace kfm::sim
