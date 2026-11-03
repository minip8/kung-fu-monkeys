// Damage on hit (prio 30, architecture §5.6, §5.7). M1 stub: the parallel interaction work fills it
// in.
#include "kfm/sim/rules/handlers.hpp"

namespace kfm::sim {

bool damagePlayer(WorldState& /*s*/, const ContentDB& /*content*/, EntityId /*a*/, EntityId /*b*/,
                  const Contact& /*contact*/, EventSink& /*sink*/) {
  return false;
}

}  // namespace kfm::sim
