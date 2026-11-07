// The reference bot (see reference_bot.hpp). Reads only the Observation;
// fixed-point arithmetic only; every Rng draw happens in a fixed order per
// decision, so the bot is a pure function of its seed and observations.
#include "kfm/ai/reference_bot.hpp"

#include "bot_math.hpp"
#include "kfm/ai/vision.hpp"
#include "kfm/sim/content/arena.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/spaces/grid_cells.hpp"
#include "kfm/sim/tags.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace kfm::ai {
namespace {

using botmath::angleOf;
using botmath::directionTo;
using botmath::segmentMinSq;
using botmath::squareRaw;
using botmath::unitOf;
using botmath::vec;

// Tuning (arena units, ticks). Tests pin the behaviour these give, not the numbers.
constexpr Fixed kHitSafety = Fixed::fromRaw(Fixed::kOneRaw / 8);    // margin on a contact
constexpr Fixed kCatchMargin = Fixed::fromRaw(Fixed::kOneRaw / 8);  // a catch must clearly connect
constexpr Fixed kNearMiss = Fixed::one();             // closer than contact + this is penalised
constexpr Fixed kHitPenalty = Fixed::fromInt(1000);   // a predicted contact
constexpr Fixed kHitThreshold = Fixed::fromInt(500);  // danger at or above: some path hits
constexpr Fixed kPreferredRange = Fixed::fromInt(6);  // armed: distance kept to the target
constexpr Fixed kFleeRange = Fixed::fromInt(7);       // unarmed: no gain beyond this distance
constexpr Fixed kPowerUpRadius = Fixed::fromInt(10);  // power-ups worth a detour
constexpr Fixed kWallMargin = Fixed::fromInt(2);      // outer walls: penalised closer than this
constexpr Fixed kHysteresis = Fixed::fromRaw(Fixed::kOneRaw / 4);  // keep the last direction
constexpr uint8_t kGoalTick = 16;      // where the goal is measured on a path
constexpr uint8_t kStrikeTick = 8;     // where the distance to dash-ready enemies is measured
constexpr uint8_t kDirections = 16;    // stick directions considered
constexpr uint32_t kMaxThrowHold = 2;  // a standoff's random hold before a lined-up throw
// v0.57: walled off from its target this long, an armed bot throws anyway (a
// bank shot off the wall beats two mirrored bots waiting for ever).
constexpr uint16_t kStandoffTicks = 180;
// v0.48: the speed multiplier a charged throw adds beyond what its reach needs
// (a faster banana leaves less time to dodge).
constexpr Fixed kChargeMargin = Fixed::fromRaw(Fixed::kOneRaw * 2 / 5);
// v0.35, the grid: the fighting range and the throw cap in cells, the turns
// between two calls, how long a sighting or a call stays a goal, the turns
// between two quarter turns of the sweep.
constexpr int32_t kGridPreferredCells = 2;
constexpr int32_t kGridThrowCells = 3;
constexpr uint32_t kCallEveryTurns = 8;
constexpr uint32_t kSightingTicks = 600;
constexpr uint32_t kSweepTurns = 4;

[[nodiscard]] bool ready(const ObsAbility& a, uint8_t blockedButtons) {
  return a.kind != static_cast<uint8_t>(ObsAbilityKind::None) &&
         a.phase == static_cast<uint8_t>(ObsAbilityPhase::Idle) && a.cooldownTicks == 0 &&
         (blockedButtons & a.button) == 0;
}

[[nodiscard]] Vec2 posOf(const ObsPlayer& p) {
  return vec(p.posX, p.posY);
}
[[nodiscard]] Vec2 velOf(const ObsPlayer& p) {
  return vec(p.velX, p.velY);
}

[[nodiscard]] bool hasTag(uint32_t tags, uint32_t bit) {
  return (tags & (1u << bit)) != 0;
}
constexpr uint32_t kTagInvulnerable = 4;  // sim::Tag::Invulnerable
constexpr uint32_t kTagShielded = 3;      // sim::Tag::Shielded
constexpr uint32_t kTagFriendlyFire = 8;  // sim::Tag::FriendlyFire (v0.32)
constexpr uint32_t kTagFalling = 10;      // sim::Tag::Falling (v0.57)
static_assert(kTagInvulnerable == static_cast<uint32_t>(sim::Tag::Invulnerable));
static_assert(kTagShielded == static_cast<uint32_t>(sim::Tag::Shielded));
static_assert(kTagFriendlyFire == static_cast<uint32_t>(sim::Tag::FriendlyFire));
static_assert(kTagFalling == static_cast<uint32_t>(sim::Tag::Falling));

[[nodiscard]] Fixed distanceBetween(Vec2 a, Vec2 b) {
  return length(b - a);
}

// Whether enemy `p` can use ability slot `slot` of kind `kind` within `ticks`
// ticks (idle and the cooldown running out by then).
[[nodiscard]] bool enemyReady(const ObsPlayer& p, uint32_t slot, ObsAbilityKind kind,
                              uint32_t ticks) {
  return p.abilityKind[slot] == static_cast<uint8_t>(kind) &&
         p.abilityPhase[slot] == static_cast<uint8_t>(ObsAbilityPhase::Idle) &&
         p.abilityCooldownTicks[slot] <= ticks;
}

// Ties are broken by the bot's own Rng, never by slot, banana or direction
// index: an index order is a fixed spatial or slot preference that two
// mirrored bots share. Reservoir sampling over the candidates that tie with
// the best: the k-th tie replaces the pick with chance 1/k, so every tied
// candidate is equally likely and the Rng is drawn only on a tie (a decision
// without ties draws nothing).
struct TieBreak {
  uint32_t ties{0};
  // Whether a candidate that ties with the current best replaces it.
  [[nodiscard]] bool replaces(Rng& rng) {
    ++ties;
    return rng.below(ties) == 0;
  }
  void reset() { ties = 1; }
};

// The nearest living enemy, preferring one without spawn protection; -1 when
// none. Equal distances: a random one of them.
[[nodiscard]] int pickTarget(const Observation& obs, Rng& rng) {
  const Vec2 me = vec(obs.self.posX, obs.self.posY);
  int best = -1;
  bool bestProtected = true;
  uint64_t bestD = UINT64_MAX;
  TieBreak tie;
  for (uint32_t i = 0; i < kObsMaxPlayers; ++i) {
    const ObsPlayer& p = obs.players[i];
    // v0.57: a monkey falling down a pit is already out.
    if (p.alive == 0 || p.relation != static_cast<uint8_t>(ObsRelation::Enemy) ||
        hasTag(p.tags, kTagFalling)) {
      continue;
    }
    const bool prot = hasTag(p.tags, kTagInvulnerable);
    const uint64_t d = lengthSqRaw(posOf(p) - me);
    bool take = (bestProtected && !prot) || (prot == bestProtected && d < bestD);
    if (take) {
      tie.reset();
    } else if (prot == bestProtected && d == bestD) {
      take = tie.replaces(rng);
    }
    if (take) {
      best = static_cast<int>(i);
      bestD = d;
      bestProtected = prot;
    }
  }
  return best;
}

// One tick of the own monkey: the outer box shrunk by the radius, and a
// sliding stop against the interior walls.
[[nodiscard]] Vec2 stepMonkey(const Observation& obs, Vec2 from, Vec2 vel) {
  const Fixed r = Fixed::fromRaw(obs.self.radius);
  const Vec2 lim = vec(obs.arena.halfExtents) - Vec2{r, r};
  const auto clampBox = [&](Vec2 p) {
    return Vec2{clamp(p.x, -lim.x, lim.x), clamp(p.y, -lim.y, lim.y)};
  };
  const auto blocked = [&](Vec2 p) {
    for (uint32_t w = 0; w < obs.arena.wallCount && w < kObsMaxWalls; ++w) {
      if (botmath::circleOverlaps(obs.arena.walls[w], p, r)) return true;
    }
    return false;
  };
  const Vec2 next = clampBox(from + vel);
  if (!blocked(next)) return next;
  const Vec2 xOnly = clampBox(Vec2{from.x + vel.x, from.y});
  if (!blocked(xOnly)) return xOnly;
  const Vec2 yOnly = clampBox(Vec2{from.x, from.y + vel.y});
  if (!blocked(yOnly)) return yOnly;
  return from;
}

// Whether a monkey of radius r at c would fall: its whole disc over the
// union of the arena's pits, the sim's own test (sim::discWithinBoxes,
// v0.57; a monkey not dashing there falls).
[[nodiscard]] bool inPit(const Observation& obs, Vec2 c, Fixed r) {
  return sim::discWithinBoxes(
      std::min<uint32_t>(obs.arena.hazardCount, kObsMaxHazards),
      [&obs](uint32_t i, Vec2& min, Vec2& max) {
        const ObsHazard& h = obs.arena.hazards[i];
        if (h.kind != static_cast<uint8_t>(ObsHazardKind::Pit)) return false;
        min = Vec2{Fixed::fromRaw(h.box.minX), Fixed::fromRaw(h.box.minY)};
        max = Vec2{Fixed::fromRaw(h.box.maxX), Fixed::fromRaw(h.box.maxY)};
        return true;
      },
      c, r);
}

// Whether a banana of radius r flies from `from` to `to` clear of the
// interior walls (the line sampled every half unit).
[[nodiscard]] bool lineClear(const Observation& obs, Vec2 from, Vec2 to, Fixed r) {
  const int32_t samples = std::max<int32_t>(1, (distanceBetween(from, to) * 2).ceilToInt());
  for (int32_t s = 1; s <= samples; ++s) {
    const Vec2 pt = from + ((to - from) * Fixed::fromRatio(s, samples));
    for (uint32_t w = 0; w < obs.arena.wallCount && w < kObsMaxWalls; ++w) {
      if (botmath::circleOverlaps(obs.arena.walls[w], pt, r)) return false;
    }
  }
  return true;
}

}  // namespace

