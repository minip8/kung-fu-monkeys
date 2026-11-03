// CollisionSystem (architecture §5.3 step 6, §5.11): uniform-grid broadphase
// checked against brute force, swept circle narrowphase with time of impact.
// Reads state only; every scratch array is local to the call (no heap, no
// state in WorldState).
#include "kfm/sim/systems.hpp"
#include "systems/collision_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace kfm::sim {
namespace {

using collision::Aabb;
using collision::GridLayout;

static_assert(kMaxPlayers <= 16, "player sets below are uint16_t bit masks");

[[nodiscard]] Fixed pickupRadius(const ContentDB& content) {
  return content.pickup.radius;
}

[[nodiscard]] Aabb playerBox(const Player& p, const ContentDB& content) {
  return collision::sweptAabb(p.transform.prevPos, p.transform.pos, content.player.radius);
}
[[nodiscard]] Aabb boomerangBox(const Boomerang& b, const ContentDB& content) {
  return collision::sweptAabb(b.transform.prevPos, b.transform.pos, content.boomerang.radius);
}
[[nodiscard]] Aabb pickupBox(const Pickup& p, const ContentDB& content) {
  return collision::sweptAabb(p.pos, p.pos, pickupRadius(content));
}

void sortPairs(CandidatePairs& out) {
  std::sort(out.items.begin(), out.items.begin() + out.count);
}

// Players bucketed by grid cell (counting sort). A player whose box spans more
// than kMaxCellsPerPlayer cells (a teleport or a very fast dash) is kept in a
// side list tested against every target instead, so the bucket arrays have a
// fixed bound; the exact AABB test makes the result identical either way.
constexpr uint32_t kMaxCellsPerPlayer = 16;
constexpr uint32_t kMaxPlayerEntries = uint32_t{kMaxPlayers} * kMaxCellsPerPlayer;

struct CellRange {
  uint16_t x0, y0, x1, y1;
  [[nodiscard]] uint32_t area() const { return (uint32_t{x1} - x0 + 1) * (uint32_t{y1} - y0 + 1); }
};

[[nodiscard]] CellRange cellRange(const GridLayout& g, const Aabb& box) {
  return CellRange{g.cellX(box.minX), g.cellY(box.minY), g.cellX(box.maxX), g.cellY(box.maxY)};
}

struct PlayerGrid {
  GridLayout layout{};
  std::array<uint16_t, collision::kMaxGridCells + 1> cellStart{};  // prefix sums
  std::array<uint8_t, kMaxPlayerEntries> entries{};                // player slot per entry
  uint16_t oversized{0};  // PlayerMask of side-list players
  std::array<Aabb, kMaxPlayers> boxes{};
  uint16_t alive{0};  // PlayerMask

