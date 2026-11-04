// The observation builder (architecture §8): projects the WorldState onto one
// slot's Observation v1. A pure function of its arguments in integer and
// fixed-point arithmetic only (no floating point in engine/ai, CI greps), no
// heap, no hash-container iteration, so a bot match reproduces bit for bit on
// every machine and in the wasm build. It never writes the world.
#pragma once

#include "kfm/ai/observation.hpp"
#include "kfm/ai/vision.hpp"
#include "kfm/core/fixed.hpp"
#include "kfm/core/types.hpp"
#include "kfm/core/vec2.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/world_state.hpp"

#include <array>
#include <cstdint>

namespace kfm::ai {

// Closest approach of a point at offset `rel` from an observer standing
// still, moving at `vel` per tick: the integer tick k in [0, horizon]
// minimising |rel + k·vel| (exact in int64; the smaller k on a tie) and that
// length, floor-rounded to Q16.16. Exact (no intermediate overflow) for
// |rel| up to 2048 units and |vel| up to 32 units per tick with a horizon up
// to kObsApproachHorizonTicks. A zero velocity gives k = 0 and |rel|.
struct ClosestApproach {
  uint16_t ticks{0};
  Fixed distance{};
};
[[nodiscard]] ClosestApproach closestApproach(Vec2 rel, Vec2 vel,
                                              uint16_t horizon = kObsApproachHorizonTicks);

// What a slot has seen over the current turn window (v0.35, fog): the
// entities by pool index, a banana or pickup by the handle it had when seen
// (a pool slot reused within the turn by an unseen entity stays unseen).
struct ObsVisibility {
  uint16_t players{0};  // bit i: slot i
  std::array<EntityId, kObsMaxBananas> bananas{};
  std::array<EntityId, kObsMaxPickups> pickups{};

  // ORs one tick's sight in (the handles of `world`, the tick it was seen on).
  void add(const Sight& sight, const sim::WorldState& world);
  void clear() { *this = ObsVisibility{}; }
};

// A call a slot heard (v0.35, §8): the caller, its symbol, the tick of the
// call and the caller's cell then (when the arena has cells).
struct HeardCall {
  uint8_t slot{kObsNoSlot};
  uint8_t symbol{0};
  uint32_t tick{0};
  Cell cell{};
  bool hasCell{false};
};

// ObsHeardCall::cellX/cellY of a call whose caller the listener does not see.
inline constexpr int16_t kObsNoCell = -1;

// What a host adds to the projection of one world (v0.35): the turn's
// visibility (null: this tick's alone, visionOf()) and the calls heard in
// the turn, oldest first (at most kObsMaxHeardCalls are reported).
struct ObservationContext {
  const ObsVisibility* visibility{nullptr};
  const HeardCall* calls{nullptr};
  uint8_t callCount{0};
};

// Fills `out` (every byte: the previous contents never leak) with the
// observation for `self` of the world as it stands before tick world.tick is
// stepped. inputDelayTicks is the host's delay for that slot (§8), copied
// through. The overload without a hash computes contentHash(content.db);
// a host building many observations per tick passes it in.
//
// v0.35, docs/bot-abi.md §3: in a grid mode match.reservedTurnTicks and
// reservedTicksToLatch (turnClock()), self.reservedCellX/Y and
// reservedFacingOctant (while alive) are filled. Under a fog vision
// (VisionDef::fog()) header.schemaFlags has kObsFlagFog, self and every
// seen monkey have players[i].reservedVisible 1, every seen banana
// bananas[i].reservedVisible 1, and an unseen monkey keeps only its roster
// fields (present, relation, reservedTeam, respawnTicks) while an unseen
// banana or pickup is all zero. Without fog the visible flags stay 0 and
// the observation is the v0.34 one byte for byte. Heard calls fill
// reservedHeardCalls in order, the cell kObsNoCell when the caller is not
// seen (or the arena has no cells).
void buildObservation(const sim::WorldState& world, const sim::MatchContent& content,
                      uint32_t contentHash, PlayerId self, uint8_t inputDelayTicks,
                      const ObservationContext& context, Observation& out);
void buildObservation(const sim::WorldState& world, const sim::MatchContent& content,
                      uint32_t contentHash, PlayerId self, uint8_t inputDelayTicks,
                      Observation& out);
void buildObservation(const sim::WorldState& world, const sim::MatchContent& content, PlayerId self,
                      uint8_t inputDelayTicks, Observation& out);

}  // namespace kfm::ai
