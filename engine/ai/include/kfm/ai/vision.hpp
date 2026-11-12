// Fog (architecture §8 "Teams, fog and calls", v0.35): what one monkey sees
// of the world under its mode's VisionDef, a projection the observation
// builder applies and the sim never learns of. Integer arithmetic on cells
// (kfm/sim/spaces/grid_cells.hpp's mapping) only, like the rest of
// engine/ai, so a fog match reproduces bit for bit everywhere.
//
// A monkey at cell `here` facing octant o (its facing quantised to eight
// directions, sim::grid::octantOf of the aim byte) sees a cell k when
//   - peripheral: Chebyshev(k - here) <= vision.peripheral (its own cell
//     always, radius 0 included), whatever the facing and the walls; or
//   - cone: k lies in the cone of o out to R = vision.cone (+ vision.focus
//     when the monkey is focused, below) and the line of sight from `here`
//     to k is clear. The cone of a cardinal octant is the band where the
//     sideways offset is at most the forward one, that of a diagonal octant
//     the closed quadrant, both without `here` and both of exactly R² + 2R
//     cells; or
//   - banana sight: Chebyshev(k - b) <= vision.bananaSight for the cell b of
//     one of its own bananas in flight (Outbound or Returning).
// What it sees: every monkey (itself always) and pickup whose centre's cell
// it sees, every banana whose centre's cell it sees, plus every banana in
// flight within Chebyshev vision.hearing of `here` (heard, whatever the
// facing and the walls) and its own bananas in flight. A centre on a cell
// edge lies in the cells on both sides of it (on a corner in all four):
// seen when one is, heard and banana sight measured to the nearest
// (cellOf alone gave an edge to the east and the south cell, so the two
// sides of a half-turn map saw an edge pickup at different distances).
// The monkey's own cell `here` is cellOf's.
//
// Focus: a monkey is focused on a tick when it is at rest and did not move
// on the previous tick (Player::moveTicks == 0 and transform.pos ==
// transform.prevPos): sentries see far, movers near. On the grid a walk or a
// dash moves on every tick of its slide, the last one included, so a monkey
// that stepped during the turn is not focused at the next latch, and one
// that stood or threw is.
//
// Line of sight: the integer Bresenham line from `here` to k (both ends
// included) must cross no wall cell after `here`, k included; a wall cell is
// one whose centre lies in a wall box (closed), a cell outside the arena's
// columns and rows blocks, and a diagonal step that squeezes between two
// wall cells touching at a corner is blocked (a walk cannot cut that corner
// either). Pits and slow ground never block. Peripheral, hearing and banana
// sight need no line.
//
// Kinds: Own is the monkey's sight; Team the union of every teammate's
// (MatchContent::relation Self or Ally; a dead observer still sees through
// its allies); All everything, without this module.
#pragma once

#include "kfm/core/fixed.hpp"
#include "kfm/core/types.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/spaces/grid_cells.hpp"
#include "kfm/sim/world_state.hpp"

#include <array>
#include <cstdint>

