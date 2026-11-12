// Fog and calls (architecture §8 "Teams, fog and calls", v0.35;
// docs/bot-abi.md §3): the cone's cell count and shape, Bresenham line of
// sight against a small wall map, the fog projection (own, team, all) in
// buildObservation, the turn window AgentHost ORs visibility over, the calls
// it folds in and the per-instance lifecycle.
#include "kfm/ai/vision.hpp"

#include "../sim/sim_test_util.hpp"
#include "kfm/ai/agent.hpp"
#include "kfm/ai/agent_host.hpp"
#include "kfm/ai/observation.hpp"
#include "kfm/ai/observation_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

using namespace kfm;
using namespace kfm::ai;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};
constexpr PlayerId kP2{2};
constexpr PlayerId kP3{3};

// A 10 x 10 cell arena (cell 1.0, the box [-5, 5]²) with one wall: column 4,
// rows 3..6 (x in [-1, 0], y in [-2, 2]).
[[nodiscard]] ContentDB fogContent(VisionKind kind) {
  ContentDB c = defaultContent();
  ArenaDef& a = c.arenas[0];
  a.halfExtents = Vec2{Fixed::fromInt(5), Fixed::fromInt(5)};
  a.cellSize = Fixed::one();
  a.wallCount = 1;
  a.walls[0] =
      Wall{Vec2{Fixed::fromInt(-1), Fixed::fromInt(-2)}, Vec2{Fixed::zero(), Fixed::fromInt(2)}};
  ModeDef& m = c.modes[0];
  m.vision = VisionDef{kind, 1, 3, 1, 3, 1};
  return c;
}

[[nodiscard]] Vec2 centre(int32_t col, int32_t row) {
  return Vec2{Fixed::fromRatio((2 * col) - 9, 2), Fixed::fromRatio(9 - (2 * row), 2)};
}

// Places a monkey on a cell centre facing an aim byte (at rest, still).
void put(WorldState& s, PlayerId id, int32_t col, int32_t row, uint8_t aim = 0) {
  Player& p = placePlayer(s, id, centre(col, row));
  p.transform.facing = Angle::fromInput8(aim);
  p.moveTicks = 0;
}

[[nodiscard]] Observation observe(const WorldState& s, const MatchContent& content, PlayerId self) {
  Observation o{};
  buildObservation(s, content, 0, self, 0, o);
  return o;
}

// Keeps every observation it is given.
class Recorder final : public IAgent {
 public:
  explicit Recorder(std::vector<Observation>& log, uint32_t& inits) : log_(&log), inits_(&inits) {}
  void init(uint64_t /*seed*/, PlayerId /*slot*/) override { ++*inits_; }
  std::optional<PlayerInput> act(const Observation& obs, Tick /*tick*/) override {
    log_->push_back(obs);
    return PlayerInput{};
  }

 private:
  std::vector<Observation>* log_;
  uint32_t* inits_;
};

}  // namespace

TEST_CASE("the cone holds R² + 2R cells for every facing", "[ai][vision]") {
  for (int32_t r = 1; r <= 8; ++r) {
    for (uint8_t o = 0; o < 8; ++o) {
      INFO("R " << r << " octant " << int{o});
      int32_t count = 0;
      for (int32_t dr = -r - 2; dr <= r + 2; ++dr) {
        for (int32_t dc = -r - 2; dc <= r + 2; ++dc) {
          const bool in = inCone(dc, dr, o, r);
          if (in) ++count;
          // Never the own cell, never beyond Chebyshev R.
          if (in) CHECK(chebyshev(dc, dr) <= r);
          if (dc == 0 && dr == 0) CHECK_FALSE(in);
          // The shapes: a cardinal cone is the band |side| <= forward, a
          // diagonal one the closed quadrant.
          const int32_t oc = kStepCol[o];
          const int32_t orow = kStepRow[o];
          bool expected = false;
          if (chebyshev(dc, dr) <= r && (dc != 0 || dr != 0)) {
            if (grid::diagonal(o)) {
              expected = dc * oc >= 0 && dr * orow >= 0;
            } else {
              const int32_t forward = (dc * oc) + (dr * orow);
              const int32_t side = oc != 0 ? dr : dc;
              expected = forward >= (side < 0 ? -side : side);
            }
          }
          CHECK(in == expected);
        }
      }
      CHECK(count == (r * r) + (2 * r));
    }
  }
  CHECK_FALSE(inCone(1, 0, 0, 0));  // R 0: no cone
  // East: the band; north-east: the quadrant (rows grow southward).
  CHECK(inCone(2, 2, 0, 3));
  CHECK_FALSE(inCone(2, 3, 0, 3));
  CHECK(inCone(3, 0, 1, 3));
  CHECK(inCone(0, -3, 1, 3));
  CHECK_FALSE(inCone(1, 1, 1, 3));
  CHECK(facingOctant(Angle::fromInput8(0)) == 0);
  CHECK(facingOctant(Angle::fromInput8(15)) == 0);
  CHECK(facingOctant(Angle::fromInput8(16)) == 1);  // half way: counter-clockwise
  CHECK(facingOctant(Angle::fromInput8(64)) == 2);
  CHECK(facingOctant(Angle::fromInput8(240)) == 0);
  CHECK(facingOctant(Angle::fromInput8(239)) == 7);
}