ReferenceBot::ReferenceBot(const ReferenceBotConfig& config) : config_(config) {
  config_.reactionDelayTicks = std::min(config_.reactionDelayTicks, kMaxReactionDelayTicks);
}

void ReferenceBot::init(uint64_t seed, PlayerId slot) {
  rng_ = Rng::seeded(seed ^ (uint64_t{slot.value} << 56));
  slot_ = slot;
  lastDecidedButtons_ = 0;
  lastMoveDir_ = 0;
  decisions_ = 0;
  throwHold_ = -1;
  chargeHold_ = 0;
  chargeSpeed_ = Fixed{};
  pending_.fill(sim::PlayerInput{});
  cellSize_ = Fixed::one();
  bananaSpeed_ = Fixed{};
  gridCols_ = 0;
  gridRows_ = 0;
  gridCell_ = Fixed{};
  distGoal_ = -1;
  lastSeen_ = Vec2{};
  lastSeenTick_ = 0;
  haveLastSeen_ = false;
  lastCallTick_ = 0;
  called_ = false;
}

void ReferenceBot::learn(const Observation& obs, int target) {
  const ObsSelf& me = obs.self;
  const ObsPoint& half = obs.arena.halfExtents;
  // The cell size, from the own cell while at rest on its centre: x + hx =
  // (col + 1/2)·cell, and the same down the rows; the two must agree.
  if (obs.match.reservedTurnTicks > 0 && me.alive != 0 && me.velX == 0 && me.velY == 0 &&
      me.posX == me.prevPosX && me.posY == me.prevPosY && me.reservedCellX >= 0 &&
      me.reservedCellY >= 0) {
    const int64_t cx = ((int64_t{me.posX} + half.x) * 2) / ((2 * int64_t{me.reservedCellX}) + 1);
    const int64_t cy = ((int64_t{half.y} - me.posY) * 2) / ((2 * int64_t{me.reservedCellY}) + 1);
    if (cx > 0 && cx < INT32_MAX && cx - cy <= 2 && cy - cx <= 2) {
      cellSize_ = Fixed::fromRaw(static_cast<int32_t>(cx));
    }
  }
  // The flight speed: Observation v1 has only the base one, a mode's
  // baseline scales every banana alike; the own banana's wins.
  for (const ObsBanana& b : obs.bananas) {
    if (b.valid == 0 || b.speed <= 0 || b.phase >= static_cast<uint8_t>(ObsBananaPhase::Grounded)) {
      continue;
    }
    if (b.ownerRelation == static_cast<uint8_t>(ObsRelation::Self) || bananaSpeed_ == Fixed{}) {
      bananaSpeed_ = Fixed::fromRaw(b.speed);
    }
  }
  if (target >= 0) {
    lastSeen_ = posOf(obs.players[target]);
    lastSeenTick_ = obs.match.tick;
    haveLastSeen_ = true;
    return;
  }
  // An ally's call: the coarse cell of an enemy it saw (this bot's encoding).
  for (const ObsHeardCall& c : obs.reservedHeardCalls) {
    if (c.valid == 0 || c.slot >= kObsMaxPlayers ||
        obs.players[c.slot].relation != static_cast<uint8_t>(ObsRelation::Ally) ||
        (haveLastSeen_ && c.tick <= lastSeenTick_)) {
      continue;
    }
    const int32_t col = (2 * int32_t{static_cast<uint8_t>(c.symbol & 15u)}) + 1;
    const int32_t row = (2 * int32_t{static_cast<uint8_t>(c.symbol >> 4)}) + 1;
    lastSeen_ = Vec2{(cellSize_ * col) - Fixed::fromRaw(half.x),
                     Fixed::fromRaw(half.y) - (cellSize_ * row)};
    lastSeenTick_ = c.tick;
    haveLastSeen_ = true;
  }
}

void ReferenceBot::prepareGrid(const Observation& obs) {
  if (gridCell_ == cellSize_ && gridCols_ > 0) return;
  gridCell_ = cellSize_;
  distGoal_ = -1;
  const Fixed c = cellSize_;
  const Fixed hx = Fixed::fromRaw(obs.arena.halfExtents.x);
  const Fixed hy = Fixed::fromRaw(obs.arena.halfExtents.y);
  const Fixed r = Fixed::fromRaw(obs.self.radius);
  gridCols_ = ((hx * 2) / c).floorToInt();
  gridRows_ = ((hy * 2) / c).floorToInt();
  if (gridCols_ <= 0 || gridRows_ <= 0 || gridCols_ * gridRows_ > kMaxGridCells) {
    gridCols_ = 0;
    return;
  }
  for (int32_t row = 0; row < gridRows_; ++row) {
    for (int32_t col = 0; col < gridCols_; ++col) {
      const Vec2 at{(c * col) + (c / 2) - hx, hy - (c * row) - (c / 2)};
      bool free = abs(at.x) <= hx - r && abs(at.y) <= hy - r && !inPit(obs, at, r);
      for (uint32_t w = 0; free && w < obs.arena.wallCount && w < kObsMaxWalls; ++w) {
        if (botmath::circleOverlaps(obs.arena.walls[w], at, r)) free = false;
      }
      const int32_t i = (row * gridCols_) + col;
      gridFree_[static_cast<size_t>(i)] = free ? 1 : 0;
    }
  }
}

