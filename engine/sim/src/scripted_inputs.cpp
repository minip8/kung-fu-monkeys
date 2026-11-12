// Scripted input generator (see scripted_inputs.hpp). Reads WorldState,
// never writes it; all randomness comes from the caller's Rng.
#include "kfm/sim/scripted_inputs.hpp"

#include "kfm/core/vec2.hpp"
#include "kfm/sim/abilities/throw.hpp"
#include "kfm/sim/spaces/grid_cells.hpp"
#include "kfm/sim/zones.hpp"
#include "systems/angle_of.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace kfm::sim {
namespace {

// Nearest 1/256 turn of an angle (the PlayerInput direction encoding).
[[nodiscard]] uint8_t toInput8(Angle a) {
  return static_cast<uint8_t>(((uint32_t{a.bam} + 128u) >> 8) & 0xFFu);
}

[[nodiscard]] uint8_t directionTo(Vec2 from, Vec2 to) {
  return toInput8(angleOf(to - from));
}

[[nodiscard]] uint64_t squareRaw(Fixed r) {
  const auto v = static_cast<uint64_t>(r.raw < 0 ? -int64_t{r.raw} : int64_t{r.raw});
  return v * v;
}

[[nodiscard]] bool oneIn(Rng& rng, uint16_t n) {
  return n != 0 && rng.below(n) == 0;
}

[[nodiscard]] bool ready(const AbilityState& st) {
  return st.phase == AbilityPhase::Idle && st.cooldownTicks == 0;
}

// Wander direction: the current stick direction (decoded intent), a random
// new one now and then, and back toward the interior near a wall.
[[nodiscard]] uint8_t wanderDirection(Rng& rng, const Player& p, const MatchContent& content,
                                      const ScriptedInputConfig& config) {
  const Vec2 move = p.intent.move;
  uint8_t dir =
      move == Vec2::zero() ? static_cast<uint8_t>(rng.below(256)) : toInput8(angleOf(move));
  if (oneIn(rng, config.turnOneIn)) dir = static_cast<uint8_t>(rng.below(256));

  const Vec2 pos = p.transform.pos;
  const Vec2 limit = content.arena.halfExtents - Vec2{config.wallMargin, config.wallMargin};
  const Vec2 heading = Vec2::fromAngle(Angle::fromInput8(dir));
  const bool outX = (pos.x > limit.x && heading.x > Fixed::zero()) ||
                    (pos.x < -limit.x && heading.x < Fixed::zero());
  const bool outY = (pos.y > limit.y && heading.y > Fixed::zero()) ||
                    (pos.y < -limit.y && heading.y < Fixed::zero());
  if (outX || outY) {
    // Toward the centre, give or take an eighth of a turn.
    const int32_t turn = rng.range(-32, 32);
    dir = static_cast<uint8_t>((int32_t{directionTo(pos, Vec2::zero())} + turn) & 0xFF);
  }
  return dir;
}

// The nearest non-own banana in flight within `radius` that is approaching
// `pos`, if any.
[[nodiscard]] bool incomingBanana(const WorldState& s, const Player& p, Fixed radius, Vec2& at) {
  const Vec2 pos = p.transform.pos;
  uint64_t best = squareRaw(radius);
  bool found = false;
  s.boomerangs.forEachAlive([&](EntityId /*bid*/, const Boomerang& b) {
    if (b.phase == BoomerangPhase::Grounded || b.owner == p.id) return;
    const Vec2 rel = b.transform.pos - pos;
    const uint64_t d = lengthSqRaw(rel);
    if (d <= best && dot(rel, b.transform.vel) < Fixed::zero()) {
      best = d;
      at = b.transform.pos;
      found = true;
    }
  });
  return found;
}

// The objective kinds (v0.33): a target to walk to instead of wandering,
// read from the state alone (no Rng draw, so every other mode is unchanged).
// Hill: zone 0 while outside it. Capture: a carrier its own team's zone,
// anyone else the nearest flag of another team. Control (v0.80): the nearest
// zone the monkey's team does not own.
[[nodiscard]] bool objectiveTarget(const WorldState& s, const MatchContent& content,
                                   const Player& p, Vec2& target) {
  const ArenaDef& arena = content.arena;
  const Vec2 pos = p.transform.pos;
  if (content.mode.kind == ModeKind::Hill && arena.zoneCount > 0) {
    const Zone& hill = arena.zones[0];
    if (boxContains(hill.min, hill.max, pos)) return false;
    target = hill.centre();
    return true;
  }
  const uint8_t team = content.teamOf(p.id);
  if (content.mode.kind == ModeKind::Control) {
    // v0.80: the nearest zone its team does not own (word 2 of modeData: a
    // nibble per zone, the owner + 1), while outside it.
    uint64_t nearest = std::numeric_limits<uint64_t>::max();
    bool any = false;
    for (uint8_t k = 0; k < arena.zoneCount && k < kMaxArenaZones; ++k) {
      const uint32_t owner = (s.rules.modeData[2] >> (4u * k)) & 0xFu;
      if (owner == uint32_t{team} + 1u) continue;
      const Zone& zone = arena.zones[k];
      if (boxContains(zone.min, zone.max, pos)) return false;
      const uint64_t d = lengthSqRaw(zone.centre() - pos);
      if (d < nearest) {
        nearest = d;
        target = zone.centre();
        any = true;
      }
    }
    return any;
  }
  if (content.mode.kind != ModeKind::Capture) return false;
  if (p.tags.has(Tag::Carrying)) {
    if (team >= arena.zoneCount || team >= kMaxArenaZones) return false;
    target = arena.zones[team].centre();
    return true;
  }
  uint64_t best = std::numeric_limits<uint64_t>::max();
  bool found = false;
  s.pickups.forEachAlive([&](EntityId /*kid*/, const Pickup& k) {
    if (k.kind != PickupKind::Flag || k.spawner == team) return;
    const uint64_t d = lengthSqRaw(k.pos - pos);
    if (d < best) {
      best = d;
      target = k.pos;
      found = true;
    }
  });
  return found;
}

// Harvest (v0.82) in the continuous space: where a monkey goes, read from
// the state alone (no Rng draw, keyed on the kind, so every other mode is
// unchanged). A hand with room goes for the nearest banana pickup anywhere;
// a full hand, or one with nothing left to pick, goes home (its team's zone,
// homeZoneOf) and stays there, banking. Returns false when it has no home
// either (it wanders); `stay` is set when it should stand still.
[[nodiscard]] bool harvestTarget(const WorldState& s, const MatchContent& content, const Player& p,
                                 Vec2& target, bool& stay) {
  const Vec2 pos = p.transform.pos;
  stay = false;
  if (p.ammo < p.stats.ammoMax) {
    uint64_t best = std::numeric_limits<uint64_t>::max();
    bool found = false;
    s.pickups.forEachAlive([&](EntityId /*kid*/, const Pickup& k) {
      if (k.kind != PickupKind::Banana) return;
      const uint64_t d = lengthSqRaw(k.pos - pos);
      if (d < best) {
        best = d;
        target = k.pos;
        found = true;
      }
    });
    if (found) return true;
  }
  const uint8_t home = homeZoneOf(content.arena, content.teamOf(p.id));
  if (home == kNoZone) return false;
  const Zone& zone = content.arena.zones[home];
  if (boxContains(zone.min, zone.max, pos)) {
    stay = true;
    return true;
  }
  target = zone.centre();
  return true;
}

// --- harvest in the grid space: pathing ---------------------------------------

using grid::Cell;

// The search's capacity: the arena's cells with a closed border round them
// (so a neighbour is an index offset, never off the array), 64 x 64 in all:
// up to 62 x 62 cells. The shipped grid arenas hold at most 26 x 16.
constexpr int32_t kMaxRouteCells = 4096;
constexpr uint16_t kUnreached = 0xFFFF;

// A breadth-first search over the arena's cells from the cell holding
// `from`, for a monkey of contact radius `radius`, by the grid's movement
// rules (src/spaces/grid.cpp): a step goes to one of the eight neighbours,
// one turn each, onto a cell of the arena whose circle lies in the outer box
// and overlaps no wall, and a diagonal step needs both orthogonal neighbours
// open too (no corner cutting). A pit cell (its centre over a pit) is closed
// as a wall is, corners included: the grid lets a monkey walk in and fall,
// a route never does. Other monkeys block nothing (they overlap). Each cell
// keeps its turn count and the octant of the first step of the first
// shortest route found, neighbours being tried in octant order, so the
// answer is a pure function of the arena, the cell and the radius. A grid
// over the capacity (or a start off the grid) is not searched: valid is
// false.
//
// The open cells are found up front: the outer box per cell, then each wall
// and pit box closes the cells of its footprint (the cells between those
// holding its corners, the wall's grown by the radius), each by the exact
// test grid.cpp's cellFree and discOverPits make (circleOverlapsBox for a
// wall, a centre in a pit's closed box for a pit). So a call costs a pass
// over the cells, the boxes' footprints and the search's eight looks a cell.
struct GridRoutes {
  const ArenaDef& arena;
  int32_t cols{0};
  int32_t rows{0};
  int32_t stride{0};  // cols + 2: the border columns
  Cell start{};
  bool valid{false};
  std::array<uint16_t, kMaxRouteCells> turns{};
  std::array<uint8_t, kMaxRouteCells> first{};
  std::array<uint8_t, kMaxRouteCells> open{};  // 1 when a monkey may stand there; 0 on the border
  std::array<uint16_t, kMaxRouteCells> queue{};

