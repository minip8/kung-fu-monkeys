// engine/ai treats the two sides of a half-turn map alike (the bot side of
// tests/scenario/side_symmetry_scenario.cpp, which covers the sim only).
// Every shipped contest arena is its own half turn (x, y) -> (-x, -y)
// (tests/unit/content/contest_test.cpp); with ten present slot 2k is team
// 0's k-th monkey and slot 2k + 1 team 1's, so the twin of slot i is slot
// i ^ 1. Each case builds a world that is its own half turn with the teams
// swapped: monkeys in twin pairs on mirrored cells with turned facings,
// bananas and pickups in pairs (pool slots 2j and 2j + 1) at mirrored
// points, some on cell edges and corners. Then:
//   - fog: what slot i sees (players, bananas, pickups) is the half turn of
//     what its twin sees, under own and team vision;
//   - the observation: slot i's, turned (positions negated, angles half a
//     turn on, cells counted from the other corner, slots exchanged within
//     pairs, teams 0 and 1 swapped), is byte for byte its twin's, the heard
//     calls included;
// Found and fixed with these: fog put a centre on a cell edge in the east
// and south cell only.
#include "../sim/sim_test_util.hpp"
#include "kfm/ai/observation.hpp"
#include "kfm/ai/observation_builder.hpp"
#include "kfm/ai/vision.hpp"
#include "kfm/content/loader.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/spaces/grid_cells.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

using namespace kfm;
using namespace kfm::ai;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr PlayerMask kTen = 0x3FF;
constexpr std::array<std::string_view, 4> kArenas = {"grove", "orchards", "gorge", "burrows"};

const ContentDB& shipped() {
  static const ContentDB db = [] {
    ContentDB c{};
    content::Diagnostics diag;
    const bool loaded = content::loadEmbeddedContent(c, diag);
    REQUIRE(loaded);
    return c;
  }();
  return db;
}

// The shipped content with the contest row's vision kind replaced.
[[nodiscard]] ContentDB contestWith(VisionKind kind) {
  ContentDB db = shipped();
  const std::optional<uint8_t> mode = db.findMode("contest");
  REQUIRE(mode.has_value());
  db.modes[mode.value_or(0)].vision.kind = kind;
  return db;
}

[[nodiscard]] constexpr Vec2 turned(Vec2 v) {
  return Vec2{-v.x, -v.y};
}

// A contest match on one arena, stepped into Playing, with every banana and
// pickup cleared and the world rebuilt by hand (place()).
struct MirrorWorld {
  MatchConfig config;
  MatchContent content;
  WorldState s;
  int32_t cols{0};
  int32_t rows{0};
  // Pool slot -> its twin's pool slot (bananas, pickups).
  std::array<uint16_t, kMaxBoomerangs> bananaTwin{};
  std::array<uint16_t, kMaxPickups> pickupTwin{};