TEST_CASE("Bresenham lines and the line of sight through walls", "[ai][vision]") {
  std::vector<Cell> cells;
  const bool done = bresenham(Cell{0, 0}, Cell{4, 2}, [&](Cell k, bool /*diag*/) {
    cells.push_back(k);
    return true;
  });
  CHECK(done);
  REQUIRE(cells.size() == 5);  // max(|dc|, |dr|) + 1
  CHECK(cells.front() == Cell{0, 0});
  CHECK(cells.back() == Cell{4, 2});
  for (size_t i = 1; i < cells.size(); ++i)
    CHECK(chebyshev(cells[i - 1], cells[i]) == 1);
  // Stopping early reports it.
  CHECK_FALSE(bresenham(Cell{0, 0}, Cell{-3, 3}, [](Cell k, bool /*diag*/) { return k.col > -2; }));

  const ContentDB c = fogContent(VisionKind::Own);
  const ArenaDef& a = c.arenas[0];
  CHECK(wallCell(a, Cell{4, 3}));
  CHECK(wallCell(a, Cell{4, 6}));
  CHECK_FALSE(wallCell(a, Cell{4, 2}));
  CHECK_FALSE(wallCell(a, Cell{5, 4}));
  CHECK_FALSE(lineOfSight(a, Cell{1, 5}, Cell{7, 5}));   // straight through the wall
  CHECK(lineOfSight(a, Cell{1, 5}, Cell{3, 5}));         // up to it
  CHECK_FALSE(lineOfSight(a, Cell{1, 5}, Cell{4, 5}));   // a wall cell is never seen
  CHECK(lineOfSight(a, Cell{1, 1}, Cell{8, 1}));         // above it
  CHECK(lineOfSight(a, Cell{3, 2}, Cell{5, 2}));         // past its top end
  CHECK(lineOfSight(a, Cell{2, 2}, Cell{2, 2}));         // the own cell
  CHECK_FALSE(lineOfSight(a, Cell{1, 5}, Cell{10, 5}));  // outside the columns
  // A diagonal squeeze between two walls touching at a corner is blocked.
  ContentDB corner = c;
  ArenaDef& b = corner.arenas[0];
  b.wallCount = 2;
  b.walls[0] = Wall{centre(1, 0) - Vec2{Fixed::fromRatio(1, 2), Fixed::fromRatio(1, 2)},
                    centre(1, 0) + Vec2{Fixed::fromRatio(1, 2), Fixed::fromRatio(1, 2)}};
  b.walls[1] = Wall{centre(0, 1) - Vec2{Fixed::fromRatio(1, 2), Fixed::fromRatio(1, 2)},
                    centre(0, 1) + Vec2{Fixed::fromRatio(1, 2), Fixed::fromRatio(1, 2)}};
  CHECK(wallCell(b, Cell{1, 0}));
  CHECK(wallCell(b, Cell{0, 1}));
  CHECK_FALSE(lineOfSight(b, Cell{0, 0}, Cell{1, 1}));
  CHECK(lineOfSight(b, Cell{2, 2}, Cell{1, 1}));
}

