// Owner auto-catch of a returning banana (prio 40, architecture §5.6, §5.7). M1 stub: the parallel
// interaction work fills it in.
#include "kfm/sim/rules/handlers.hpp"

namespace kfm::sim {

bool autoCatch(WorldState& /*s*/, const ContentDB& /*content*/, EntityId /*a*/, EntityId /*b*/,
               const Contact& /*contact*/, EventSink& /*sink*/) {
  return false;
}

}  // namespace kfm::sim
