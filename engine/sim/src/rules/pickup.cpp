// Pickup taken (prio 50, architecture §5.6). M1 stub: the parallel interaction work fills it in.
#include "kfm/sim/rules/handlers.hpp"

namespace kfm::sim {

bool takePickup(WorldState& /*s*/, const ContentDB& /*content*/, EntityId /*a*/, EntityId /*b*/,
                const Contact& /*contact*/, EventSink& /*sink*/) {
  return false;
}

}  // namespace kfm::sim