int32_t ReferenceBot::cellIndexOf(Vec2 at) const {
  if (gridCols_ <= 0) return -1;
  const Fixed hx = Fixed::fromRaw(gridCols_ * cellSize_.raw / 2);
  const Fixed hy = Fixed::fromRaw(gridRows_ * cellSize_.raw / 2);
  const int32_t col = std::clamp(((at.x + hx) / cellSize_).floorToInt(), 0, gridCols_ - 1);
  const int32_t row = std::clamp(((hy - at.y) / cellSize_).floorToInt(), 0, gridRows_ - 1);
  return (row * gridCols_) + col;
}

void ReferenceBot::distancesTo(Vec2 goal) {
  const int32_t g = cellIndexOf(goal);
  if (g < 0 || g == distGoal_) return;
  distGoal_ = g;
  const int32_t count = gridCols_ * gridRows_;
  const auto cells = static_cast<size_t>(count);
  std::fill_n(gridDist_.begin(), cells, uint16_t{0xFFFF});
  size_t head = 0;
  size_t tail = 0;
  gridDist_[static_cast<size_t>(g)] = 0;
  gridQueue_[tail++] = static_cast<uint16_t>(g);
  const auto freeAt = [&](int32_t col, int32_t row) {
    const int32_t i = (row * gridCols_) + col;
    return col >= 0 && row >= 0 && col < gridCols_ && row < gridRows_ &&
           gridFree_[static_cast<size_t>(i)] != 0;
  };
  while (head < tail) {
    const int32_t at = gridQueue_[head++];
    const int32_t col = at % gridCols_;
    const int32_t row = at / gridCols_;
    for (uint8_t o = 0; o < sim::grid::kOctants; ++o) {
      const int32_t nc = col + kStepCol[o];
      const int32_t nr = row + kStepRow[o];
      if (!freeAt(nc, nr)) continue;
      if (sim::grid::diagonal(o) && (!freeAt(nc, row) || !freeAt(col, nr))) continue;
      const int32_t ni = (nr * gridCols_) + nc;
      const auto n = static_cast<size_t>(ni);
      if (gridDist_[n] != 0xFFFF) continue;
      gridDist_[n] = static_cast<uint16_t>(gridDist_[static_cast<size_t>(at)] + 1);
      gridQueue_[tail++] = static_cast<uint16_t>(n);
    }
  }
}

Fixed ReferenceBot::walkDistance(Vec2 at) const {
  const int32_t i = cellIndexOf(at);
  if (i < 0 || distGoal_ < 0) return Fixed{};
  const uint16_t d = gridDist_[static_cast<size_t>(i)];
  return cellSize_ * (d == 0xFFFF ? gridCols_ + gridRows_ : int32_t{d});
}

void ReferenceBot::gridWalkPath(const Observation& obs, uint8_t octant, bool idle, Path& p) const {
  // The slide in progress runs until the next latch (or the host delay);
  // then a step to the octant's neighbour every turn, taken only when that
  // cell is open for the monkey's circle (the outer box, the walls, and for
  // a diagonal both orthogonal neighbours), else it stays. v0.57: for the
  // first step, the one the bot commits to now, a pit cell is open as in the
  // sim's latch, so the path walks in and pitDanger scores the fall (modelled
  // as staying, a step into a pit tied with idle and the hysteresis could
  // pick it: about half the grid bots' deaths). Later steps, which the bot
  // will choose again, stop at a pit as before: else every straight line
  // that meets a pit some cells on would be scored as a fall.
  const uint32_t turn = obs.match.reservedTurnTicks > 0 ? obs.match.reservedTurnTicks : 1u;
  const uint8_t start = static_cast<uint8_t>(std::min<uint32_t>(
      std::max<uint32_t>(obs.match.inputDelayTicks, obs.match.reservedTicksToLatch), kHorizon / 2));
  const Vec2 cur = vec(obs.self.velX, obs.self.velY);
  const Fixed c = cellSize_;
  const Fixed r = Fixed::fromRaw(obs.self.radius);
  const Fixed hx = Fixed::fromRaw(obs.arena.halfExtents.x);
  const Fixed hy = Fixed::fromRaw(obs.arena.halfExtents.y);
  const auto centreOf = [&](Vec2 at) {
    const int32_t col = ((at.x + hx) / c).floorToInt();
    const int32_t row = ((hy - at.y) / c).floorToInt();
    return Vec2{(c * col) + (c / 2) - hx, hy - (c * row) - (c / 2)};
  };
  bool first = true;
  const auto open = [&](Vec2 at) {
    if (abs(at.x) > hx - r || abs(at.y) > hy - r || (!first && inPit(obs, at, r))) return false;
    for (uint32_t w = 0; w < obs.arena.wallCount && w < kObsMaxWalls; ++w) {
      if (botmath::circleOverlaps(obs.arena.walls[w], at, r)) return false;
    }
    return true;
  };
  const Vec2 dc{c * kStepCol[octant & 7u], Fixed{}};
  const Vec2 dr{Fixed{}, c * -kStepRow[octant & 7u]};
  p.pos[0] = vec(obs.self.posX, obs.self.posY);
  uint8_t k = 1;
  for (; k <= start && k <= kHorizon; ++k)
    p.pos[k] = stepMonkey(obs, p.pos[k - 1], cur);
  Vec2 at = centreOf(p.pos[k - 1]);
  while (k <= kHorizon) {
    Vec2 to = at;
    if (!idle && open(at + dc + dr) &&
        (!sim::grid::diagonal(octant) || (open(at + dc) && open(at + dr)))) {
      to = at + dc + dr;
    }
    for (uint32_t j = 1; j <= turn && k <= kHorizon; ++j, ++k) {
      p.pos[k] =
          at + ((to - at) * Fixed::fromRatio(static_cast<int32_t>(j), static_cast<int32_t>(turn)));
    }
    at = to;
    first = false;
  }
}

std::optional<sim::PlayerInput> ReferenceBot::act(const Observation& obs, Tick /*tick*/) {
  if (obs.header.version != kObservationVersion || obs.header.size != sizeof(Observation)) {
    return sim::PlayerInput{};
  }
  sim::PlayerInput in = decide(obs);
  // A press is a fresh edge: a button set in the previous decision is released.
  in.buttons = static_cast<uint8_t>(in.buttons & ~lastDecidedButtons_);
  lastDecidedButtons_ = in.buttons;
  if (in.moveMag != 0) lastMoveDir_ = in.moveDir;

  const uint32_t line = uint32_t{config_.reactionDelayTicks} + 1;
  pending_[decisions_ % line] = in;
  sim::PlayerInput out{};
  if (decisions_ >= config_.reactionDelayTicks) {
    out = pending_[(decisions_ - config_.reactionDelayTicks) % line];
  }
  ++decisions_;
  return out;
}

