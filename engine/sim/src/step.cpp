// Tick pipeline (architecture §5.3): the ten systems in their fixed order,
// with the per-tick contact list living only in this frame.
#include "kfm/sim/step.hpp"

#include "kfm/core/assert.hpp"
#include "kfm/sim/contacts.hpp"
#include "kfm/sim/systems.hpp"
#include "kfm/sim/visit.hpp"

namespace kfm::sim {

void step(WorldState& s, const InputFrame& in, const MatchContent& content, EventSink& sink) {
  StepStats stats;
  step(s, in, content, sink, stats);
}

void step(WorldState& s, const InputFrame& in, const MatchContent& content, EventSink& sink,
          StepStats& stats) {
  KFM_ASSERT(in.tick == s.tick);
  rulesPreTick(s, content, sink);         // 1
  inputSystem(s, content, in);            // 2
  abilitySystem(s, content, sink);        // 3
  movementSystem(s, content);             // 4
  projectileSystem(s, content, sink);     // 5
  Contacts contacts;                      // derived, never part of WorldState
  collisionSystem(s, content, contacts);  // 6
  stats.contactsDropped = contacts.dropped;
  interactionSystem(s, content, contacts, sink);  // 7
  modifierSystem(s, content);                     // 8
  lifecycleSystem(s, content, sink);              // 9
  rulesPostTick(s, content, sink);                // 10
  ++s.tick;                                       // 11
}

uint32_t checksum(const WorldState& s) {
  return checksumOf(s);
}

}  // namespace kfm::sim