TEST_CASE("own vision: a wall hides, the cone shows, hearing covers the back", "[ai][vision]") {
  const ContentDB c = fogContent(VisionKind::Own);
  WorldState s = makePlayingWorld(5, 0b1111, c);
  put(s, kP0, 1, 5, 0);  // facing east, the wall 3 cells ahead
  put(s, kP1, 7, 5);     // behind the wall
  put(s, kP2, 3, 3);     // in the cone (2 ahead, 2 up), clear line
  put(s, kP3, 0, 9);     // behind P0, out of every range
  const EntityId behind = spawnBanana(s, kP1, centre(0, 4), Vec2{Fixed::one(), Fixed::zero()});
  const EntityId far = spawnBanana(s, kP1, centre(0, 0), Vec2{Fixed::one(), Fixed::zero()},
                                   BoomerangPhase::Returning);
  const EntityId lying = spawnBanana(s, kP1, centre(0, 8), Vec2{}, BoomerangPhase::Grounded);
  const EntityId pickupSeen = spawnPickup(s, centre(2, 5));
  const EntityId pickupHidden = spawnPickup(s, centre(8, 5));
  const MatchContent mc(c);
  const Observation o = observe(s, mc, kP0);

  CHECK((o.header.schemaFlags & kObsFlagFog) != 0);
  CHECK(o.players[0].reservedVisible == 1);  // self
  CHECK(o.players[0].posX == centre(1, 5).x.raw);
  // P1 behind the wall: roster only.
  const ObsPlayer& hidden = o.players[1];
  CHECK(hidden.reservedVisible == 0);
  CHECK(hidden.present == 1);
  CHECK(hidden.relation == static_cast<uint8_t>(ObsRelation::Enemy));
  CHECK(hidden.reservedTeam == 1);
  CHECK(hidden.alive == 0);
  CHECK(hidden.posX == 0);
  CHECK(hidden.hp == 0);
  // P2 in the cone, P3 behind and far.
  CHECK(o.players[2].reservedVisible == 1);
  CHECK(o.players[2].alive == 1);
  CHECK(o.players[2].posX == centre(3, 3).x.raw);
  CHECK(o.players[3].reservedVisible == 0);
  CHECK(o.players[3].alive == 0);
  // Hearing: the banana in flight 1 cell behind is heard; the grounded one
  // (no flight) and the far one are not.
  CHECK(o.bananas[behind.index].valid == 1);
  CHECK(o.bananas[behind.index].reservedVisible == 1);
  CHECK(o.bananas[far.index].valid == 0);
  CHECK(o.bananas[lying.index].valid == 0);
  // Pickups follow the cells: one in the peripheral ring, one behind the wall.
  CHECK(o.pickups[pickupSeen.index].valid == 1);
  CHECK(o.pickups[pickupHidden.index].valid == 0);
  // Counts stay at capacity: the match block still says who is alive.
  CHECK(o.match.aliveCount == 4);

  // Turned around (west), P2 leaves the cone.
  playerAt(s, kP0).transform.facing = Angle::fromInput8(128);
  CHECK(observe(s, mc, kP0).players[2].reservedVisible == 0);
  // Banana sight: an own banana in flight next to P1 shows it.
  playerAt(s, kP0).transform.facing = Angle::fromInput8(0);
  spawnBanana(s, kP0, centre(7, 4), Vec2{Fixed::one(), Fixed::zero()});
  CHECK(observe(s, mc, kP0).players[1].reservedVisible == 1);
}

TEST_CASE("focus lengthens the cone of a monkey at rest", "[ai][vision]") {
  ContentDB c = fogContent(VisionKind::Own);
  c.arenas[0].wallCount = 0;
  WorldState s = makePlayingWorld(6, 0b11, c);
  const MatchContent mc(c);
  put(s, kP0, 1, 5, 0);
  put(s, kP1, 5, 5);  // 4 ahead: cone 3 + focus 1
  CHECK(focused(playerAt(s, kP0)));
  CHECK(observe(s, mc, kP0).players[1].reservedVisible == 1);
  // It moved on the last tick: no focus.
  playerAt(s, kP0).transform.prevPos = centre(0, 5);
  CHECK_FALSE(focused(playerAt(s, kP0)));
  CHECK(observe(s, mc, kP0).players[1].reservedVisible == 0);
  // Mid-slide: no focus either.
  playerAt(s, kP0).transform.prevPos = centre(1, 5);
  playerAt(s, kP0).moveTicks = 3;
  CHECK(observe(s, mc, kP0).players[1].reservedVisible == 0);
}

