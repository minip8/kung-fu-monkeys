// The observation builder (architecture §8): projects the WorldState onto one
// slot's Observation v1. A pure function of its arguments in integer and
// fixed-point arithmetic only (no floating point in engine/ai, CI greps), no
// heap, no hash-container iteration, so a bot match reproduces bit for bit on
// every machine and in the wasm build. It never writes the world.
#pragma once

#include "kfm/ai/observation.hpp"
#include "kfm/core/fixed.hpp"
#include "kfm/core/types.hpp"
#include "kfm/core/vec2.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/world_state.hpp"

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

// Fills `out` (every byte: the previous contents never leak) with the
// observation for `self` of the world as it stands before tick world.tick is
// stepped. inputDelayTicks is the host's delay for that slot (§8), copied
// through. The overload without a hash computes contentHash(content.db);
// a host building many observations per tick passes it in.
void buildObservation(const sim::WorldState& world, const sim::MatchContent& content,
                      uint32_t contentHash, PlayerId self, uint8_t inputDelayTicks,
                      Observation& out);
void buildObservation(const sim::WorldState& world, const sim::MatchContent& content, PlayerId self,
                      uint8_t inputDelayTicks, Observation& out);

}  // namespace kfm::ai