  GridRoutes(const ArenaDef& a, Vec2 from, Fixed radius)
      : arena(a),
        cols(grid::columns(a)),
        rows(grid::rows(a)),
        stride(cols + 2),
        start(grid::cellOf(a, from)) {
    const int64_t padded = int64_t{cols + 2} * (rows + 2);
    if (cols <= 0 || rows <= 0 || padded > kMaxRouteCells || !inside(start)) return;
    valid = true;
    std::fill_n(turns.begin(), padded, kUnreached);
    markOpen(radius);
    search();
  }

  [[nodiscard]] bool inside(Cell k) const {
    return k.col >= 0 && k.row >= 0 && k.col < cols && k.row < rows;
  }
  [[nodiscard]] size_t index(Cell k) const {
    const int32_t i = ((k.row + 1) * stride) + k.col + 1;
    return static_cast<size_t>(i);
  }

  // Closes every cell of the box [min, max] grown by `grow` for which
  // `closes(centre)` holds.
  template <class Closes>
  void closeFootprint(Vec2 min, Vec2 max, Fixed grow, Closes&& closes) {
    const Cell nw = grid::cellOf(arena, Vec2{min.x - grow, max.y + grow});
    const Cell se = grid::cellOf(arena, Vec2{max.x + grow, min.y - grow});
    for (int32_t row = std::max(nw.row, 0); row <= std::min(se.row, rows - 1); ++row) {
      for (int32_t col = std::max(nw.col, 0); col <= std::min(se.col, cols - 1); ++col) {
        const Cell k{col, row};
        if (closes(grid::cellCentre(arena, k))) open[index(k)] = 0;
      }
    }
  }