void ReferenceBot::predictBanana(const Observation& obs, const ObsBanana& b, Track& t) {
  const ObsRules& rules = obs.rules;
  Vec2 pos = vec(b.posX, b.posY);
  Vec2 vel = vec(b.velX, b.velY);
  Angle heading = Angle::fromBam(b.headingBam);
  auto phase = static_cast<ObsBananaPhase>(b.phase);
  uint16_t inPhase = b.ticksInPhase;
  const Fixed speed = Fixed::fromRaw(b.speed);
  const Fixed restitution = Fixed::fromRaw(rules.bananaRestitution);
  const Vec2 lim = vec(obs.arena.halfExtents) - Vec2{t.radius, t.radius};
  const bool ownerAlive = b.owner < kObsMaxPlayers && obs.players[b.owner].alive != 0;
  const Vec2 ownerPos = ownerAlive ? posOf(obs.players[b.owner]) : Vec2{};
  // v0.35, fog: an owner alive (the roster's aliveMask) but unseen keeps its
  // banana flying; only a gone owner grounds it. Without fog the two agree.
  const bool ownerHidden =
      !ownerAlive && b.owner < kObsMaxPlayers && ((obs.match.aliveMask >> b.owner) & 1u) != 0;
  t.pos[0] = pos;
  t.harmlessFrom = kHorizon + 1;
  for (uint8_t k = 1; k <= kHorizon; ++k) {
    if (phase != ObsBananaPhase::Grounded) {
      if (inPhase < UINT16_MAX) ++inPhase;
      if (phase == ObsBananaPhase::Outbound && inPhase >= rules.bananaOutboundTicks) {
        phase = ObsBananaPhase::Returning;
        inPhase = 0;
      }
      if (phase == ObsBananaPhase::Returning) {
        if (ownerHidden && inPhase <= rules.bananaMaxReturnTicks) {
          // Unknown homing: straight on.
        } else if (!ownerAlive || inPhase > rules.bananaMaxReturnTicks) {
          phase = ObsBananaPhase::Grounded;
          vel = Vec2{};
          if (t.harmlessFrom > kHorizon) t.harmlessFrom = k;
        } else {
          if (ownerPos != pos) {
            const int32_t rate = rules.bananaTurnRateBam;
            const int32_t delta =
                std::clamp<int32_t>(angleOf(ownerPos - pos).signedDeltaFrom(heading), -rate, rate);
            heading += Angle::fromBam(static_cast<uint16_t>(delta));
          }
          vel = Vec2::fromAngle(heading, speed);
        }
      }
      pos += vel;
      // The outer box mirrors the overshoot (interior walls are not predicted).
      bool bounced = false;
      if (pos.x > lim.x || pos.x < -lim.x) {
        const Fixed edge = pos.x > lim.x ? lim.x : -lim.x;
        pos.x = edge + edge - pos.x;
        vel.x = -vel.x * restitution;
        bounced = true;
      }
      if (pos.y > lim.y || pos.y < -lim.y) {
        const Fixed edge = pos.y > lim.y ? lim.y : -lim.y;
        pos.y = edge + edge - pos.y;
        vel.y = -vel.y * restitution;
        bounced = true;
      }
      if (bounced && vel != Vec2{}) heading = angleOf(vel);
    }
    t.pos[k] = pos;
  }
}

void ReferenceBot::trackThreats(const Observation& obs) {
  // Harmful bananas: in flight and not the own (an own outbound banana flies
  // through its thrower, an own returning one is caught on contact). The
  // nearest kMaxTracks win the slots.
  const Vec2 me = vec(obs.self.posX, obs.self.posY);
  trackCount_ = 0;
  std::array<uint64_t, kMaxTracks> dist{};
  const auto add = [&](uint64_t d) -> Track* {
    if (trackCount_ < kMaxTracks) {
      dist[trackCount_] = d;
      return &tracks_[trackCount_++];
    }
    uint8_t worst = 0;
    for (uint8_t i = 1; i < kMaxTracks; ++i) {
      if (dist[i] > dist[worst]) worst = i;
    }
    if (d >= dist[worst]) return nullptr;
    dist[worst] = d;
    return &tracks_[worst];
  };
  // v0.32: a teammate's banana passes through unless friendly fire is on
  // (the mode's baseline grants the tag), so it is no threat to track.
  const bool friendlyFire = hasTag(obs.self.tags, kTagFriendlyFire);
  for (const ObsBanana& b : obs.bananas) {
    // Grounded, or (v0.57) sinking into a pit: harmless.
    if (b.valid == 0 || b.phase >= static_cast<uint8_t>(ObsBananaPhase::Grounded) ||
        b.ownerRelation == static_cast<uint8_t>(ObsRelation::Self) ||
        (b.ownerRelation == static_cast<uint8_t>(ObsRelation::Ally) && !friendlyFire)) {
      continue;
    }
    const uint64_t d = lengthSqRaw(vec(b.posX, b.posY) - me);
    Track* t = add(d);
    if (t == nullptr) continue;
    t->radius = Fixed::fromRaw(b.radius);
    t->catchable = true;
    predictBanana(obs, b, *t);
  }
  // A dashing enemy holding the ammo that arms a dash attack strikes on contact.
  for (const ObsPlayer& p : obs.players) {
    if (p.alive == 0 || p.relation != static_cast<uint8_t>(ObsRelation::Enemy) || p.dashing == 0 ||
        p.ammo < obs.rules.dashAttackMinAmmo) {
      continue;
    }
    Track* t = add(lengthSqRaw(posOf(p) - me));
    if (t == nullptr) continue;
    // v0.34: the radius is a derived stat; Observation v1 carries only our
    // own, and a mode's baseline gives every monkey the same one.
    t->radius = Fixed::fromRaw(obs.self.radius);
    t->catchable = false;
    const uint16_t active = p.abilityPhase[0] == static_cast<uint8_t>(ObsAbilityPhase::Active)
                                ? p.abilityPhaseTicks[0]
                                : 0;
    const uint16_t dashTicks = obs.self.abilities[0].defActiveTicks;  // the same loadout as ours
    const uint32_t left = dashTicks > active ? uint32_t{dashTicks} - active : 0;
    t->harmlessFrom = static_cast<uint8_t>(std::min<uint32_t>(left + 1, kHorizon + 1));
    Vec2 pos = posOf(p);
    t->pos[0] = pos;
    for (uint8_t k = 1; k <= kHorizon; ++k) {
      if (k <= left) pos += velOf(p);
      t->pos[k] = pos;
    }
  }
}

// The paths: ticks 1..lead keep the current velocity (inputs already on their
// way), the plan's motion starts after.
void ReferenceBot::walkPath(const Observation& obs, uint8_t dir, bool idle, Path& p) {
  const uint8_t lead = std::min<uint8_t>(obs.match.inputDelayTicks, kHorizon / 2);
  const Vec2 cur = vec(obs.self.velX, obs.self.velY);
  const Vec2 v = idle ? Vec2{} : unitOf(dir) * Fixed::fromRaw(obs.self.moveSpeed);
  p.pos[0] = vec(obs.self.posX, obs.self.posY);
  for (uint8_t k = 1; k <= kHorizon; ++k) {
    p.pos[k] = stepMonkey(obs, p.pos[k - 1], k <= lead ? cur : v);
  }
}

void ReferenceBot::dashPath(const Observation& obs, uint8_t dir, Path& p) {
  const uint8_t lead = std::min<uint8_t>(obs.match.inputDelayTicks, kHorizon / 2);
  const ObsAbility& dash = obs.self.abilities[0];
  const Vec2 cur = vec(obs.self.velX, obs.self.velY);
  Vec2 v = unitOf(dir) * Fixed::fromRaw(dash.defSpeed);
  // v0.57, the grid: a dash commits to whole cells along its octant, so a
  // diagonal one covers its dash cells on both axes, not along the diagonal
  // (the dash def's speed x active ticks is the cells' length); a pit check
  // on the old straight line missed the diagonal landing cell.
  if (obs.match.reservedTurnTicks > 0) {
    const uint8_t o = sim::grid::octantOf(dir);
    v = Vec2{Fixed::fromInt(kStepCol[o]), Fixed::fromInt(-kStepRow[o])} *
        Fixed::fromRaw(dash.defSpeed);
  }
  p.pos[0] = vec(obs.self.posX, obs.self.posY);
  for (uint8_t k = 1; k <= kHorizon; ++k) {
    Vec2 step{};
    if (k <= lead) {
      step = cur;
    } else if (k <= lead + dash.defActiveTicks) {
      step = v;
    }
    p.pos[k] = stepMonkey(obs, p.pos[k - 1], step);
  }
}