  MirrorWorld(const ContentDB& db, std::string_view arena)
      : config(configFor(db, arena)), content(matchContent(db, config)), s(initialWorld(config)) {
    EventLog log;
    for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i) {
      runTicks(s, frameFor(s), 1, content, log);
    }
    REQUIRE(s.rules.phase == MatchPhase::Playing);
    cols = grid::columns(content.arena);
    rows = grid::rows(content.arena);
  }

  static MatchConfig configFor(const ContentDB& db, std::string_view arena) {
    MatchConfig c{};
    c.seed = 7;
    c.players = kTen;
    const std::optional<uint8_t> mode = db.findMode("contest");
    const std::optional<uint8_t> a = db.findArena(arena);
    REQUIRE(mode.has_value());
    REQUIRE(a.has_value());
    c.mode = mode.value_or(0);
    c.arena = a.value_or(0);
    c.teams = teamsFor(db.modeAt(c.mode), c.players);
    return c;
  }

  [[nodiscard]] Cell twinCell(Cell k) const { return Cell{cols - 1 - k.col, rows - 1 - k.row}; }

  // A floor cell (no wall, no hazard) at random.
  [[nodiscard]] Cell floorCell(Rng& rng) const {
    const ArenaDef& a = content.arena;
    while (true) {
      const Cell k{static_cast<int32_t>(rng.below(static_cast<uint32_t>(cols))),
                   static_cast<int32_t>(rng.below(static_cast<uint32_t>(rows)))};
      if (wallCell(a, k)) continue;
      const Vec2 c = grid::cellCentre(a, k);
      bool hazard = false;
      for (uint32_t i = 0; i < a.hazardCount; ++i) {
        const Hazard& h = a.hazards[i];
        if (c.x >= h.min.x && c.x <= h.max.x && c.y >= h.min.y && c.y <= h.max.y) hazard = true;
      }
      if (!hazard) return k;
    }
  }

  // A point near a floor cell: its centre, a point on one of its edges or
  // corners, or anywhere in it.
  [[nodiscard]] Vec2 pointNear(Rng& rng) const {
    const Vec2 c = grid::cellCentre(content.arena, floorCell(rng));
    const Fixed half = content.arena.cellSize / 2;
    switch (rng.below(4)) {
      case 0:
        return c;
      case 1:
        return Vec2{c.x + (rng.below(2) == 0 ? half : -half), c.y};  // a column edge
      case 2:
        return Vec2{c.x + half, c.y + (rng.below(2) == 0 ? half : -half)};  // a corner
      default:
        return Vec2{c.x + Fixed::fromRaw(rng.range(-half.raw + 1, half.raw - 1)),
                    c.y + Fixed::fromRaw(rng.range(-half.raw + 1, half.raw - 1))};
    }
  }

  // The world rebuilt from `seed`: five twin pairs of monkeys, `bananas`
  // twin pairs of bananas (flying ones only when `flying`) and `pickups`
  // pairs of banana pickups.
  void place(uint64_t seed, uint32_t bananas, uint32_t pickups, bool flying) {
    Rng rng = Rng::seeded(seed);
    s.boomerangs.forEachAlive(
        [&](EntityId id, const Boomerang& /*b*/) { s.boomerangs.despawn(id); });
    s.pickups.forEachAlive([&](EntityId id, const Pickup& /*k*/) { s.pickups.despawn(id); });
    for (uint8_t k = 0; k < kMaxPlayers / 2; ++k) {
      const PlayerId west{static_cast<uint8_t>(2 * k)};
      const PlayerId east{static_cast<uint8_t>((2 * k) + 1)};
      const Cell c = floorCell(rng);
      const auto aim = static_cast<uint8_t>(rng.below(256));
      const bool still = rng.below(2) == 0;
      const auto ammo = static_cast<uint8_t>(rng.below(4));
      const Vec2 at = grid::cellCentre(content.arena, c);
      Player& w = placePlayer(s, west, at);
      Player& e = placePlayer(s, east, turned(at));
      w.transform.facing = Angle::fromInput8(aim);
      e.transform.facing = Angle::fromInput8(static_cast<uint8_t>(aim + 128u));
      w.moveTicks = e.moveTicks = still ? 0 : 1;
      w.ammo = e.ammo = ammo;
    }
    for (uint32_t j = 0; j < bananas; ++j) {
      const auto owner = static_cast<uint8_t>(rng.below(kMaxPlayers));
      const Vec2 at = pointNear(rng);
      constexpr std::array<BoomerangPhase, 3> kPhases = {
          BoomerangPhase::Grounded, BoomerangPhase::Outbound, BoomerangPhase::Returning};
      const BoomerangPhase phase = kPhases[rng.below(flying ? 3 : 1)];
      const Angle heading = Angle::fromBam(static_cast<uint16_t>(rng.below(65536)));
      const Vec2 vel = phase == BoomerangPhase::Grounded
                           ? Vec2{}
                           : Vec2{Fixed::fromRaw(rng.range(-60000, 60000)),
                                  Fixed::fromRaw(rng.range(-60000, 60000))};
      const EntityId a = spawnBanana(s, PlayerId{owner}, at, vel, phase, content.db);
      const EntityId b = spawnBanana(s, PlayerId{static_cast<uint8_t>(owner ^ 1u)}, turned(at),
                                     turned(vel), phase, content.db);
      s.boomerangs.items[a.index].transform.facing = heading;
      s.boomerangs.items[b.index].transform.facing = heading + Angle::fromBam(Angle::kHalf);
      bananaTwin[a.index] = b.index;
      bananaTwin[b.index] = a.index;
    }
    for (uint32_t j = 0; j < pickups; ++j) {
      const Vec2 at = pointNear(rng);
      const EntityId a = spawnPickup(s, at);
      const EntityId b = spawnPickup(s, turned(at));
      pickupTwin[a.index] = b.index;
      pickupTwin[b.index] = a.index;
    }
  }

  [[nodiscard]] Observation observe(PlayerId self, const std::vector<HeardCall>& calls = {}) const {
    Observation o{};
    ObservationContext context;
    context.calls = calls.data();
    context.callCount = static_cast<uint8_t>(calls.size());
    buildObservation(s, content, 0, self, 0, context, o);
    return o;
  }
};

