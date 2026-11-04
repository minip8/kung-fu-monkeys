// The grid space (architecture §5.4, §5.12, v0.34): monkeys move from cell
// centre to cell centre, one of the eight neighbours per turn of
// ModeDef::turnTicks; everything else (bananas, collision, the rules table)
// is the continuous sim's. Cells come from kfm/sim/spaces/grid_cells.hpp.
//
// latch (InputSystem, after the decode). Outside Playing, facing follows aim
// as in the continuous space. On a Playing tick with ticksInPhase %
// turnTicks == 0 (a latch), every alive monkey's facing becomes its aim, and
// each monkey that is not Eliminated, not in HitStun, not rooted
// (AbilityDef::rootPhases) and at rest (moveTicks == 0) takes the frame's
// intent:
//   - idle (moveMag == 0): it stays; a monkey off its cell's centre (a push,
//     a stun, a blink) commits to that centre;
//   - else the neighbour of its cell in octantOf(moveDir), when that cell is
//     one of the arena's, its circle (the radius stat) lies in the outer box
//     and overlaps no wall, and for a diagonal both orthogonal neighbours do
//     too (no corner cutting). A pit is allowed: the monkey falls on the way.
//     A blocked step stays (re-centring if off-centre).
// Contested cells, judged in one pass over the steps taken above: a step
// loses to another that targets the same cell, or that swaps cells with it,
// and was pressed earlier (the frame's subTick of the latch tick, lower
// first), then by the lower slot. The loser stays; a winner keeps its
// target even if the loser stays in it. Moving into a cell another monkey
// stands in is not a contest (monkeys overlap as in the continuous space;
// a dash attack needs exactly that). A commitment is moveTarget = the cell
// centre and moveTicks = turnTicks.
//
// move (MovementSystem), per alive monkey: prevPos = pos, velocity zero;
// outside Playing nothing moves. HitStun ends a commitment where the monkey
// stands (the next latch re-centres it). A dash's first Active tick (the
// slot granting Tag::Dashing, phaseTicks 0) commits to up to ModeDef::
// dashCells cells from the cell holding the monkey's centre along
// octantOfAngle(the dash's direction), each step checked like a walk, so a
// wall stops it on the last free cell, with moveTicks = the dash def's
// activeTicks: the slide takes the dash's active phase, Dashing for exactly
// as long as it moves. Then a committed monkey's velocity is (moveTarget -
// pos) / moveTicks, a slam's knockback adds on top, the move goes through
// statics::moveCircle (Slide) like any monkey's and moveTicks counts down.
// The last tick's step is exactly moveTarget - pos, so an unobstructed slide
// lands on the target bit for bit and rounding never accumulates; a slide a
// push deflected ends where it stands. Slow hazards and move_speed have no
// say in the grid: a walk is one cell per turn.
#include "kfm/sim/spaces/grid_cells.hpp"
#include "kfm/sim/systems.hpp"
#include "spaces/spaces.hpp"
#include "systems/angle_of.hpp"
#include "systems/statics.hpp"

#include <array>
#include <cstdint>