void ReferenceBot::blinkPath(const Observation& obs, uint8_t dir, Path& p) {
  const uint8_t lead = std::min<uint8_t>(obs.match.inputDelayTicks, kHorizon / 2);
  const ObsAbility& blink = obs.self.abilities[2];
  const Vec2 cur = vec(obs.self.velX, obs.self.velY);
  const Vec2 walk = unitOf(dir) * Fixed::fromRaw(obs.self.moveSpeed);
  const Vec2 jump = unitOf(dir) * (Fixed::fromRaw(blink.defSpeed) * int32_t{blink.defActiveTicks});
  const uint32_t jumpAt = uint32_t{lead} + blink.defStartupTicks + 1;
  p.pos[0] = vec(obs.self.posX, obs.self.posY);
  for (uint8_t k = 1; k <= kHorizon; ++k) {
    Vec2 step{};
    if (k <= lead) {
      step = cur;
    } else if (k < jumpAt) {
      step = walk;
    } else if (k == jumpAt) {
      step = jump;  // the jump passes everything on its way (§5.5)
    }
    p.pos[k] = stepMonkey(obs, p.pos[k - 1], step);
  }
}

ReferenceBot::Outcome ReferenceBot::evaluate(const Observation& obs, const Path& p,
                                             uint8_t catchFrom, uint8_t catchUntil) const {
  Outcome o;
  const Fixed myRadius = Fixed::fromRaw(obs.self.radius);
  for (uint8_t i = 0; i < trackCount_; ++i) {
    const Track& t = tracks_[i];
    const Fixed contact = myRadius + t.radius;
    const uint64_t hitSq = squareRaw(contact + kHitSafety);
    const uint64_t catchSq = squareRaw(contact - kCatchMargin);
    const uint64_t nearSq = squareRaw(contact + kNearMiss);
    for (uint8_t k = 1; k <= kHorizon && k < t.harmlessFrom; ++k) {
      const Vec2 r = t.pos[k - 1] - p.pos[k - 1];
      const Vec2 d = (t.pos[k] - t.pos[k - 1]) - (p.pos[k] - p.pos[k - 1]);
      const uint64_t sq = segmentMinSq(r, d);
      if (t.catchable && k >= catchFrom && k < catchUntil) {
        // Inside the perfect window a contact is a catch: the banana is gone.
        if (sq < catchSq) {
          o.catches = true;
          break;
        }
        continue;
      }
      if (sq < hitSq) {
        // Sooner is worse: later predictions are less certain.
        o.danger += kHitPenalty + Fixed::fromInt(16 * (kHorizon - k));
        if (o.firstHit == 0 || k < o.firstHit) o.firstHit = k;
        break;
      }
      if (sq < nearSq) {
        const Fixed dist = Fixed::fromRaw(static_cast<int32_t>(isqrt(sq)));
        o.danger += (contact + kNearMiss - dist) * 2;
      }
    }
  }
  return o;
}

Fixed ReferenceBot::pitDanger(const Observation& obs, const Path& p, uint8_t airFrom,
                              uint8_t airUntil) {
  if (obs.arena.hazardCount == 0) return Fixed{};
  const Fixed r = Fixed::fromRaw(obs.self.radius);
  for (uint8_t k = 1; k <= kHorizon; ++k) {
    if (k >= airFrom && k < airUntil) continue;
    if (inPit(obs, p.pos[k], r)) return kHitPenalty + Fixed::fromInt(16 * (kHorizon - k));
  }
  return Fixed{};
}

