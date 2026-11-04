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
// 2. PlayerInput -> Intent, press-edge detection, then the space's latch:
//    facing and the grid's commitments (systems/input.cpp, spaces/).
void inputSystem(WorldState& s, const MatchContent& content, const InputFrame& in);
// 3. Activation, phase state machines, cooldowns, phase tags (systems/ability.cpp,
//    behaviours in abilities/).
void abilitySystem(WorldState& s, const MatchContent& content, EventSink& sink);
// The tags the player's ability slots grant in their current phases (Dashing,
// PerfectDash, def phase tags; never HitStun or Eliminated). AbilitySystem
// sets exactly these; ModifierSystem keeps them when its own grants end.
[[nodiscard]] Tags abilityPhaseTags(const Player& p, const MatchContent& content);
// The tags one slot grants in its current phase (the def's phase tags plus
// the kind's AbilityBehavior::kindTags).
[[nodiscard]] Tags slotPhaseTags(const AbilityState& st, const MatchContent& content);
// The first slot (in slot order) whose current phase grants `tag`, or null:
// the dash whose press time the catch rule and the tie-break read (§5.11).
[[nodiscard]] const AbilityState* slotGranting(const Player& p, const MatchContent& content,
                                               Tag tag);
// Whether any of the player's ability slots runs a phase its def roots
// (AbilityDef::rootPhases, v0.34): movement intent is ignored (continuous)
// and no commitment is taken at a latch (grid).
[[nodiscard]] bool rooted(const Player& p, const MatchContent& content);
// 4. The space's move: intent x stats -> velocity, integrate, arena clamp
//    (continuous), or the slide to the committed cell (grid)
//    (systems/movement.cpp, spaces/).
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
// 8. Power-up expiry and timers, derived Stats, HP clamp, ability overrides,
//    granted tags, HitStun expiry (systems/modifier.cpp).
void modifierSystem(WorldState& s, const MatchContent& content);
// 9. Eliminated players despawn, grounded boomerangs become pickups, power-up
//    spawners (systems/lifecycle.cpp).
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