  [[nodiscard]] uint32_t cellIndex(uint16_t cx, uint16_t cy) const {
    return (uint32_t{cy} * layout.nx) + cx;
  }
};

void buildPlayerGrid(const WorldState& s, const ContentDB& content, PlayerGrid& g) {
  g.layout = collision::gridLayout(content.arena.halfExtents);
  const uint32_t cellCount = uint32_t{g.layout.nx} * g.layout.ny;
  std::array<CellRange, kMaxPlayers> ranges{};

  // Pass 1: count entries per cell.
  std::fill(g.cellStart.begin(), g.cellStart.begin() + cellCount + 1, uint16_t{0});
  s.players.forEachAlive([&](EntityId id, const Player& p) {
    const uint16_t slot = id.index;
    g.alive |= static_cast<uint16_t>(1u << slot);
    g.boxes[slot] = playerBox(p, content);
    ranges[slot] = cellRange(g.layout, g.boxes[slot]);
    if (ranges[slot].area() > kMaxCellsPerPlayer) {
      g.oversized |= static_cast<uint16_t>(1u << slot);
      return;
    }
    for (uint16_t cy = ranges[slot].y0; cy <= ranges[slot].y1; ++cy) {
      for (uint16_t cx = ranges[slot].x0; cx <= ranges[slot].x1; ++cx) {
        ++g.cellStart[g.cellIndex(cx, cy) + 1];
      }
    }
  });
  // Prefix sums: cellStart[c] .. cellStart[c + 1] are cell c's entries.
  for (uint32_t c = 0; c < cellCount; ++c) {
    g.cellStart[c + 1] = static_cast<uint16_t>(g.cellStart[c + 1] + g.cellStart[c]);
  }
  // Pass 2: fill, in slot order within each cell.
  std::array<uint16_t, collision::kMaxGridCells> cursor{};
  std::copy(g.cellStart.begin(), g.cellStart.begin() + cellCount, cursor.begin());
  for (uint16_t slot = 0; slot < kMaxPlayers; ++slot) {
    const auto bit = static_cast<uint16_t>(1u << slot);
    if ((g.alive & bit) == 0 || (g.oversized & bit) != 0) continue;
    for (uint16_t cy = ranges[slot].y0; cy <= ranges[slot].y1; ++cy) {
      for (uint16_t cx = ranges[slot].x0; cx <= ranges[slot].x1; ++cx) {
        g.entries[cursor[g.cellIndex(cx, cy)]++] = static_cast<uint8_t>(slot);
      }
    }
  }
}

// Mask of players whose box overlaps `box`, found through the grid. A
// target spanning many cells just visits them all: the grid has at most 4096
// cells and a mask dedupes players seen in several of them.
[[nodiscard]] uint16_t queryPlayers(const PlayerGrid& g, const Aabb& box) {
  uint16_t candidates = g.oversized;
  const CellRange r = cellRange(g.layout, box);
  for (uint16_t cy = r.y0; cy <= r.y1; ++cy) {
    for (uint16_t cx = r.x0; cx <= r.x1; ++cx) {
      const uint32_t c = g.cellIndex(cx, cy);
      for (uint32_t e = g.cellStart[c]; e < g.cellStart[c + 1]; ++e) {
        candidates |= static_cast<uint16_t>(1u << g.entries[e]);
      }
    }
  }
  uint16_t hits = 0;
  for (uint16_t slot = 0; slot < kMaxPlayers; ++slot) {
    const auto bit = static_cast<uint16_t>(1u << slot);
    if ((candidates & bit) != 0 && collision::overlaps(g.boxes[slot], box)) hits |= bit;
  }
  return hits;
}

void pushPairs(uint16_t playerMask, EntityKind kindB, uint16_t indexB, CandidatePairs& out) {
  for (uint16_t slot = 0; slot < kMaxPlayers; ++slot) {
    if ((playerMask & (1u << slot)) != 0) {
      out.push(CandidatePair{EntityKind::Player, slot, kindB, indexB});
    }
  }
}

// Narrowphase for one candidate pair; pushes a Contact on a hit.
void narrowphase(const WorldState& s, const ContentDB& content, const CandidatePair& pair,
                 Contacts& out) {
  const Player& player = s.players.items[pair.indexA];
  const Transform& ta = player.transform;
  Vec2 prevB{};
  Vec2 posB{};
  Fixed radiusB{};
  EntityId idB{};
  if (pair.kindB == EntityKind::Boomerang) {
    const Transform& tb = s.boomerangs.items[pair.indexB].transform;
    prevB = tb.prevPos;
    posB = tb.pos;
    radiusB = content.boomerang.radius;
    idB = s.boomerangs.idAt(pair.indexB);
  } else {
    prevB = s.pickups.items[pair.indexB].pos;
    posB = prevB;
    radiusB = pickupRadius(content);
    idB = s.pickups.idAt(pair.indexB);
  }

  const collision::SweptHit hit =
      collision::sweptCircleHit(ta.prevPos, ta.pos, prevB, posB, content.player.radius + radiusB);
  if (!hit.hit) return;

  const Vec2 atA = lerp(ta.prevPos, ta.pos, hit.tHit);
  const Vec2 atB = lerp(prevB, posB, hit.tHit);
  Vec2 normal = normalized(atB - atA);
  if (normal == Vec2::zero()) normal = Vec2{Fixed::one(), Fixed::zero()};
  out.push(Contact{.kindA = EntityKind::Player,
                   .a = s.players.idAt(pair.indexA),
                   .kindB = pair.kindB,
                   .b = idB,
                   .tHit = hit.tHit,
                   .point = atB,
                   .normal = normal});
}

}  // namespace

void collisionBroadphaseGrid(const WorldState& s, const ContentDB& content, CandidatePairs& out) {
  out.clear();
  PlayerGrid grid;
  buildPlayerGrid(s, content, grid);
  if (grid.alive == 0) return;
  s.boomerangs.forEachAlive([&](EntityId id, const Boomerang& b) {
    pushPairs(queryPlayers(grid, boomerangBox(b, content)), EntityKind::Boomerang, id.index, out);
  });
  s.pickups.forEachAlive([&](EntityId id, const Pickup& p) {
    pushPairs(queryPlayers(grid, pickupBox(p, content)), EntityKind::Pickup, id.index, out);
  });
  sortPairs(out);
}

void collisionBroadphaseBruteForce(const WorldState& s, const ContentDB& content,
                                   CandidatePairs& out) {
  out.clear();
  s.players.forEachAlive([&](EntityId pid, const Player& p) {
    const Aabb pb = playerBox(p, content);
    s.boomerangs.forEachAlive([&](EntityId bid, const Boomerang& b) {
      if (collision::overlaps(pb, boomerangBox(b, content))) {
        out.push(CandidatePair{EntityKind::Player, pid.index, EntityKind::Boomerang, bid.index});
      }
    });
    s.pickups.forEachAlive([&](EntityId kid, const Pickup& k) {
      if (collision::overlaps(pb, pickupBox(k, content))) {
        out.push(CandidatePair{EntityKind::Player, pid.index, EntityKind::Pickup, kid.index});
      }
    });
  });
  sortPairs(out);
}

void collisionSystem(const WorldState& s, const ContentDB& content, Contacts& out) {
  out.clear();
  CandidatePairs pairs;
  collisionBroadphaseGrid(s, content, pairs);
  for (const CandidatePair& pair : pairs) {
    narrowphase(s, content, pair, out);
  }
}

}  // namespace kfm::sim