sim::PlayerInput ReferenceBot::decide(const Observation& obs) {
  sim::PlayerInput in{};
  const ObsSelf& me = obs.self;
  const Vec2 myPos = vec(me.posX, me.posY);
  const int target = pickTarget(obs, rng_);
  // v0.35: the grid and the fog (the turn clock is 0 in a continuous mode).
  const bool grid = obs.match.reservedTurnTicks > 0;
  const bool fog = (obs.header.schemaFlags & kObsFlagFog) != 0;
  const uint32_t turn = grid ? obs.match.reservedTurnTicks : 1u;
  if (grid || fog) learn(obs, target);
  in.aimDir = me.facing;
  if (target >= 0) in.aimDir = directionTo(myPos, posOf(obs.players[target]));
  // Nobody in sight under fog or on the grid: the last sighting (or an
  // ally's call), else the centre; the cone turns toward it.
  bool explore = false;
  Vec2 exploreAt{};
  if (target < 0 && (grid || fog)) {
    if (haveLastSeen_ && obs.match.tick - lastSeenTick_ > kSightingTicks) haveLastSeen_ = false;
    if (haveLastSeen_ && distanceBetween(myPos, lastSeen_) <= cellSize_) haveLastSeen_ = false;
    explore = true;
    exploreAt = haveLastSeen_ ? lastSeen_ : Vec2{};
    if (distanceBetween(myPos, exploreAt) > cellSize_) {
      in.aimDir = directionTo(myPos, exploreAt);
    } else {
      in.aimDir = static_cast<uint8_t>(((obs.match.tick / (kSweepTurns * turn)) & 3u) * 64u);
    }
  }
  if (me.alive == 0 || hasTag(me.tags, kTagFalling) ||
      obs.match.phase != static_cast<uint8_t>(ObsMatchPhase::Playing)) {
    throwHold_ = -1;
    chargeHold_ = 0;
    return in;
  }

  trackThreats(obs);
  const uint8_t lead = std::min<uint8_t>(obs.match.inputDelayTicks, kHorizon / 2);
  const bool armed = me.ammo > 0;

  // --- movement: the best of 16 directions and idle -------------------------
  // Goal points, measured kGoalTick ticks into each path.
  const ObsPlayer* tgt = target >= 0 ? &obs.players[target] : nullptr;
  // Where a path's goal is measured: kGoalTick ticks, on the grid two turns
  // after the next latch.
  // v0.57: an armed bot with a wall between it and its target looks to the
  // end of the horizon, far enough to see round a block (at kGoalTick no
  // walk leaves a 2-unit block's face band, and two such bots stood still).
  const bool walledOff =
      !grid && me.ammo > 0 && target >= 0 &&
      !lineClear(obs, myPos, posOf(obs.players[target]), Fixed::fromRaw(obs.rules.bananaRadius));
  walledTicks_ = walledOff ? static_cast<uint16_t>(std::min<uint32_t>(walledTicks_ + 1u, 65535u))
                           : uint16_t{0};
  const uint8_t continuousGoalTick = walledOff ? kHorizon : kGoalTick;
  const auto goalTick = static_cast<uint8_t>(
      grid ? std::clamp<uint32_t>(uint32_t{obs.match.reservedTicksToLatch} + (2 * turn), 1u,
                                  kHorizon)
           : continuousGoalTick);
  const Fixed preferredRange = grid ? cellSize_ * kGridPreferredCells : kPreferredRange;
  const Vec2 tgtAt = tgt != nullptr ? posOf(*tgt) + (velOf(*tgt) * int32_t{goalTick}) : Vec2{};
  bool haveFetch = false;
  Vec2 fetch{};
  Fixed fetchWeight = Fixed::one();
  if (!armed) {
    // The own banana on its way back, else the nearest banana lying around.
    uint64_t best = UINT64_MAX;
    for (const ObsBanana& b : obs.bananas) {
      if (b.valid == 0 || b.ownerRelation != static_cast<uint8_t>(ObsRelation::Self) ||
          b.phase >= static_cast<uint8_t>(ObsBananaPhase::Grounded)) {
        continue;
      }
      const uint64_t d = lengthSqRaw(vec(b.posX, b.posY) - myPos);
      if (d < best) {
        best = d;
        fetch = vec(b.posX, b.posY);
        haveFetch = true;
      }
    }
    if (!haveFetch) {
      for (const ObsPickup& k : obs.pickups) {
        // v0.57: a banana lying where standing would mean a fall (deep in
        // a pit) costs the monkey that fetches it; one near the edge is
        // reached from the floor.
        if (k.valid == 0 || k.kind != static_cast<uint8_t>(ObsPickupKind::Banana) ||
            inPit(obs, vec(k.posX, k.posY), Fixed::fromRaw(obs.self.radius))) {
          continue;
        }
        const uint64_t d = lengthSqRaw(vec(k.posX, k.posY) - myPos);
        if (d < best) {
          best = d;
          fetch = vec(k.posX, k.posY);
          haveFetch = true;
          fetchWeight = Fixed::fromRatio(3, 2);
        }
      }
    }
  }
  bool havePowerUp = false;
  Vec2 powerUp{};
  {
    uint64_t best = squareRaw(kPowerUpRadius);
    for (const ObsPickup& k : obs.pickups) {
      if (k.valid == 0 || k.kind != static_cast<uint8_t>(ObsPickupKind::PowerUp)) continue;
      const uint64_t d = lengthSqRaw(vec(k.posX, k.posY) - myPos);
      if (d < best) {
        best = d;
        powerUp = vec(k.posX, k.posY);
        havePowerUp = true;
      }
    }
  }
  // Armed enemies whose dash is ready: keep out of a dash attack's reach.
  const ObsAbility& myDash = me.abilities[0];
  const Fixed strikeReach = (Fixed::fromRaw(myDash.defSpeed) * int32_t{myDash.defActiveTicks}) +
                            (Fixed::fromRaw(obs.rules.playerRadius) * 2);
  const auto strikeDanger = [&](Vec2 at, uint8_t k) {
    Fixed u{};
    for (const ObsPlayer& e : obs.players) {
      if (e.alive == 0 || e.relation != static_cast<uint8_t>(ObsRelation::Enemy) ||
          e.ammo < obs.rules.dashAttackMinAmmo || e.ammo == 0 ||
          e.abilityKind[0] != static_cast<uint8_t>(ObsAbilityKind::Dash) ||
          e.abilityCooldownTicks[0] > k) {
        continue;
      }
      const Fixed d = distanceBetween(at, posOf(e) + (velOf(e) * int32_t{k}));
      if (d < strikeReach) u += (strikeReach - d) * 2;
    }
    return u;
  };
  const Vec2 half = vec(obs.arena.halfExtents);
  // v0.35, the grid: the walking distance (around walls) to what the bot
  // goes for: an armed bot's target, an unarmed one's banana, or where it
  // explores; walls make the straight distance a trap.
  bool walk = false;
  if (grid) {
    prepareGrid(obs);
    if (gridCols_ > 0) {
      if (armed && tgt != nullptr) {
        distancesTo(posOf(*tgt));
        walk = true;
      } else if (!armed && haveFetch) {
        distancesTo(fetch);
        walk = true;
      } else if (explore) {
        distancesTo(exploreAt);
        walk = true;
      }
    }
  }
  const auto goal = [&](const Path& p) {
    const Vec2 end = p.pos[goalTick];
    Fixed u{};
    if (tgt != nullptr) {
      const Fixed d = walk && armed ? walkDistance(end) : distanceBetween(end, tgtAt);
      u -= armed ? abs(d - preferredRange) : -min(d, kFleeRange) / 2;
      // An armed bot wants a clear line to its target (a wall between two
      // bots at fighting range is a standoff for ever): v0.35 on the grid,
      // v0.57 in the continuous space too (two mirrored bots had parked on
      // either side of island's central block for eight minutes).
      if (armed && !lineClear(obs, end, tgtAt, Fixed::fromRaw(obs.rules.bananaRadius))) {
        u -= preferredRange;
      }
    } else if (explore && (armed || !haveFetch)) {
      u -= walk ? walkDistance(end) : distanceBetween(end, exploreAt);
    }
    if (haveFetch) u -= (walk ? walkDistance(end) : distanceBetween(end, fetch)) * fetchWeight;
    if (havePowerUp) u -= distanceBetween(end, powerUp) / 2;
    // The wall band is penalised at 3 per unit, more than the fetch pull, so
    // it must never forbid the fetch point itself: a banana lying inside the
    // band would otherwise leave an unarmed bot parked at the band's edge
    // forever (two such bots stalled a round). The band ends where the
    // fetch lies, on each axis.
    Fixed limitX = half.x - kWallMargin;
    Fixed limitY = half.y - kWallMargin;
    if (haveFetch) {
      limitX = max(limitX, abs(fetch.x));
      limitY = max(limitY, abs(fetch.y));
    }
    const Fixed ex = abs(end.x) - limitX;
    const Fixed ey = abs(end.y) - limitY;
    if (ex > Fixed{}) u -= ex * 3;
    if (ey > Fixed{}) u -= ey * 3;
    u -= strikeDanger(p.pos[kStrikeTick], kStrikeTick);
    return u;
  };

  Fixed bestUtil = Fixed::min();
  Fixed bestDanger{};
  uint8_t bestFirstHit = 0;
  uint8_t bestDir = lastMoveDir_;
  bool bestIdle = true;
  TieBreak moveTie;
  // v0.57: the walk the goal would want most if it did not fall down a pit.
  Fixed pitWalkUtil = Fixed::min();
  uint8_t pitWalkDir = 0;
  bool pitWalk = false;
  // On the grid: the eight octants (moveDir 32·o is exactly octant o) and idle.
  const uint8_t directions = grid ? sim::grid::kOctants : kDirections;
  for (uint8_t c = 0; c <= directions; ++c) {
    const bool idle = c == directions;
    const auto dir = static_cast<uint8_t>(idle ? 0 : c * (256 / directions));
    if (grid) {
      gridWalkPath(obs, c, idle, path_);
    } else {
      walkPath(obs, dir, idle, path_);
    }
    const Outcome o = evaluate(obs, path_, 0, 0);
    Fixed util = goal(path_) - o.danger;
    if (!idle && dir == lastMoveDir_) util += kHysteresis;
    const Fixed fall = pitDanger(obs, path_, 0, 0);
    if (fall > Fixed{}) {
      if (!idle && o.danger < kHitThreshold && util > pitWalkUtil) {
        pitWalkUtil = util;
        pitWalkDir = dir;
        pitWalk = true;
      }
      util -= fall;
    }
    bool take = util > bestUtil;
    if (take) {
      moveTie.reset();
    } else if (util == bestUtil) {
      take = moveTie.replaces(rng_);
    }
    if (take) {
      bestUtil = util;
      bestDanger = o.danger;
      bestFirstHit = o.firstHit;
      bestDir = dir;
      bestIdle = idle;
    }
  }
  in.moveDir = bestDir;
  in.moveMag = bestIdle ? 0 : 255;

  const uint8_t blocked = lastDecidedButtons_;
  const ObsAbility& dash = me.abilities[0];
  const ObsAbility& thr = me.abilities[1];
  const ObsAbility& extra = me.abilities[2];
  const bool dashReady =
      ready(dash, blocked) && dash.kind == static_cast<uint8_t>(ObsAbilityKind::Dash);
  const bool canCatch = me.ammo < me.ammoMax;
  const auto press = [&](const ObsAbility& a, uint8_t dir) {
    in.buttons = static_cast<uint8_t>(in.buttons | a.button);
    in.moveDir = dir;
    in.moveMag = 255;
  };
  // A grid dash goes along the octant of its direction.
  const auto snap = [&](uint8_t dir) {
    return grid ? static_cast<uint8_t>(sim::grid::octantOf(dir) * 32u) : dir;
  };
  bool dashed = false;
  // A dash's active ticks on its path (dashPath): the press at lead + 1.
  const auto airFrom = static_cast<uint8_t>(lead + 1);
  const auto airUntil = static_cast<uint8_t>(
      std::min<uint32_t>(uint32_t{lead} + 1 + dash.defActiveTicks, uint32_t{kHorizon} + 1));

  // --- dash-catch -----------------------------------------------------------
  const auto catchFrom = static_cast<uint8_t>(lead + 1);
  const auto catchUntil = static_cast<uint8_t>(lead + 1 + dash.defPerfectWindowTicks);
  if (dashReady && canCatch && dash.defPerfectWindowTicks > 0) {
    bool found = false;
    uint8_t catchDir = 0;
    Fixed catchDanger = Fixed::max();
    for (uint8_t i = 0; i < trackCount_; ++i) {
      const Track& t = tracks_[i];
      if (!t.catchable) continue;
      for (uint8_t aim = catchFrom; aim < catchUntil && aim <= kHorizon; ++aim) {
        const uint8_t dir = snap(directionTo(myPos, t.pos[aim]));
        dashPath(obs, dir, path_);
        const Outcome o = evaluate(obs, path_, catchFrom, catchUntil);
        if (o.catches && o.danger < catchDanger &&
            pitDanger(obs, path_, airFrom, airUntil) == Fixed{}) {
          found = true;
          catchDir = dir;
          catchDanger = o.danger;
        }
      }
    }
    if (found && catchDanger < kHitThreshold && rng_.below(256) < config_.catchPer256) {
      press(dash, catchDir);
      dashed = true;
    }
  }

  // --- dodge when no walk escapes -------------------------------------------
  if (!dashed && bestDanger >= kHitThreshold) {
    if (dashReady) {
      Fixed best = bestDanger;
      bool found = false;
      uint8_t bestDash = 0;
      for (uint8_t c = 0; c < directions; ++c) {
        const auto dir = static_cast<uint8_t>(c * (256 / directions));
        dashPath(obs, dir, path_);
        const Outcome o = evaluate(obs, path_, canCatch ? catchFrom : 0, canCatch ? catchUntil : 0);
        if (o.danger + pitDanger(obs, path_, airFrom, airUntil) < best) {
          best = o.danger;
          bestDash = dir;
          found = true;
        }
      }
      if (found) {
        press(dash, bestDash);
        dashed = true;
      }
    } else if (ready(extra, blocked)) {
      const auto kind = static_cast<ObsAbilityKind>(extra.kind);
      const uint32_t startup = extra.defStartupTicks;
      if (kind == ObsAbilityKind::Blink) {
        Fixed best = bestDanger;
        bool found = false;
        uint8_t bestBlink = 0;
        for (uint8_t c = 0; c < kDirections; ++c) {
          const auto dir = static_cast<uint8_t>(c * (256 / kDirections));
          blinkPath(obs, dir, path_);
          const Outcome o = evaluate(obs, path_, 0, 0);
          if (o.danger + pitDanger(obs, path_, 0, 0) < best) {
            best = o.danger;
            bestBlink = dir;
            found = true;
          }
        }
        if (found) press(extra, bestBlink);
      } else if (kind == ObsAbilityKind::Parry && bestFirstHit > lead) {
        // The hit lands inside the guard's window from a press now.
        const uint32_t at = bestFirstHit - 1u - lead;
        if (at >= startup && at < startup + extra.defActiveTicks) {
          in.buttons = static_cast<uint8_t>(in.buttons | extra.button);
        }
      } else if (kind == ObsAbilityKind::Slam && bestFirstHit > lead + startup + 1) {
        // The shockwave comes before the hit and reaches the banana.
        walkPath(obs, bestDir, bestIdle, path_);
        const uint32_t impact = lead + startup + 1;
        const Fixed reach = Fixed::fromRaw(extra.defParam0);
        for (uint8_t i = 0; i < trackCount_; ++i) {
          if (tracks_[i].catchable &&
              lengthSqRaw(tracks_[i].pos[impact] - path_.pos[impact]) < squareRaw(reach)) {
            in.buttons = static_cast<uint8_t>(in.buttons | extra.button);
            break;
          }
        }
      }
    }
  }

  // --- dash across a pit (v0.57) ---------------------------------------------
  // The walk the goal wants most would fall in: a dash the same way that
  // lands clear of every pit and every banana, and scores better than the
  // walk chosen, takes it. The grid's walks never enter a pit, so this is
  // the continuous space's.
  if (!dashed && !grid && dashReady && pitWalk && pitWalkUtil > bestUtil &&
      bestDanger < kHitThreshold) {
    dashPath(obs, pitWalkDir, path_);
    const Outcome o = evaluate(obs, path_, 0, 0);
    if (o.danger < kHitThreshold && pitDanger(obs, path_, airFrom, airUntil) == Fixed{} &&
        goal(path_) - o.danger > bestUtil) {
      press(dash, pitWalkDir);
      dashed = true;
    }
  }

  // --- dash attack ----------------------------------------------------------
  // Only on a target that cannot answer with a dash attack of its own on the
  // same tick: two strikes pressed on one tick at the same sub-tick are
  // resolved in slot order (§5.11), a coin the sim tosses by slot index, not
  // a fight the bot can win. Unarmed targets and targets whose dash is busy
  // or cooling down are fair game.
  const bool counterable = tgt != nullptr && tgt->ammo >= obs.rules.dashAttackMinAmmo &&
                           tgt->ammo > 0 && enemyReady(*tgt, 0, ObsAbilityKind::Dash, lead);
  if (!dashed && dashReady && tgt != nullptr && !counterable &&
      me.ammo >= obs.rules.dashAttackMinAmmo && me.ammo > 0 && bestDanger < kHitThreshold &&
      !hasTag(tgt->tags, kTagInvulnerable) && !hasTag(tgt->tags, kTagShielded)) {
    const Fixed strikeRange = (Fixed::fromRaw(obs.rules.playerRadius) * 2) - kCatchMargin;
    const uint64_t strikeSq = squareRaw(strikeRange);
    const Vec2 tp = posOf(*tgt);
    const Vec2 tv = velOf(*tgt);
    const uint8_t active =
        static_cast<uint8_t>(std::min<uint32_t>(dash.defActiveTicks, uint32_t{kHorizon} - lead));
    bool found = false;
    uint8_t strikeDir = 0;
    for (uint8_t aim = 1; aim <= active && !found; ++aim) {
      const uint8_t dir = snap(directionTo(myPos, tp + (tv * int32_t{lead + aim})));
      dashPath(obs, dir, path_);
      for (uint32_t k = uint32_t{lead} + 1; k <= uint32_t{lead} + active; ++k) {
        const Vec2 e0 = tp + (tv * static_cast<int32_t>(k - 1));
        const Vec2 r = e0 - path_.pos[k - 1];
        const Vec2 d = tv - (path_.pos[k] - path_.pos[k - 1]);
        if (segmentMinSq(r, d) < strikeSq) {
          found = evaluate(obs, path_, 0, 0).danger < kHitThreshold &&
                  pitDanger(obs, path_, airFrom, airUntil) == Fixed{};
          strikeDir = dir;
          break;
        }
      }
    }
    if (found && rng_.below(256) < config_.throwPer256) press(dash, strikeDir);
  }

  // --- throw ----------------------------------------------------------------
  const ObsRules& r = obs.rules;
  const Fixed offset = Fixed::fromRaw(r.bananaSpawnOffset);
  // Where to aim for a banana at `speed` leaving in `windUp` ticks: the
  // target's position extrapolated over the wind-up and the flight.
  const auto leadAim = [&](const ObsPlayer& t, Fixed speed, int32_t windUp, Vec2& from) {
    from = myPos + (vec(me.velX, me.velY) * windUp);
    const Vec2 tp = posOf(t);
    const Vec2 tv = velOf(t);
    Vec2 at = tp;
    for (int it = 0; it < 4; ++it) {
      const Fixed flight = max(distanceBetween(from, at) - offset, Fixed{}) / speed;
      at = tp + (tv * (flight + Fixed::fromInt(windUp)));
    }
    return at;
  };
  if (chargeHold_ > 0) {
    // v0.48: a charging throw stays held until its planned charge, still
    // leading the target; with the target gone it leaves at once.
    if (tgt == nullptr || thr.kind != static_cast<uint8_t>(ObsAbilityKind::Throw)) {
      chargeHold_ = 0;
    } else {
      --chargeHold_;
      in.buttons = static_cast<uint8_t>(in.buttons | thr.button);
      Vec2 from{};
      const Vec2 aimAt = leadAim(*tgt, chargeSpeed_, int32_t{lead} + chargeHold_, from);
      int32_t aim = directionTo(myPos, aimAt);
      if (config_.aimNoise != 0) aim += rng_.range(-int32_t{config_.aimNoise}, config_.aimNoise);
      in.aimDir = static_cast<uint8_t>(aim & 0xFF);
    }
  } else if (tgt == nullptr || !armed || !ready(thr, blocked) ||
             thr.kind != static_cast<uint8_t>(ObsAbilityKind::Throw)) {
    throwHold_ = -1;
  } else {
    const Fixed baseSpeed =
        grid && bananaSpeed_ > Fixed{} ? bananaSpeed_ : Fixed::fromRaw(r.bananaSpeed);
    const int32_t outbound = r.bananaOutboundTicks;
    // v0.48, a charged throw (the def's rate in defSpeed, the multipliers of
    // a tap and a full charge in defParam0/1): hold for the charge the
    // target's distance needs plus a margin, aiming for the release.
    const Fixed rate = Fixed::fromRaw(thr.defSpeed);
    const Fixed mulMin = Fixed::fromRaw(thr.defParam0);
    const Fixed mulMax = Fixed::fromRaw(thr.defParam1);
    const bool charges = !grid && rate > Fixed{} && mulMax > mulMin && outbound > 0;
    int32_t hold = 0;
    Fixed speed = charges ? baseSpeed * mulMin : baseSpeed;
    Vec2 from{};
    Vec2 aimAt = leadAim(*tgt, speed, int32_t{lead} + thr.defStartupTicks, from);
    if (charges) {
      const int32_t maxHold = ((mulMax - mulMin) / rate).ceilToInt();
      for (int pass = 0; pass < 3; ++pass) {
        const Fixed need =
            ((distanceBetween(from, aimAt) - offset + Fixed::half()) / (baseSpeed * outbound)) +
            kChargeMargin;
        hold = need <= mulMin ? 0 : std::min(((need - mulMin) / rate).ceilToInt(), maxHold);
        speed = baseSpeed * min(mulMin + (rate * hold), mulMax);
        aimAt = leadAim(*tgt, speed, int32_t{lead} + thr.defStartupTicks + hold, from);
      }
    }
    Fixed range =
        (baseSpeed * (charges ? mulMax : Fixed::one()) * outbound) + offset - Fixed::half();
    if (grid) range = min(range, cellSize_ * kGridThrowCells);
    const Fixed dist = distanceBetween(from, aimAt);
    bool clear = dist <= range;
    if (clear && obs.arena.wallCount > 0 && walledTicks_ < kStandoffTicks) {
      // Sample the line every half unit against the walls (v0.57: unless a
      // long standoff calls for a bank shot).
      const int32_t samples = std::max<int32_t>(1, (dist * 2).ceilToInt());
      const Fixed bananaRadius = Fixed::fromRaw(r.bananaRadius);
      for (int32_t s = 1; s <= samples && clear; ++s) {
        const Vec2 pt = from + ((aimAt - from) * Fixed::fromRatio(s, samples));
        for (uint32_t w = 0; w < obs.arena.wallCount && w < kObsMaxWalls; ++w) {
          if (botmath::circleOverlaps(obs.arena.walls[w], pt, bananaRadius)) {
            clear = false;
            break;
          }
        }
      }
    }
    // A standoff (the target holds a banana and could throw it on this very
    // tick): two bots that line up on the same tick, as mirrored monkeys do
    // at a round's start, would trade and both fall on one tick. A hold of
    // 0..kMaxThrowHold ticks, drawn once per lined-up shot from the bot's own
    // Rng, breaks the mirror without a fixed preference; outside a standoff
    // the shot goes at once.
    if (!clear) {
      throwHold_ = -1;
    } else if (throwHold_ < 0) {
      const bool standoff = tgt->ammo > 0 && enemyReady(*tgt, 1, ObsAbilityKind::Throw, lead);
      throwHold_ = standoff ? static_cast<int8_t>(rng_.below(kMaxThrowHold + 1)) : int8_t{0};
    }
    if (clear && throwHold_ > 0) {
      --throwHold_;
      in.aimDir = directionTo(myPos, aimAt);
    } else if (clear && rng_.below(256) < config_.throwPer256) {
      throwHold_ = -1;
      in.buttons = static_cast<uint8_t>(in.buttons | thr.button);
      // The press tick is the first held one: the banana leaves after the
      // startup plus `hold` ticks held past it.
      chargeHold_ = hold > 0 ? static_cast<uint16_t>(thr.defStartupTicks + hold - 1) : uint16_t{0};
      chargeSpeed_ = speed;
      int32_t aim = directionTo(myPos, aimAt);
      if (config_.aimNoise != 0) aim += rng_.range(-int32_t{config_.aimNoise}, config_.aimNoise);
      in.aimDir = static_cast<uint8_t>(aim & 0xFF);
    } else {
      in.aimDir = directionTo(myPos, aimAt);
    }
  }

  // --- call (v0.35): an enemy's coarse cell to the team --------------------
  if (grid && tgt != nullptr && ready(extra, blocked) && (in.buttons & thr.button) == 0 &&
      extra.kind == static_cast<uint8_t>(ObsAbilityKind::Call) &&
      (turn == 1 || obs.match.reservedTicksToLatch != 0) &&
      (!called_ || obs.match.tick - lastCallTick_ >= kCallEveryTurns * turn)) {
    const Vec2 at = posOf(*tgt);
    const Fixed hx = Fixed::fromRaw(obs.arena.halfExtents.x);
    const Fixed hy = Fixed::fromRaw(obs.arena.halfExtents.y);
    const int32_t col = std::clamp(((at.x + hx) / cellSize_).floorToInt(), 0, 31);
    const int32_t row = std::clamp(((hy - at.y) / cellSize_).floorToInt(), 0, 31);
    in.buttons = static_cast<uint8_t>(in.buttons | extra.button);
    in.aimDir = static_cast<uint8_t>((col >> 1) | ((row >> 1) << 4));
    lastCallTick_ = obs.match.tick;
    called_ = true;
  }

  // --- slam on a monkey in reach ---------------------------------------------
  if (tgt != nullptr && ready(extra, blocked) && (in.buttons & extra.button) == 0 &&
      extra.kind == static_cast<uint8_t>(ObsAbilityKind::Slam)) {
    const uint32_t impact =
        std::min<uint32_t>(uint32_t{lead} + extra.defStartupTicks + 1u, kHorizon);
    const Vec2 at = posOf(*tgt) + (velOf(*tgt) * static_cast<int32_t>(impact));
    walkPath(obs, in.moveDir, in.moveMag == 0, path_);
    const Fixed reach = Fixed::fromRaw(extra.defParam0) - Fixed::half();
    if (lengthSqRaw(at - path_.pos[impact]) < squareRaw(reach) &&
        rng_.below(256) < config_.throwPer256) {
      in.buttons = static_cast<uint8_t>(in.buttons | extra.button);
    }
  }
  return in;
}

}  // namespace kfm::ai