namespace kfm::ai {

using sim::grid::Cell;

[[nodiscard]] constexpr int32_t absInt(int32_t v) {
  return v < 0 ? -v : v;
}
[[nodiscard]] constexpr int32_t chebyshev(int32_t dc, int32_t dr) {
  return absInt(dc) > absInt(dr) ? absInt(dc) : absInt(dr);
}
[[nodiscard]] constexpr int32_t chebyshev(Cell a, Cell b) {
  return chebyshev(b.col - a.col, b.row - a.row);
}

// The column and row step of each octant as int32 (grid_cells.hpp's int8
// tables, widened once here; asserted equal below).
inline constexpr std::array<int32_t, sim::grid::kOctants> kStepCol = {1, 1, 0, -1, -1, -1, 0, 1};
inline constexpr std::array<int32_t, sim::grid::kOctants> kStepRow = {0, -1, -1, -1, 0, 1, 1, 1};
[[nodiscard]] constexpr bool sameSteps() {
  for (uint8_t o = 0; o < sim::grid::kOctants; ++o) {
    if (kStepCol[o] != sim::grid::kOctantCol[o] || kStepRow[o] != sim::grid::kOctantRow[o]) {
      return false;
    }
  }
  return true;
}
static_assert(sameSteps(), "kStepCol/kStepRow must equal sim::grid::kOctantCol/kOctantRow");

// The facing quantised to eight directions (0 = east, counter-clockwise; a
// half-way aim byte rounds counter-clockwise, as a moveDir does).
[[nodiscard]] constexpr uint8_t facingOctant(Angle facing) {
  return sim::grid::octantOf(static_cast<uint8_t>(facing.bam >> 8));
}

// Whether the cell at offset (dc columns east, dr rows south) from the
// monkey lies in the cone of `octant` out to Chebyshev distance `range`.
[[nodiscard]] constexpr bool inCone(int32_t dc, int32_t dr, uint8_t octant, int32_t range) {
  if (range <= 0 || (dc == 0 && dr == 0) || chebyshev(dc, dr) > range) return false;
  const int32_t oc = kStepCol[octant & 7u];
  const int32_t orow = kStepRow[octant & 7u];
  if (sim::grid::diagonal(octant)) return dc * oc >= 0 && dr * orow >= 0;
  const int32_t forward = (dc * oc) + (dr * orow);
  const int32_t side = oc != 0 ? dr : dc;
  return forward >= absInt(side);
}

// Every cell of the integer Bresenham line from `from` to `to`, both
// included, in order; `visit(Cell, bool diagonalStep)` returns false to
// stop (the result is then false). diagonalStep: the cell was reached by a
// step that changed both the column and the row.
template <class F>
constexpr bool bresenham(Cell from, Cell to, F&& visit) {
  const int32_t dx = absInt(to.col - from.col);
  const int32_t dy = -absInt(to.row - from.row);
  const int32_t sx = from.col < to.col ? 1 : -1;
  const int32_t sy = from.row < to.row ? 1 : -1;
  int32_t err = dx + dy;
  Cell k = from;
  bool diag = false;
  while (true) {
    if (!visit(k, diag)) return false;
    if (k == to) return true;
    const int32_t e2 = 2 * err;
    bool stepped = false;
    if (e2 >= dy) {
      err += dy;
      k.col += sx;
      stepped = true;
    }
    if (e2 <= dx) {
      err += dx;
      k.row += sy;
      diag = stepped;
    } else {
      diag = false;
    }
  }
}

// Whether the cell's centre lies in one of the arena's walls.
[[nodiscard]] bool wallCell(const sim::ArenaDef& a, Cell k);
// The line of sight above, from `from` to `to`.
[[nodiscard]] bool lineOfSight(const sim::ArenaDef& a, Cell from, Cell to);
// Focus (above).
[[nodiscard]] constexpr bool focused(const sim::Player& p) {
  return p.moveTicks == 0 && p.transform.pos == p.transform.prevPos;
}

// What a monkey sees on one tick, as entity masks by pool index.
struct Sight {
  uint16_t players{0};  // bit i: slot i
  uint64_t bananas{0};  // bit i: boomerang pool index i
  uint32_t pickups{0};  // bit i: pickup pool index i
  constexpr Sight& operator|=(const Sight& o) {
    players = static_cast<uint16_t>(players | o.players);
    bananas |= o.bananas;
    pickups |= o.pickups;
    return *this;
  }
  constexpr bool operator==(const Sight&) const = default;
};

// The monkey in `slot`'s own sight this tick (nothing without a monkey).
[[nodiscard]] Sight sightOf(const sim::WorldState& world, const sim::MatchContent& content,
                            PlayerId slot);
// What `observer` sees this tick under the mode's vision kind: Own its own
// sight, Team the union over its team (the observer included, alive or
// not), All every live entity. `own`, when given, holds sightOf() for every
// slot already (a host computing them once per tick).
[[nodiscard]] Sight visionOf(const sim::WorldState& world, const sim::MatchContent& content,
                             PlayerId observer,
                             const std::array<Sight, sim::kMaxPlayers>* own = nullptr);

// The turn clock a bot reads (bot-abi.md §3): a grid mode's turn length and
// the ticks from this observation's tick to the next latch (0: this tick's
// input is latched), predicted from the phase machine (a round that ends
// early moves it). Both 0 in a continuous mode.
struct TurnClock {
  uint16_t turnTicks{0};
  uint16_t ticksToLatch{0};
};
[[nodiscard]] TurnClock turnClock(const sim::WorldState& world, const sim::MatchContent& content);
// Whether the turn window a host ORs visibility over closes with this tick:
// on a latch tick, and on every tick that is not Playing on the grid (or of
// a continuous mode, or a one-tick turn), so a window is one turn long.
[[nodiscard]] bool turnWindowEnds(const sim::WorldState& world, const sim::MatchContent& content);

}  // namespace kfm::ai