TEST_CASE("team vision is the union of the allies' sight", "[ai][vision]") {
  ContentDB c = fogContent(VisionKind::Team);
  c.modes[0].teams = 2;
  WorldState s = makePlayingWorld(7, 0b1111, c);
  MatchConfig config{7, 0b1111};
  config.teams = teamsFor(c.modes[0], 0b1111);  // slots 0, 2 vs 1, 3
  const MatchContent mc = matchContent(c, config);
  put(s, kP0, 1, 5, 0);
  put(s, kP2, 8, 8, 64);  // P0's ally, on the far side of the wall, facing north
  put(s, kP1, 8, 6);      // behind the wall for P0, in P2's cone
  put(s, kP3, 1, 0);      // nobody sees it
  const Observation team = observe(s, mc, kP0);
  CHECK(team.players[1].reservedVisible == 1);
  CHECK(team.players[2].reservedVisible == 1);  // an ally is always seen under team
  CHECK(team.players[2].relation == static_cast<uint8_t>(ObsRelation::Ally));
  CHECK(team.players[3].reservedVisible == 0);
  // The same world under own vision: only P0's sight.
  ContentDB own = c;
  own.modes[0].vision.kind = VisionKind::Own;
  const MatchContent mcOwn = matchContent(own, config);
  const Observation alone = observe(s, mcOwn, kP0);
  CHECK(alone.players[1].reservedVisible == 0);
  CHECK(alone.players[2].reservedVisible == 0);
  // A dead observer still sees through its allies.
  s.players.despawn(s.players.idAt(0));
  const Observation dead = observe(s, mc, kP0);
  CHECK(dead.self.alive == 0);
  CHECK(dead.players[1].reservedVisible == 1);
}

TEST_CASE("vision all is the pre-fog observation byte for byte", "[ai][vision]") {
  ContentDB c = fogContent(VisionKind::All);
  c.arenas[0].zones[0] = Zone{centre(2, 2), centre(3, 3)};  // v0.80: the zones block too
  c.arenas[0].zoneCount = 1;
  WorldState s = makePlayingWorld(8, 0b111, c);
  put(s, kP0, 1, 5, 0);
  put(s, kP1, 7, 5);
  spawnBanana(s, kP1, centre(9, 9), Vec2{Fixed::one(), Fixed::zero()});
  const MatchContent mc(c);
  Observation plain{};
  buildObservation(s, mc, 0, kP0, 0, plain);
  // Any visibility a host hands in is ignored without fog.
  const ObsVisibility none;
  Observation withContext{};
  buildObservation(s, mc, 0, kP0, 0, ObservationContext{&none, nullptr, 0}, withContext);
  CHECK(std::memcmp(&plain, &withContext, sizeof(Observation)) == 0);
  CHECK(plain.header.schemaFlags == 0);
  CHECK(plain.zones.count == 1);
  for (const ObsPlayer& p : plain.players)
    CHECK(p.reservedVisible == 0);
  for (const ObsBanana& b : plain.bananas)
    CHECK(b.reservedVisible == 0);
  CHECK(plain.players[1].alive == 1);
  // A continuous mode leaves the grid fields 0.
  CHECK(plain.match.reservedTurnTicks == 0);
  CHECK(plain.self.reservedCellX == 0);
  CHECK(plain.self.reservedFacingOctant == 0);
}

TEST_CASE("fog never hides a zone (v0.80)", "[ai][vision][zones]") {
  // Three zones, one behind the wall from P0 and one at its back: under own
  // and team vision the zones block and words 8-15 are those of vision all.
  ContentDB c = fogContent(VisionKind::Own);
  c.modes[0].teams = 2;
  ArenaDef& a = c.arenas[0];
  a.zones[0] = Zone{centre(7, 4) - Vec2{Fixed::one(), Fixed::one()}, centre(7, 4)};
  a.zones[1] = Zone{centre(0, 8), centre(1, 9)};
  a.zones[2] = Zone{centre(4, 0), centre(6, 1)};
  a.zoneCount = 3;
  WorldState s = makePlayingWorld(9, 0b1111, c);
  put(s, kP0, 1, 5, 0);
  put(s, kP1, 7, 5);
  put(s, kP2, 8, 8, 64);
  put(s, kP3, 1, 0);
  MatchConfig config{9, 0b1111};
  config.teams = teamsFor(c.modes[0], 0b1111);
  ContentDB all = c;
  all.modes[0].vision.kind = VisionKind::All;
  const Observation seen = observe(s, matchContent(all, config), kP0);
  REQUIRE(seen.zones.count == 3);
  for (const VisionKind kind : {VisionKind::Own, VisionKind::Team}) {
    ContentDB fog = c;
    fog.modes[0].vision.kind = kind;
    const Observation o = observe(s, matchContent(fog, config), kP0);
    INFO("vision " << static_cast<int>(kind));
    CHECK((o.header.schemaFlags & kObsFlagFog) != 0);
    CHECK(o.players[3].reservedVisible == 0);  // the fog is on: nobody sees P3
    CHECK(o.players[3].posX == 0);
    CHECK(std::memcmp(&o.zones, &seen.zones, sizeof(ObsZones)) == 0);
    for (uint32_t w = kObsZoneWord; w < kObsModeDataWords; ++w)
      CHECK(o.match.reservedModeData[w] == seen.match.reservedModeData[w]);
  }
}

