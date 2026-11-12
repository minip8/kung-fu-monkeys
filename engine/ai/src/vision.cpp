// Fog (see vision.hpp). Integer arithmetic only.
#include "kfm/ai/vision.hpp"

#include "kfm/sim/content/arena.hpp"
#include "kfm/sim/content/mode.hpp"

#include <array>
#include <cstdint>

namespace kfm::ai {
namespace {

using sim::grid::cellOf;

[[nodiscard]] constexpr bool inFlight(const sim::Boomerang& b) {
  return b.phase != sim::BoomerangPhase::Grounded;
}

[[nodiscard]] constexpr uint16_t bit16(uint32_t i) {
  return static_cast<uint16_t>(1u << i);
}

// The cells a centre lies in: cellOf's, which takes the east and the south
// cell of an edge, and on a column edge the west one too, on a row edge the
// north one (on a corner all four). The half turn maps the set of a point
// onto the set of its image, where cellOf alone leaned east and south.
struct CellSpan {
  Cell lo;  // the north-west cell
  Cell hi;  // cellOf's
};
[[nodiscard]] CellSpan cellsOf(const sim::ArenaDef& a, Vec2 p) {
  const Cell k = cellOf(a, p);
  Cell lo = k;
  const int64_t c = a.cellSize.raw;
  if (c > 0 && (int64_t{p.x.raw} + a.halfExtents.x.raw) % c == 0) lo.col -= 1;
  if (c > 0 && (int64_t{a.halfExtents.y.raw} - p.y.raw) % c == 0) lo.row -= 1;
  return CellSpan{lo, k};
}

// The Chebyshev distance from a cell to the nearest cell of a span.
[[nodiscard]] constexpr int32_t spanDistance(Cell from, const CellSpan& s) {
  const auto gap = [](int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo - v;
    return v > hi ? v - hi : 0;
  };
  return chebyshev(gap(from.col, s.lo.col, s.hi.col), gap(from.row, s.lo.row, s.hi.row));
}

}  // namespace

bool wallCell(const sim::ArenaDef& a, Cell k) {
  const Vec2 c = sim::grid::cellCentre(a, k);
  for (uint32_t i = 0; i < a.wallCount && i < a.walls.size(); ++i) {
    const sim::Wall& w = a.walls[i];
    if (c.x >= w.min.x && c.x <= w.max.x && c.y >= w.min.y && c.y <= w.max.y) return true;
  }
  return false;
}

bool lineOfSight(const sim::ArenaDef& a, Cell from, Cell to) {
  Cell prev = from;
  return bresenham(from, to, [&](Cell k, bool diagonalStep) {
    if (k == from) return true;
    if (!sim::grid::inside(a, k) || wallCell(a, k)) return false;
    if (diagonalStep && wallCell(a, Cell{k.col, prev.row}) && wallCell(a, Cell{prev.col, k.row})) {
      return false;
    }
    prev = k;
    return true;
  });
}

Sight sightOf(const sim::WorldState& world, const sim::MatchContent& content, PlayerId slot) {
  Sight s;
  const sim::Player* me = world.playerBySlot(slot);
  if (me == nullptr) return s;
  const sim::ArenaDef& a = content.arena;
  const sim::VisionDef& v = content.mode.vision;
  const Cell here = cellOf(a, me->transform.pos);
  const uint8_t octant = facingOctant(me->transform.facing);
  const int32_t cone = int32_t{v.cone} + (focused(*me) ? int32_t{v.focus} : 0);

  // The cells of the monkey's own bananas in flight (banana sight).
  std::array<CellSpan, sim::kMaxBoomerangs> mine{};
  uint32_t mineCount = 0;
  world.boomerangs.forEachAlive([&](EntityId /*id*/, const sim::Boomerang& b) {
    if (b.owner == slot && inFlight(b) && mineCount < mine.size()) {
      mine[mineCount++] = cellsOf(a, b.transform.pos);
    }
  });
  const auto seenCell = [&](Cell k) {
    if (chebyshev(here, k) <= int32_t{v.peripheral}) return true;
    if (inCone(k.col - here.col, k.row - here.row, octant, cone) && lineOfSight(a, here, k)) {
      return true;
    }
    for (uint32_t i = 0; i < mineCount; ++i) {
      if (spanDistance(k, mine[i]) <= int32_t{v.bananaSight}) return true;
    }
    return false;
  };
  // A centre is seen when one of its cells is.
  const auto seen = [&](Vec2 at) {
    const CellSpan span = cellsOf(a, at);
    for (int32_t row = span.lo.row; row <= span.hi.row; ++row) {
      for (int32_t col = span.lo.col; col <= span.hi.col; ++col) {
        if (seenCell(Cell{col, row})) return true;
      }
    }
    return false;
  };

  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    const sim::Player* p = world.playerBySlot(PlayerId{i});
    if (p == nullptr) continue;
    if (i == slot.value || seen(p->transform.pos)) {
      s.players = static_cast<uint16_t>(s.players | bit16(i));
    }
  }
  world.boomerangs.forEachAlive([&](EntityId id, const sim::Boomerang& b) {
    const bool flying = inFlight(b);
    if ((flying && b.owner == slot) ||
        (flying && spanDistance(here, cellsOf(a, b.transform.pos)) <= int32_t{v.hearing}) ||
        seen(b.transform.pos)) {
      s.bananas |= uint64_t{1} << id.index;
    }
  });
  world.pickups.forEachAlive([&](EntityId id, const sim::Pickup& k) {
    if (seen(k.pos)) s.pickups |= uint32_t{1} << id.index;
  });
  return s;
}