[[nodiscard]] constexpr uint8_t twinSlot(uint8_t i) {
  return static_cast<uint8_t>(i ^ 1u);
}
[[nodiscard]] constexpr uint8_t twinTeam(uint8_t t) {
  return t < 2 ? static_cast<uint8_t>(t ^ 1u) : t;
}
[[nodiscard]] uint16_t twinMask(uint16_t m) {
  uint16_t out = 0;
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    if ((m >> i & 1u) != 0) out = static_cast<uint16_t>(out | (1u << twinSlot(i)));
  }
  return out;
}

// What slot i's observation becomes for its twin in a world that is its own
// half turn (arena statics, rules and the mode's words do not move).
[[nodiscard]] Observation turnedObservation(const Observation& o, const MirrorWorld& w) {
  Observation t = o;
  ObsMatch& m = t.match;
  m.presentMask = twinMask(o.match.presentMask);
  m.aliveMask = twinMask(o.match.aliveMask);
  if (o.match.lastRoundWinner != kObsNoSlot) m.lastRoundWinner = twinSlot(o.match.lastRoundWinner);
  for (uint8_t i = 0; i < kObsMaxPlayers; ++i) {
    m.scores[twinSlot(i)] = o.match.scores[i];
    m.deaths[twinSlot(i)] = o.match.deaths[i];
    m.respawnTicks[twinSlot(i)] = o.match.respawnTicks[i];
    m.reservedTeamScores[twinTeam(i)] = o.match.reservedTeamScores[i];
  }
  ObsSelf& s = t.self;
  s.slot = twinSlot(o.self.slot);
  s.reservedTeam = twinTeam(o.self.reservedTeam);
  if (o.self.alive != 0) {
    s.posX = -o.self.posX;
    s.posY = -o.self.posY;
    s.prevPosX = -o.self.prevPosX;
    s.prevPosY = -o.self.prevPosY;
    s.velX = -o.self.velX;
    s.velY = -o.self.velY;
    s.knockbackX = -o.self.knockbackX;
    s.knockbackY = -o.self.knockbackY;
    s.facing = static_cast<uint8_t>(o.self.facing + 128u);
    s.reservedFacingOctant = static_cast<uint8_t>((o.self.reservedFacingOctant + 4u) & 7u);
    s.reservedCellX = static_cast<int16_t>(w.cols - 1 - o.self.reservedCellX);
    s.reservedCellY = static_cast<int16_t>(w.rows - 1 - o.self.reservedCellY);
    for (ObsAbility& a : s.abilities) {
      a.dirX = -a.dirX;
      a.dirY = -a.dirY;
    }
  }
  for (uint8_t i = 0; i < kObsMaxPlayers; ++i) {
    const ObsPlayer& p = o.players[i];
    ObsPlayer& q = t.players[twinSlot(i)];
    q = p;
    q.reservedTeam = twinTeam(p.reservedTeam);
    if (p.alive == 0) continue;
    q.posX = -p.posX;
    q.posY = -p.posY;
    q.velX = -p.velX;
    q.velY = -p.velY;
    q.facing = static_cast<uint8_t>(p.facing + 128u);
  }
  std::array<ObsBanana, kObsMaxBananas> bananas{};
  for (uint32_t i = 0; i < kObsMaxBananas; ++i) {
    const ObsBanana& b = o.bananas[i];
    if (b.valid == 0) continue;
    ObsBanana& c = bananas[w.bananaTwin[i]];
    c = b;
    c.owner = twinSlot(b.owner);
    c.posX = -b.posX;
    c.posY = -b.posY;
    c.velX = -b.velX;
    c.velY = -b.velY;
    c.headingBam = static_cast<uint16_t>(b.headingBam + 32768u);
  }
  std::memcpy(static_cast<void*>(t.bananas), bananas.data(), sizeof t.bananas);
  std::array<ObsPickup, kObsMaxPickups> pickups{};
  for (uint32_t i = 0; i < kObsMaxPickups; ++i) {
    const ObsPickup& k = o.pickups[i];
    if (k.valid == 0) continue;
    ObsPickup& c = pickups[w.pickupTwin[i]];
    c = k;
    c.posX = -k.posX;
    c.posY = -k.posY;
  }
  std::memcpy(static_cast<void*>(t.pickups), pickups.data(), sizeof t.pickups);
  for (ObsHeardCall& c : t.reservedHeardCalls) {
    if (c.valid == 0) continue;
    c.slot = twinSlot(c.slot);
    c.symbol = static_cast<uint8_t>(c.symbol + 128u);
    if (c.cellX != kObsNoCell) c.cellX = static_cast<int16_t>(w.cols - 1 - c.cellX);
    if (c.cellY != kObsNoCell) c.cellY = static_cast<int16_t>(w.rows - 1 - c.cellY);
  }
  return t;
}

