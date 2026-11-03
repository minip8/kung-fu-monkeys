// Interaction rule handlers (architecture §5.6). Each returns true when it
// resolved the contact and false when it did not apply (keep scanning).
// a is always the player, b the boomerang or pickup.
#pragma once

#include "kfm/sim/contacts.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/world_state.hpp"

namespace kfm::sim {

// Prio 10/11, rules/catch.cpp: perfect-dash catch judged against press sub-tick (§5.11);
// declines at ammoMax (v0.16).
bool catchBoomerang(WorldState& s, const ContentDB& content, EntityId a, EntityId b,
                    const Contact& contact, EventSink& sink);
// Prio 30, rules/damage.cpp: hp loss, HitStun, elimination, banana grounds unless pierce.
bool damagePlayer(WorldState& s, const ContentDB& content, EntityId a, EntityId b,
                  const Contact& contact, EventSink& sink);
// Prio 40, rules/auto_catch.cpp: the owner touches its returning banana.
bool autoCatch(WorldState& s, const ContentDB& content, EntityId a, EntityId b,
               const Contact& contact, EventSink& sink);
// Prio 50, rules/pickup.cpp: a grounded banana restores ammo.
bool takePickup(WorldState& s, const ContentDB& content, EntityId a, EntityId b,
                const Contact& contact, EventSink& sink);

}  // namespace kfm::sim