namespace kfm::sim {
namespace {

using grid::Cell;

// Whether a monkey of radius r may rest on cell k.
[[nodiscard]] bool cellFree(const ArenaDef& a, Cell k, Fixed r) {
  if (!grid::inside(a, k)) return false;
  const Vec2 c = grid::cellCentre(a, k);
  const Vec2 limit = a.halfExtents - Vec2{r, r};
  if (abs(c.x) > limit.x || abs(c.y) > limit.y) return false;
  return !statics::overlapsAnyWall(a, c, r);
}

// Whether a step from `from` in an octant may be taken.
[[nodiscard]] bool stepFree(const ArenaDef& a, Cell from, uint8_t octant, Fixed r) {
  const Cell to = grid::neighbour(from, octant);
  if (!cellFree(a, to, r)) return false;
  if (!grid::diagonal(octant)) return true;
  return cellFree(a, Cell{to.col, from.row}, r) && cellFree(a, Cell{from.col, to.row}, r);
}

void commit(Player& p, Vec2 target, uint16_t ticks) {
  p.moveTarget = target;
  p.moveTicks = ticks > 0 ? ticks : uint16_t{1};
}

// A monkey that stays at a latch: back to its cell's centre if a push, a
// stun or a blink left it off it.
void stay(Player& p, const ArenaDef& a, uint16_t turn) {
  const Vec2 centre = grid::cellCentre(a, grid::cellOf(a, p.transform.pos));
  if (p.transform.pos != centre) commit(p, centre, turn);
}

struct Step {
  bool taken{false};
  Cell from{};
  Cell to{};
  uint8_t subTick{0};
};

// Whether step j beats step i for a contested cell: the earlier press, then
// the lower slot.
[[nodiscard]] constexpr bool beats(const Step& j, uint8_t slotJ, const Step& i, uint8_t slotI) {
  return j.subTick < i.subTick || (j.subTick == i.subTick && slotJ < slotI);
}

// A dash that started this tick commits to its cells.
void commitDash(Player& p, const MatchContent& content) {
  const AbilityState* dash = slotGranting(p, content, Tag::Dashing);
  if (dash == nullptr || dash->phase != AbilityPhase::Active || dash->phaseTicks != 0) return;
  const ArenaDef& a = content.arena;
  const uint8_t octant = grid::octantOfAngle(angleOf(dash->dir));
  Cell to = grid::cellOf(a, p.transform.pos);
  for (uint8_t k = 0; k < content.mode.dashCells; ++k) {
    if (!stepFree(a, to, octant, p.stats.radius)) break;
    to = grid::neighbour(to, octant);
  }
  commit(p, grid::cellCentre(a, to), content.db.abilityAt(dash->defIndex).activeTicks);
}

}  // namespace

void gridLatch(WorldState& s, const MatchContent& content, const InputFrame& frame) {
  const ArenaDef& a = content.arena;
  const uint32_t turn = content.mode.turnTicks > 0 ? content.mode.turnTicks : 1u;
  if (s.rules.phase != MatchPhase::Playing) {
    s.players.forEachAlive([](EntityId /*id*/, Player& p) { p.transform.facing = p.intent.aim; });
    return;
  }
  if (s.rules.ticksInPhase % turn != 0) return;
  s.players.forEachAlive([](EntityId /*id*/, Player& p) { p.transform.facing = p.intent.aim; });
  if (!grid::hasCells(a)) return;  // an incompatible pair (§5.10): nobody moves

  const auto turnTicks = static_cast<uint16_t>(turn);
  std::array<Step, kMaxPlayers> steps{};
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    Player* p = s.playerBySlot(PlayerId{i});
    if (p == nullptr || p->tags.has(Tag::Eliminated) || p->tags.has(Tag::HitStun) ||
        p->moveTicks > 0 || rooted(*p, content)) {
      continue;
    }
    const PlayerInput in = maskHas(frame.present, p->id) ? frame.inputs[i] : PlayerInput{};
    const Cell here = grid::cellOf(a, p->transform.pos);
    const uint8_t octant = grid::octantOf(in.moveDir);
    if (in.moveMag == 0 || !stepFree(a, here, octant, p->stats.radius)) {
      stay(*p, a, turnTicks);
      continue;
    }
    steps[i] = Step{true, here, grid::neighbour(here, octant), in.subTick};
  }
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    if (!steps[i].taken) continue;
    bool lost = false;
    for (uint8_t j = 0; j < kMaxPlayers && !lost; ++j) {
      if (j == i || !steps[j].taken) continue;
      const bool sameCell = steps[j].to == steps[i].to;
      const bool swap = steps[j].to == steps[i].from && steps[i].to == steps[j].from;
      lost = (sameCell || swap) && beats(steps[j], j, steps[i], i);
    }
    Player* p = s.playerBySlot(PlayerId{i});
    if (p == nullptr) continue;
    if (lost) {
      stay(*p, a, turnTicks);
    } else {
      commit(*p, grid::cellCentre(a, steps[i].to), turnTicks);
    }
  }
}

void gridMove(WorldState& s, const MatchContent& content) {
  const bool playing = s.rules.phase == MatchPhase::Playing;
  s.players.forEachAlive([&](EntityId /*id*/, Player& p) {
    const statics::MoveParams params{.radius = p.stats.radius,
                                     .response = statics::Response::Slide};
    Transform& t = p.transform;
    t.prevPos = t.pos;
    t.vel = Vec2::zero();
    if (!playing) return;
    if (p.tags.has(Tag::HitStun)) {
      p.moveTicks = 0;
    } else if (grid::hasCells(content.arena)) {
      commitDash(p, content);
    }
    if (p.moveTicks > 0) t.vel = (p.moveTarget - t.pos) / int32_t{p.moveTicks};
    if (p.knockbackTicks > 0) {  // a slam's push (v0.21) adds on top of anything
      t.vel = t.vel + p.knockback;
      if (--p.knockbackTicks == 0) p.knockback = Vec2::zero();
    }
    t.pos = statics::moveCircle(content.arena, t.pos, t.vel, t.vel, params).pos;
    if (p.moveTicks > 0) --p.moveTicks;
  });
}

}  // namespace kfm::sim