// Entry by entry first, so a failure names it, then every byte.
void checkSame(const Observation& want, const Observation& got) {
  for (uint8_t i = 0; i < kObsMaxPlayers; ++i) {
    INFO("player " << int{i});
    CHECK(std::memcmp(&want.players[i], &got.players[i], sizeof(ObsPlayer)) == 0);
  }
  for (uint32_t i = 0; i < kObsMaxBananas; ++i) {
    INFO("banana " << i);
    CHECK(std::memcmp(&want.bananas[i], &got.bananas[i], sizeof(ObsBanana)) == 0);
  }
  for (uint32_t i = 0; i < kObsMaxPickups; ++i) {
    INFO("pickup " << i);
    CHECK(std::memcmp(&want.pickups[i], &got.pickups[i], sizeof(ObsPickup)) == 0);
  }
  CHECK(std::memcmp(&want.self, &got.self, sizeof(ObsSelf)) == 0);
  CHECK(std::memcmp(&want.match, &got.match, sizeof(ObsMatch)) == 0);
  CHECK(std::memcmp(&want.reservedHeardCalls, &got.reservedHeardCalls,
                    sizeof want.reservedHeardCalls) == 0);
  CHECK(std::memcmp(&want, &got, sizeof(Observation)) == 0);
}

}  // namespace

