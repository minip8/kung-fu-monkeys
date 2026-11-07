// Bananas never disappear by accident (architecture §5.6, §5.7, v0.57): every
// path that would destroy a banana entity or a banana held as ammo drops it as
// a floor pickup instead. The exceptions are a round reset (a fresh round),
// the bananas in a monkey's hand when it falls down a pit (they go down with
// it), a banana that comes down wholly over a pit (it drops into the hole:
// Sinking, then gone), a dead monkey's bananas in a mode whose deathBananas is
// vanish (the respawn kinds: the respawn hands one out again), and the extra
// bananas an ammo power-up granted when it runs out (they go with it).
// Internal to engine/sim/src.
#pragma once

#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/world_state.hpp"

#include <cstdint>

namespace kfm::sim {

// Lays one banana pickup at `pos` (PickupSpawned) and returns it. With the
// pickup pool full it is added to the banana pickup nearest `pos` instead
// (Pickup::count + 1, the lowest pool index on a tie, no event): power-ups
// and flags never hold more than 24 of the 32 slots (the loader caps the
// spawners at half the pool, a flag per zone at most 8), so a full pool
// always holds a banana pickup to stack on.
EntityId layBananaPickup(WorldState& s, Vec2 pos, EventSink& sink);

// Drops one banana at `pos` (clamped into the arena box by the pickup
// radius). Wholly over the pits (discOverPits at the banana's radius) it
// sinks: a Sinking banana entity of `owner` there (BoomerangFell); else a
// floor pickup (layBananaPickup) and BananaDropped{owner, pickup}.
void dropBanana(WorldState& s, const MatchContent& content, Vec2 pos, PlayerId owner,
                EventSink& sink);

// Drops `n` of p's held bananas (ammo -= n) around its position: one at the
// position itself, several on a ring of kBananaScatterRadius from angle 0
// (east) in equal steps, each pulled back to the monkey's own position if it
// would overlap a wall, and clamped into the arena box (dropBanana).
void dropHeldBananas(WorldState& s, const MatchContent& content, Player& p, uint8_t n,
                     EventSink& sink);

// Every banana of `owner` still away from it (Outbound or Returning) drops
// where it is (Grounded, velocity zero); LifecycleSystem lays it as a pickup
// this tick, or sinks it if it came down over a pit. Called when a monkey
// starts to fall (it is doomed) and as an eliminated one despawns.
void dropBananasOf(WorldState& s, PlayerId owner);

// What a death (or the start of a fall) does to `owner`'s bananas away from
// it, by ModeDef::deathBananas (v0.57): drop, dropBananasOf; vanish, each
// one despawns where it is (BananaVanished).
void releaseBananasOf(WorldState& s, const MatchContent& content, PlayerId owner, EventSink& sink);

// The ring a monkey's held bananas scatter on at a death (units).
inline constexpr Fixed kBananaScatterRadius = Fixed::fromRaw(Fixed::kOneRaw * 3 / 4);

}  // namespace kfm::sim
