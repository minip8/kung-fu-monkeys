// LifecycleSystem (architecture §5.3 step 9): despawns eliminated players and
// turns grounded boomerangs into pickups.
// M1: implemented in a parallel M1 branch (contract step 9). Stub: no-op.
#include "kfm/sim/systems.hpp"

namespace kfm::sim {

void lifecycleSystem(WorldState& /*s*/, const ContentDB& /*content*/, EventSink& /*sink*/) {}

}  // namespace kfm::sim