TEST_CASE("a grid mode fills the cell, the octant and the turn clock", "[ai][vision]") {
  ContentDB c = fogContent(VisionKind::Own);
  c.modes[0].space = Space::Grid;
  c.modes[0].turnTicks = 5;
  WorldState s = makePlayingWorld(9, 0b11, c);
  const MatchContent mc(c);
  put(s, kP0, 6, 2, 96);  // north-west
  s.rules.ticksInPhase = 7;
  const Observation o = observe(s, mc, kP0);
  CHECK(o.self.reservedCellX == 6);
  CHECK(o.self.reservedCellY == 2);
  CHECK(o.self.reservedFacingOctant == 3);
  CHECK(o.match.reservedTurnTicks == 5);
  CHECK(o.match.reservedTicksToLatch == 3);
  s.rules.ticksInPhase = 10;
  CHECK(observe(s, mc, kP0).match.reservedTicksToLatch == 0);
  CHECK(turnWindowEnds(s, mc));
  s.rules.ticksInPhase = 11;
  CHECK_FALSE(turnWindowEnds(s, mc));
  s.rules.phase = MatchPhase::RoundStarting;
  s.rules.ticksInPhase = 20;
  CHECK(turnClock(s, mc).ticksToLatch == c.modes[0].roundStartTicks - 20);
  CHECK(turnWindowEnds(s, mc));
  s.rules.phase = MatchPhase::RoundEnded;
  s.rules.ticksInPhase = 100;
  CHECK(turnClock(s, mc).ticksToLatch ==
        c.modes[0].roundEndTicks - 100 + c.modes[0].roundStartTicks);
}

TEST_CASE("AgentHost ORs visibility over a turn and folds the calls in", "[ai][vision][host]") {
  ContentDB c = fogContent(VisionKind::Own);
  c.modes[0].space = Space::Grid;
  c.modes[0].turnTicks = 3;
  c.modes[0].teams = 2;
  WorldState s = makePlayingWorld(10, 0b111, c);
  MatchConfig config{10, 0b111};
  config.teams = teamsFor(c.modes[0], 0b111);  // slots 0, 2 vs 1
  const MatchContent mc = matchContent(c, config);
  std::vector<Observation> log;
  uint32_t inits = 0;
  AgentHost host;
  host.attach(kP0, std::make_unique<Recorder>(log, inits), 1);
  put(s, kP0, 1, 5, 0);
  put(s, kP1, 3, 5);  // in sight
  put(s, kP2, 2, 6);  // P0's ally, within a call's range

  const auto fillAt = [&](uint32_t ticksInPhase) {
    s.rules.ticksInPhase = ticksInPhase;
    InputFrame f = frameFor(s);
    host.fill(f, s, mc);
    s.tick = s.tick.plus(1);
    host.observe(EventSink{}, s, mc);  // a step without calls
  };
  fillAt(1);  // window ticks 1, 2, 3 (the latch)
  REQUIRE(log.size() == 1);
  CHECK(log.back().players[1].reservedVisible == 1);
  put(s, kP1, 8, 5);  // gone behind the wall
  fillAt(2);
  CHECK(log.back().players[1].reservedVisible == 1);  // seen earlier this turn
  CHECK(log.back().players[1].posX == centre(8, 5).x.raw);
  // A call from the ally (range 4) and one from the enemy behind the wall
  // (in range: 7 columns away? no: out of range) reach P0 for the latch.
  EventSink sink;
  sink.push(SimEvent{EventKind::MonkeyCalled, s.tick, kP2, s.players.idAt(2), 200,
                     static_cast<uint32_t>(Fixed::fromInt(4).raw)});
  sink.push(SimEvent{EventKind::MonkeyCalled, s.tick, kP1, s.players.idAt(1), 9,
                     static_cast<uint32_t>(Fixed::fromInt(4).raw)});
  sink.push(SimEvent{EventKind::MonkeyCalled, s.tick, kP1, s.players.idAt(1), 10,
                     static_cast<uint32_t>(Fixed::fromInt(8).raw)});
  host.observe(sink, s, mc);
  fillAt(3);  // the latch
  const Observation& latch = log.back();
  CHECK(latch.players[1].reservedVisible == 1);
  CHECK(latch.match.reservedTicksToLatch == 0);
  REQUIRE(latch.reservedHeardCalls[0].valid == 1);
  CHECK(latch.reservedHeardCalls[0].slot == 2);
  CHECK(latch.reservedHeardCalls[0].symbol == 200);
  CHECK(latch.reservedHeardCalls[0].cellX == 2);
  CHECK(latch.reservedHeardCalls[0].cellY == 6);
  // The far enemy's second call (range 8) is heard, with its cell because
  // it was seen this turn.
  REQUIRE(latch.reservedHeardCalls[1].valid == 1);
  CHECK(latch.reservedHeardCalls[1].slot == 1);
  CHECK(latch.reservedHeardCalls[1].symbol == 10);
  CHECK(latch.reservedHeardCalls[1].cellX == 8);
  CHECK(latch.reservedHeardCalls[2].valid == 0);
  // The next turn starts empty: P1 is unseen, the calls are gone.
  fillAt(4);
  CHECK(log.back().players[1].reservedVisible == 0);
  CHECK(log.back().reservedHeardCalls[0].valid == 0);
  // An unseen caller's call carries no cell.
  sink.clear();
  sink.push(SimEvent{EventKind::MonkeyCalled, s.tick, kP1, s.players.idAt(1), 11,
                     static_cast<uint32_t>(Fixed::fromInt(8).raw)});
  host.observe(sink, s, mc);
  fillAt(5);
  REQUIRE(log.back().reservedHeardCalls[0].valid == 1);
  CHECK(log.back().reservedHeardCalls[0].cellX == kObsNoCell);
  CHECK(log.back().reservedHeardCalls[0].cellY == kObsNoCell);
}