  void markOpen(Fixed radius) {
    const Vec2 limit = arena.halfExtents - Vec2{radius, radius};
    for (int32_t row = 0; row < rows; ++row) {
      for (int32_t col = 0; col < cols; ++col) {
        const Vec2 c = grid::cellCentre(arena, Cell{col, row});
        open[index(Cell{col, row})] = abs(c.x) <= limit.x && abs(c.y) <= limit.y ? 1 : 0;
      }
    }
    for (uint8_t w = 0; w < arena.wallCount && w < kMaxArenaWalls; ++w) {
      const Wall& wall = arena.walls[w];
      closeFootprint(wall.min, wall.max, radius,
                     [&](Vec2 c) { return circleOverlapsBox(wall.min, wall.max, c, radius); });
    }
    for (uint8_t h = 0; h < arena.hazardCount && h < kMaxArenaHazards; ++h) {
      const Hazard& pit = arena.hazards[h];
      if (pit.kind != HazardKind::Pit) continue;
      closeFootprint(pit.min, pit.max, Fixed::zero(),
                     [&](Vec2 c) { return boxContains(pit.min, pit.max, c); });
    }
  }

  void search() {
    // An octant's neighbour as an index offset, and for a diagonal its two
    // orthogonal neighbours (the corner rule).
    std::array<int32_t, grid::kOctants> step{};
    for (uint8_t o = 0; o < grid::kOctants; ++o)
      step[o] = grid::kOctantCol[o] + (grid::kOctantRow[o] * stride);
    size_t head = 0;
    size_t tail = 0;
    const size_t from = index(start);
    turns[from] = 0;
    queue[tail++] = static_cast<uint16_t>(from);
    while (head < tail) {
      const size_t u = queue[head++];
      const auto here = static_cast<int32_t>(u);
      const auto next = static_cast<uint16_t>(turns[u] + 1);
      for (uint8_t o = 0; o < grid::kOctants; ++o) {
        const int32_t to = here + step[o];
        const auto v = static_cast<size_t>(to);
        if (open[v] == 0 || turns[v] != kUnreached) continue;
        if (grid::diagonal(o)) {
          const int32_t across = here + grid::kOctantCol[o];            // same row
          const int32_t along = here + (grid::kOctantRow[o] * stride);  // same column
          if (open[static_cast<size_t>(across)] == 0 || open[static_cast<size_t>(along)] == 0) {
            continue;
          }
        }
        turns[v] = next;
        first[v] = u == from ? o : first[u];
        queue[tail++] = static_cast<uint16_t>(v);
      }
    }
  }

