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
  std::array<Cell, sim::kMaxBoomerangs> mine{};
  uint32_t mineCount = 0;
  world.boomerangs.forEachAlive([&](EntityId /*id*/, const sim::Boomerang& b) {
    if (b.owner == slot && inFlight(b) && mineCount < mine.size()) {
      mine[mineCount++] = cellOf(a, b.transform.pos);
    }
  });
  const auto seen = [&](Cell k) {
    if (chebyshev(here, k) <= int32_t{v.peripheral}) return true;
    if (inCone(k.col - here.col, k.row - here.row, octant, cone) && lineOfSight(a, here, k)) {
      return true;
    }
    for (uint32_t i = 0; i < mineCount; ++i) {
      if (chebyshev(mine[i], k) <= int32_t{v.bananaSight}) return true;
    }
    return false;
  };

  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    const sim::Player* p = world.playerBySlot(PlayerId{i});
    if (p == nullptr) continue;
    if (i == slot.value || seen(cellOf(a, p->transform.pos))) {
      s.players = static_cast<uint16_t>(s.players | bit16(i));
    }
  }
  world.boomerangs.forEachAlive([&](EntityId id, const sim::Boomerang& b) {
    const Cell k = cellOf(a, b.transform.pos);
    const bool flying = inFlight(b);
    if ((flying && b.owner == slot) || (flying && chebyshev(here, k) <= int32_t{v.hearing}) ||
        seen(k)) {
      s.bananas |= uint64_t{1} << id.index;
    }
  });
  world.pickups.forEachAlive([&](EntityId id, const sim::Pickup& k) {
    if (seen(cellOf(a, k.pos))) s.pickups |= uint32_t{1} << id.index;
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