TEST_CASE("fog sees the two sides of a half-turn arena alike", "[ai][vision][symmetry]") {
  for (const VisionKind kind : {VisionKind::Own, VisionKind::Team}) {
    const ContentDB db = contestWith(kind);
    for (const std::string_view arena : kArenas) {
      MirrorWorld w(db, arena);
      for (uint64_t trial = 0; trial < 40; ++trial) {
        w.place(1000 + trial, 8, 8, /*flying=*/true);
        for (uint8_t i = 0; i < kMaxPlayers; ++i) {
          INFO("vision " << int{static_cast<uint8_t>(kind)} << " arena " << arena << " trial "
                         << trial << " slot " << int{i});
          const Sight a = visionOf(w.s, w.content, PlayerId{i});
          const Sight b = visionOf(w.s, w.content, PlayerId{twinSlot(i)});
          CHECK(twinMask(a.players) == b.players);
          for (uint32_t k = 0; k < kMaxBoomerangs; ++k) {
            if (!w.s.boomerangs.isAliveIndex(static_cast<uint16_t>(k))) continue;
            CHECK(((a.bananas >> k) & 1u) == ((b.bananas >> w.bananaTwin[k]) & 1u));
          }
          for (uint32_t k = 0; k < kMaxPickups; ++k) {
            if (!w.s.pickups.isAliveIndex(static_cast<uint16_t>(k))) continue;
            CHECK(((a.pickups >> k) & 1u) == ((b.pickups >> w.pickupTwin[k]) & 1u));
          }
        }
      }
    }
  }
}

TEST_CASE("a centre on a cell edge is seen from both sides of it", "[ai][vision][symmetry]") {
  // One monkey west of a pickup lying on the edge between two columns, one
  // east of its twin: each sees its pickup at the same Chebyshev distance.
  const ContentDB db = contestWith(VisionKind::Own);
  MirrorWorld w(db, "grove");
  w.place(1, 0, 0, false);
  const Fixed half = w.content.arena.cellSize / 2;
  // Slot 0 on the cell left of the centre's row-6 cells, facing south (its
  // cone away), so only peripheral sight (1 cell) can see.
  const Vec2 at = grid::cellCentre(w.content.arena, Cell{9, 6});
  placePlayer(w.s, PlayerId{0}, at).transform.facing = Angle::fromInput8(192);
  placePlayer(w.s, PlayerId{1}, turned(at)).transform.facing = Angle::fromInput8(64);
  // The pickup on the east edge of the next cell: a column and a half off.
  const Vec2 edge{at.x + w.content.arena.cellSize + half, at.y};
  const EntityId a = spawnPickup(w.s, edge);
  const EntityId b = spawnPickup(w.s, turned(edge));
  const Sight west = visionOf(w.s, w.content, PlayerId{0});
  const Sight east = visionOf(w.s, w.content, PlayerId{1});
  // The edge point is in cells 10 and 11; cell 10 is within the peripheral 1.
  CHECK(((west.pickups >> a.index) & 1u) == 1u);
  CHECK(((east.pickups >> b.index) & 1u) == 1u);
}

TEST_CASE("the observation of a slot turns into its twin's", "[ai][observation][symmetry]") {
  for (const VisionKind kind : {VisionKind::Own, VisionKind::Team}) {
    const ContentDB db = contestWith(kind);
    for (const std::string_view arena : kArenas) {
      MirrorWorld w(db, arena);
      for (uint64_t trial = 0; trial < 12; ++trial) {
        w.place(2000 + trial, 6, 6, /*flying=*/true);
        for (uint8_t i = 0; i < kMaxPlayers; ++i) {
          INFO("vision " << int{static_cast<uint8_t>(kind)} << " arena " << arena << " trial "
                         << trial << " slot " << int{i});
          // A call from each of two other slots, the caller's cell its own.
          std::vector<HeardCall> calls;
          std::vector<HeardCall> twinCalls;
          for (const uint8_t from :
               {static_cast<uint8_t>((i + 2) % 10), static_cast<uint8_t>((i + 5) % 10)}) {
            const Player& p = playerAt(w.s, PlayerId{from});
            const Cell k = grid::cellOf(w.content.arena, p.transform.pos);
            const auto symbol = static_cast<uint8_t>(17u * from);
            calls.push_back(HeardCall{from, symbol, 5, k, true});
            twinCalls.push_back(HeardCall{twinSlot(from), static_cast<uint8_t>(symbol + 128u), 5,
                                          w.twinCell(k), true});
          }
          const Observation mine = w.observe(PlayerId{i}, calls);
          const Observation twin = w.observe(PlayerId{twinSlot(i)}, twinCalls);
          checkSame(turnedObservation(mine, w), twin);
        }
      }
    }
  }
}