  [[nodiscard]] uint16_t turnsTo(Cell k) const {
    return valid && inside(k) ? turns[index(k)] : kUnreached;
  }
  [[nodiscard]] uint8_t firstStepTo(Cell k) const {
    return valid && inside(k) ? first[index(k)] : uint8_t{0};
  }
};

// The best goal cell seen so far: the fewest turns, the first one offered on
// a tie (callers offer cells in a fixed order).
struct Goal {
  uint16_t turns{kUnreached};
  Cell cell{};

  void offer(const GridRoutes& routes, Cell k) {
    const uint16_t t = routes.turnsTo(k);
    if (t < turns) {
      turns = t;
      cell = k;
    }
  }
  [[nodiscard]] bool reachable() const { return turns != kUnreached; }
};

// Offers every cell whose centre lies in zone k (the closed box, as
// zoneOccupants reads a monkey's centre), in row-major order.
void offerZone(const GridRoutes& routes, uint8_t k, Goal& goal) {
  const Zone& zone = routes.arena.zones[k];
  for (int32_t row = 0; row < routes.rows; ++row) {
    for (int32_t col = 0; col < routes.cols; ++col) {
      const Cell c{col, row};
      if (boxContains(zone.min, zone.max, grid::cellCentre(routes.arena, c))) {
        goal.offer(routes, c);
      }
    }
  }
}

// --- harvest in the grid space: roles and goals --------------------------------

// The roles (by the slot's place among its team's present slots, so
// Rng-free and the same on both sides): the second monkey of a team raids,
// the third defends, the rest gather. A team of one only gathers.
constexpr uint8_t kRaiderRank = 1;
constexpr uint8_t kDefenderRank = 2;

[[nodiscard]] uint8_t teamRank(const WorldState& s, const MatchContent& content, PlayerId id) {
  const uint8_t team = content.teamOf(id);
  uint8_t rank = 0;
  for (uint8_t j = 0; j < id.value && j < kMaxPlayers; ++j) {
    if (maskHas(s.rules.present, PlayerId{j}) && content.teams[j] == team) ++rank;
  }
  return rank;
}

// The harvest steering in the grid space (see scripted_inputs.hpp): sets
// the stick for the role's goal, reached by the first step of a shortest
// route, or idles it on the goal or with no goal reachable. Returns false,
// touching nothing, when the arena's grid cannot be routed.
[[nodiscard]] bool gridHarvestSteer(const WorldState& s, const MatchContent& content,
                                    const Player& p, PlayerInput& in) {
  if (p.tags.has(Tag::Eliminated) || p.tags.has(Tag::Falling)) return true;  // the latch ignores it
  const ArenaDef& arena = content.arena;
  const GridRoutes routes(arena, p.transform.pos, p.stats.radius);
  if (!routes.valid) return false;
  const auto go = [&](const Goal& goal) {
    if (!goal.reachable()) return false;
    if (goal.turns == 0) {
      in.moveMag = 0;
    } else {
      in.moveMag = 255;
      in.moveDir = static_cast<uint8_t>(routes.firstStepTo(goal.cell) * 32u);
    }
    return true;
  };

  const Vec2 pos = p.transform.pos;
  const uint8_t team = content.teamOf(p.id);
  const uint8_t home = homeZoneOf(arena, team);
  const bool room = p.ammo < p.stats.ammoMax;
  const uint8_t rank = teamRank(s, content, p.id);

  // At home with a banana: it stands until it has banked them all.
  if (p.ammo > 0 && home != kNoZone &&
      boxContains(arena.zones[home].min, arena.zones[home].max, pos)) {
    in.moveMag = 0;
    return true;
  }
  // The defender: home while an enemy stands in it and the team has points.
  if (rank == kDefenderRank && home != kNoZone && content.teamScore(s.rules.scores, team) > 0) {
    const PlayerMask inside = zoneOccupants(s, arena, home);
    bool enemy = false;
    for (uint8_t j = 0; j < kMaxPlayers; ++j) {
      if (maskHas(inside, PlayerId{j}) && content.teams[j] != team) enemy = true;
    }
    Goal goal;
    if (enemy) offerZone(routes, home, goal);
    if (go(goal)) return true;
  }
  // The raider, with room in its hand: the nearest enemy home holding points.
  if (rank == kRaiderRank && room) {
    Goal goal;
    const uint8_t zones = std::min<uint8_t>(arena.zoneCount, kMaxArenaZones);
    for (uint8_t k = 0; k < zones; ++k) {
      if (k != team && content.teamScore(s.rules.scores, k) > 0) offerZone(routes, k, goal);
    }
    if (go(goal)) return true;
  }
  // Room in the hand: the nearest fruit (any banana lying anywhere).
  if (room) {
    Goal goal;
    s.pickups.forEachAlive([&](EntityId /*kid*/, const Pickup& k) {
      if (k.kind == PickupKind::Banana) goal.offer(routes, grid::cellOf(arena, k.pos));
    });
    if (go(goal)) return true;
  }
  // A banana in the hand (full, or nothing left to fetch): home.
  if (p.ammo > 0 && home != kNoZone) {
    Goal goal;
    offerZone(routes, home, goal);
    if (go(goal)) return true;
  }
  // Empty-handed with nothing to fetch: wait on the nearest tree.
  if (p.ammo == 0) {
    Goal goal;
    for (uint8_t i = 0; i < arena.pickupSpawnerCount && i < kMaxPickupSpawners; ++i) {
      const PickupSpawner& sp = arena.pickupSpawners[i];
      if (sp.isTree()) goal.offer(routes, grid::cellOf(arena, sp.pos));
    }
    if (go(goal)) return true;
  }
  in.moveMag = 0;  // nothing reachable
  return true;
}

// The third slot (v0.21): only when the loadout or a power-up put an ability
// there, so a base-kit match draws nothing extra from the Rng.
void useExtraAbility(Rng& rng, const WorldState& s, const MatchContent& content, const Player& p,
                     uint64_t nearestEnemySq, const ScriptedInputConfig& config, PlayerInput& in) {
  const AbilityState& slot = p.abilities[kAbilitySlotExtra];
  const AbilityDef& def = content.db.abilityAt(slot.defIndex);
  const uint8_t bit = buttonBit(Button::Ability1);
  if (def.kind == AbilityKind::None || (p.prevButtons & bit) != 0 || !ready(slot)) return;
  Vec2 threat{};
  switch (def.kind) {
    case AbilityKind::Blink:
      // Blink away from an incoming banana (sideways to its path), or now and then.
      if (incomingBanana(s, p, config.threatRadius, threat)) {
        if (rng.below(256) < config.dodgePer256) {
          in.buttons |= bit;
          const uint8_t toward = directionTo(p.transform.pos, threat);
          in.moveDir = static_cast<uint8_t>(toward + (rng.below(2) == 0 ? 64 : 192));
          in.moveMag = 255;
        }
      } else if (oneIn(rng, config.abilityOneIn)) {
        in.buttons |= bit;
      }
      break;
    case AbilityKind::Slam: {
      // Slam when an enemy or an incoming banana is within the reach.
      const uint64_t reachSq = squareRaw(def.param0);
      const bool target = nearestEnemySq <= reachSq || incomingBanana(s, p, def.param0, threat);
      if (target && oneIn(rng, config.slamOneIn)) in.buttons |= bit;
      break;
    }
    case AbilityKind::Parry:
      // Parry an incoming banana that is close; the timing is left to luck.
      if (incomingBanana(s, p, config.parryRadius, threat) && rng.below(256) < config.parryPer256) {
        in.buttons |= bit;
      }
      break;
    case AbilityKind::Dash:
    case AbilityKind::Throw:
    case AbilityKind::Call:  // v0.35: now and then, the symbol being the aim
      if (oneIn(rng, config.abilityOneIn)) in.buttons |= bit;
      break;
    case AbilityKind::None:
    case AbilityKind::kCount:
      break;
  }
}

}  // namespace

