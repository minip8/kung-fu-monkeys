// Interaction rule handlers (architecture §5.6). Each returns true when it
// resolved the contact and false when it did not apply (keep scanning).
// a is always a player; b the boomerang, pickup or (dash attack) the victim.
#pragma once

#include "kfm/sim/contacts.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/world_state.hpp"

namespace kfm::sim {

// Prio 10/11, rules/catch.cpp: perfect-dash catch judged against press sub-tick (§5.11);
// declines at ammoMax (v0.16).
bool catchBoomerang(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                    const Contact& contact, EventSink& sink);
// Prio 20, rules/shield.cpp: a shielded player absorbs a banana hit; the
// shield's power-up ends and the banana grounds (§5.8).
bool shieldBlocksBanana(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                        const Contact& contact, EventSink& sink);
// Prio 24, rules/shield.cpp: a shielded player absorbs a dash attack
// (declines below minAmmo, like the attack).
bool shieldBlocksDash(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                      const Contact& contact, EventSink& sink);
// Prio 25, rules/dash_attack.cpp: a dasher holding ammo damages the player it
// touches (no pickup; ammo kept unless content.db.dashAttack.consumesAmmo).
bool dashAttack(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                const Contact& contact, EventSink& sink);
// Prio 30, rules/damage.cpp: hp loss, HitStun, elimination, banana grounds unless pierce.
bool damagePlayer(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                  const Contact& contact, EventSink& sink);
// Prio 40, rules/auto_catch.cpp: the owner touches its returning banana.
bool autoCatch(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
               const Contact& contact, EventSink& sink);
// Prio 50, rules/pickup.cpp: a grounded banana restores ammo.
bool takePickup(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                const Contact& contact, EventSink& sink);
// Prio 51, rules/powerup.cpp: a power-up pickup adds (or refreshes) a
// ModifierList entry; declines with the list full (§5.8).
bool grantPowerUp(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                  const Contact& contact, EventSink& sink);

}  // namespace kfm::sim
