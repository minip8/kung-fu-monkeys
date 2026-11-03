// The ten tick systems (architecture §5.3), in pipeline order. Internal to
// engine/sim: step() is the only production caller; tests may call systems
// directly. Each system is a stateless free function in its own file under
// src/systems/, and only systems mutate WorldState.
#pragma once

#include "kfm/sim/contacts.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/world_state.hpp"

namespace kfm::sim {

// 1. Round start/end, spawns, phase timers (systems/rules.cpp).
void rulesPreTick(WorldState& s, const MatchContent& content, EventSink& sink);
// 2. PlayerInput -> Intent, press-edge detection, facing (systems/input.cpp).
void inputSystem(WorldState& s, const InputFrame& in);
// 3. Activation, phase state machines, cooldowns, phase tags (systems/ability.cpp,
//    behaviours in abilities/).
void abilitySystem(WorldState& s, const MatchContent& content, EventSink& sink);
// 4. Intent x stats -> velocity, integrate, arena clamp (systems/movement.cpp).
void movementSystem(WorldState& s, const MatchContent& content);
// 5. Boomerang flight, return steering, wall bounce, grounding (systems/projectile.cpp).
void projectileSystem(WorldState& s, const MatchContent& content, EventSink& sink);
// 6. Broadphase + swept narrowphase; reads state only (systems/collision.cpp).
void collisionSystem(const WorldState& s, const MatchContent& content, Contacts& out);
// 7. Contacts resolved through the rules table (systems/interaction.cpp, rules/).
//    A Player x Player contact is tried in both orientations (§5.6).
void interactionSystem(WorldState& s, const MatchContent& content, const Contacts& contacts,
                       EventSink& sink);
// InteractionSystem's step for one contact, in the orientation given: scan the
// rows by priority until a handler returns true or an entity is gone. Exported
// for kfm_replay stats, which resolves contacts one at a time in
// InteractionSystem's order; the caller checks MatchPhase::Playing.
void interactionResolveContact(WorldState& s, const MatchContent& content, const Contact& c,
                               EventSink& sink);
// 8. Modifier timers, derived Stats, HP clamp, HitStun expiry (systems/modifier.cpp).
void modifierSystem(WorldState& s, const MatchContent& content);
// 9. Eliminated players despawn, grounded boomerangs become pickups (systems/lifecycle.cpp).
void lifecycleSystem(WorldState& s, const MatchContent& content, EventSink& sink);
// 10. Win conditions and scoring (systems/rules.cpp).
void rulesPostTick(WorldState& s, const MatchContent& content, EventSink& sink);

// CollisionSystem's two broadphases, exported so a property test can assert
// they produce the same candidate set (§5.3). Both fill `out` with the
// Player x Player (indexA < indexB), Player x Boomerang and Player x Pickup
// pairs whose swept AABBs
// (prevPos..pos ± radius) overlap, deduplicated and sorted by
// (kindA, indexA, kindB, indexB).
void collisionBroadphaseGrid(const WorldState& s, const MatchContent& content, CandidatePairs& out);
void collisionBroadphaseBruteForce(const WorldState& s, const MatchContent& content,
                                   CandidatePairs& out);

}  // namespace kfm::sim
