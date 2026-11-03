// InteractionSystem (architecture §5.3 step 7, §5.6): resolves contacts through
// the prioritised rules table (rules/interaction.hpp).
// M1: implemented in a parallel M1 branch (contract step 7). Stub: no-op.
#include "kfm/sim/systems.hpp"

namespace kfm::sim {

void interactionSystem(WorldState& /*s*/, const ContentDB& /*content*/,
                       const Contacts& /*contacts*/, EventSink& /*sink*/) {}

}  // namespace kfm::sim