PlayerInput scriptedInput(Rng& rng, const WorldState& s, const MatchContent& content, PlayerId id,
                          const ScriptedInputConfig& config) {
  const Player* self = s.playerBySlot(id);
  if (self == nullptr) return PlayerInput{};
  const Player& p = *self;
  const Vec2 pos = p.transform.pos;

  PlayerInput in;
  in.moveMag = 255;
  in.moveDir = wanderDirection(rng, p, content, config);

  // Out of ammo: go for the nearest banana lying within reach. Holding a
  // banana: go for the nearest power-up lying within reach (none in an arena
  // without spawners, so this never changes a power-up-free match).
  const bool wantBanana = p.ammo == 0;
  const PickupKind wanted = wantBanana ? PickupKind::Banana : PickupKind::PowerUp;
  uint64_t best = squareRaw(wantBanana ? config.pickupRadius : config.powerUpRadius);
  bool found = false;
  Vec2 target{};
  s.pickups.forEachAlive([&](EntityId /*kid*/, const Pickup& k) {
    if (k.kind != wanted) return;
    const uint64_t d = lengthSqRaw(k.pos - pos);
    if (d <= best) {
      best = d;
      target = k.pos;
      found = true;
    }
  });
  // Else the objective (v0.33), with the same hands-full condition. Harvest
  // (v0.82) steers by its own rule instead: routed over the cells in the
  // grid space, straight at the goal in the continuous one.
  if (content.mode.kind == ModeKind::Harvest) {
    const bool routed = content.mode.space == Space::Grid && gridHarvestSteer(s, content, p, in);
    bool stay = false;
    Vec2 goal{};
    if (!routed && harvestTarget(s, content, p, goal, stay)) {
      if (stay) {
        in.moveMag = 0;
      } else if (goal != pos) {
        in.moveDir = directionTo(pos, goal);
      }
    }
  } else {
    const bool chase = found && target != pos;
    if (chase || (!wantBanana && objectiveTarget(s, content, p, target) && target != pos)) {
      in.moveDir = directionTo(pos, target);
    }
  }

  // Aim at the nearest other alive player, with a little jitter.
  uint64_t nearest = std::numeric_limits<uint64_t>::max();  // stays max with nobody else alive
  Vec2 aimAt = pos + Vec2::fromAngle(p.transform.facing);
  Vec2 enemyAt{};
  s.players.forEachAlive([&](EntityId /*pid*/, const Player& other) {
    if (other.id == p.id) return;
    const uint64_t d = lengthSqRaw(other.transform.pos - pos);
    if (d < nearest) {
      nearest = d;
      aimAt = other.transform.pos;
      enemyAt = other.transform.pos;
    }
  });
  const int32_t jitter = rng.range(-int32_t{config.aimJitter}, int32_t{config.aimJitter});
  in.aimDir = static_cast<uint8_t>((int32_t{directionTo(pos, aimAt)} + jitter) & 0xFF);

  // Buttons: only meaningful while playing, and only as fresh edges.
  if (s.rules.phase == MatchPhase::Playing) {
    const uint8_t throwBit = buttonBit(Button::Throw);
    const uint8_t dashBit = buttonBit(Button::Dash);
    const bool throwUp = (p.prevButtons & throwBit) == 0;
    const bool dashUp = (p.prevButtons & dashBit) == 0;

    if (throwUp && p.ammo > 0 && ready(p.abilities[kAbilitySlotThrow]) &&
        oneIn(rng, config.throwOneIn)) {
      in.buttons |= throwBit;
    }
    // v0.48: a charging throw stays held now and then (no edge needed: the
    // Startup holds while the button is down).
    const AbilityState& thr = p.abilities[kAbilitySlotThrow];
    if (!throwUp && thr.phase == AbilityPhase::Startup &&
        throwCharges(content.db.abilityAt(thr.defIndex)) &&
        rng.below(256) < config.chargeHoldPer256) {
      in.buttons |= throwBit;
    }

    if (dashUp && ready(p.abilities[kAbilitySlotDash])) {
      // A non-own banana in flight, close and approaching: dash into it.
      const uint64_t threatSq = squareRaw(config.threatRadius);
      bool threat = false;
      Vec2 threatPos{};
      s.boomerangs.forEachAlive([&](EntityId /*bid*/, const Boomerang& b) {
        if (threat || b.phase == BoomerangPhase::Grounded || b.owner == p.id) return;
        const Vec2 rel = b.transform.pos - pos;
        if (lengthSqRaw(rel) <= threatSq && dot(rel, b.transform.vel) < Fixed::zero()) {
          threat = true;
          threatPos = b.transform.pos;
        }
      });
      // A close enemy and enough ammo for a dash attack: dash straight at it.
      const bool enemyClose = nearest <= squareRaw(config.dashAttackRadius);
      if (threat && rng.below(256) < config.dodgePer256) {
        in.buttons |= dashBit;
        if (threatPos != pos) in.moveDir = directionTo(pos, threatPos);
      } else if (enemyClose && p.ammo >= content.db.dashAttack.minAmmo &&
                 oneIn(rng, config.dashAttackOneIn)) {
        in.buttons |= dashBit;
        if (enemyAt != pos) in.moveDir = directionTo(pos, enemyAt);
      } else if (oneIn(rng, config.dashOneIn)) {
        in.buttons |= dashBit;
      }
    }
    useExtraAbility(rng, s, content, p, nearest, config, in);
  }
  if (in.buttons != 0) in.subTick = static_cast<uint8_t>(rng.below(256));
  return in;
}

GridStep gridStepToward(const ArenaDef& arena, Vec2 from, Fixed radius, Vec2 goal) {
  const GridRoutes routes(arena, from, radius);
  const uint16_t turns = routes.turnsTo(grid::cellOf(arena, goal));
  if (turns == kUnreached) return GridStep{};
  return GridStep{.reachable = true,
                  .turns = turns,
                  .octant = turns > 0 ? routes.firstStepTo(grid::cellOf(arena, goal)) : uint8_t{0}};
}

InputFrame scriptedFrame(Rng& rng, const WorldState& s, const MatchContent& content,
                         const ScriptedInputConfig& config) {
  InputFrame f;
  f.tick = s.tick;
  f.present = s.rules.present;
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const PlayerId id{i};
    if (maskHas(f.present, id)) f.inputs[i] = scriptedInput(rng, s, content, id, config);
  }
  return f;
}

}  // namespace kfm::sim