Sight visionOf(const sim::WorldState& world, const sim::MatchContent& content, PlayerId observer,
               const std::array<Sight, sim::kMaxPlayers>* own) {
  Sight s;
  switch (content.mode.vision.kind) {
    case sim::VisionKind::All:
    case sim::VisionKind::kCount: {
      for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
        if (world.playerBySlot(PlayerId{i}) != nullptr) {
          s.players = static_cast<uint16_t>(s.players | bit16(i));
        }
      }
      world.boomerangs.forEachAlive(
          [&](EntityId id, const sim::Boomerang& /*b*/) { s.bananas |= uint64_t{1} << id.index; });
      world.pickups.forEachAlive(
          [&](EntityId id, const sim::Pickup& /*k*/) { s.pickups |= uint32_t{1} << id.index; });
      return s;
    }
    case sim::VisionKind::Own:
      return own != nullptr && observer.isValid() && observer.value < sim::kMaxPlayers
                 ? (*own)[observer.value]
                 : sightOf(world, content, observer);
    case sim::VisionKind::Team:
      break;
  }
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    const PlayerId id{i};
    if (content.relation(observer, id) == sim::Relation::Enemy) continue;
    if (world.playerBySlot(id) == nullptr) continue;
    s |= own != nullptr ? (*own)[i] : sightOf(world, content, id);
  }
  return s;
}

TurnClock turnClock(const sim::WorldState& world, const sim::MatchContent& content) {
  const sim::ModeDef& m = content.mode;
  if (m.space != sim::Space::Grid) return {};
  const uint32_t turn = m.turnTicks > 0 ? m.turnTicks : 1u;
  const sim::MatchRules& r = world.rules;
  const uint32_t k = r.ticksInPhase;
  uint32_t to = 0;
  switch (r.phase) {
    case sim::MatchPhase::Playing:
      to = (turn - (k % turn)) % turn;
      break;
    case sim::MatchPhase::RoundStarting:
      to = k < m.roundStartTicks ? m.roundStartTicks - k : 0;
      break;
    case sim::MatchPhase::Lobby:
      to = m.roundStartTicks;
      break;
    case sim::MatchPhase::RoundEnded:
      to = (k < m.roundEndTicks ? m.roundEndTicks - k : 0) + m.roundStartTicks;
      break;
    case sim::MatchPhase::MatchEnded:
      break;
  }
  return TurnClock{static_cast<uint16_t>(turn), static_cast<uint16_t>(to > 0xFFFF ? 0xFFFF : to)};
}

bool turnWindowEnds(const sim::WorldState& world, const sim::MatchContent& content) {
  const sim::ModeDef& m = content.mode;
  if (world.rules.phase != sim::MatchPhase::Playing || m.space != sim::Space::Grid ||
      m.turnTicks <= 1) {
    return true;
  }
  return world.rules.ticksInPhase % m.turnTicks == 0;
}

}  // namespace kfm::ai