TEST_CASE("AgentHost pauses an eliminated slot and wipes it on respawn if asked",
          "[ai][host][lifecycle]") {
  for (const RespawnMemory memory : {RespawnMemory::Keep, RespawnMemory::Wipe}) {
    INFO("wipe " << (memory == RespawnMemory::Wipe));
    WorldState s = makePlayingWorld(11, 0b11);
    std::vector<Observation> log;
    uint32_t inits = 0;
    AgentHost host;
    host.setRespawnMemory(memory);
    host.attach(kP1, std::make_unique<Recorder>(log, inits), 3);
    CHECK(inits == 1);
    const auto fillOnce = [&] {
      InputFrame f = frameFor(s);
      f.inputs[1].moveMag = 77;  // a stale value the host must overwrite
      host.fill(f, s, defaultContent());
      s.tick = s.tick.plus(1);
      host.observe(EventSink{}, s, defaultContent());  // a step without calls
      return f.inputs[1];
    };
    fillOnce();
    CHECK(log.size() == 1);
    CHECK_FALSE(host.paused(kP1));
    // Eliminated: no act, an idle input, the paused counter.
    const Player kept = playerAt(s, kP1);
    s.players.despawn(s.players.idAt(1));
    for (int i = 0; i < 4; ++i)
      CHECK(fillOnce() == PlayerInput{});
    CHECK(log.size() == 1);
    CHECK(host.paused(kP1));
    CHECK(host.stats(kP1).paused == 4);
    CHECK(host.stats(kP1).acts == 1);
    // Respawned: it acts again, re-inited only under Wipe.
    const EntityId back = s.players.spawnAt(1);
    s.players.items[back.index] = kept;
    fillOnce();
    CHECK(log.size() == 2);
    CHECK_FALSE(host.paused(kP1));
    CHECK(inits == (memory == RespawnMemory::Wipe ? 2u : 1u));
    CHECK(host.wipes(kP1) == (memory == RespawnMemory::Wipe ? 1u : 0u));
  }
  // A slot that never had a monkey acts as before (the lobby, a late joiner).
  WorldState fresh = initialWorld(MatchConfig{12, 0b11});
  std::vector<Observation> log;
  uint32_t inits = 0;
  AgentHost host;
  host.attach(kP0, std::make_unique<Recorder>(log, inits), 3);
  InputFrame f = frameFor(fresh);
  f.present = 0b11;
  fresh.rules.phase = MatchPhase::Playing;
  host.fill(f, fresh, defaultContent());
  CHECK(log.size() == 1);
  CHECK_FALSE(host.paused(kP0));
}
