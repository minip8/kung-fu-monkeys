// Contended pickups (architecture §5.6, systems/interaction.cpp): a pickup
// goes to one monkey, so when two or more monkeys that could take it touch
// it on one tick, the world Rng draws the order they take it in (a
// Fisher-Yates shuffle, one Rng::below per contender after the first). A
// monkey that could not take it (a full hand, its own team's flag) is no
// contender; one contender draws nothing, so a tick without such a tie is
// resolved as before. Before the draw the lower slot took every tie (pool
// order), which on harvest's grove gave the west team every first fruit of
// a shared tree. Hand-built content: the default dojo given 1-unit cells
// and no spawners but what a test adds.
#include "kfm/sim/rules/handlers.hpp"
#include "kfm/sim/rules/interaction.hpp"
#include "loadout_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <vector>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};
constexpr PlayerId kP2{2};

// Seeded trials per fairness test: each side wins about half (binomial,
// a standard deviation of 10 at 400; the bounds are 5 of them).
constexpr uint64_t kTrials = 400;
constexpr int kLow = 150;
constexpr int kHigh = 250;

[[nodiscard]] constexpr Vec2 half(int32_t x2, int32_t y2) {  // (x2 / 2, y2 / 2)
  return Vec2{Fixed::fromRatio(x2, 2), Fixed::fromRatio(y2, 2)};
}

// The tree's cell centre, (0.5, 0.5), and two places beside it that touch
// whatever lies there (the player and pickup radii are 0.5 each).
constexpr Vec2 kSpot = half(1, 1);
constexpr Vec2 kWest = half(0, 1);
constexpr Vec2 kEast = half(2, 1);
constexpr Vec2 kFar = Vec2{Fixed::fromInt(-8), Fixed::fromInt(-4)};

constexpr uint16_t kFirst = 30;

[[nodiscard]] ContentDB contestContent(std::initializer_list<PickupSpawner> spawners = {},
                                       uint8_t ammoMax = 1) {
  ContentDB c = defaultContent();
  addPowerUp(c, "more", 600, {statGrant(StatKind::AmmoMax, Fixed::one(), Fixed::fromInt(2))});
  c.player.ammoMax = ammoMax;
  ArenaDef& a = c.arenas[0];
  a.cellSize = Fixed::one();
  a.pickupSpawnerCount = 0;
  for (const PickupSpawner& sp : spawners)
    a.pickupSpawners[a.pickupSpawnerCount++] = sp;
  return c;
}

[[nodiscard]] PickupSpawner tree(Vec2 pos) {
  PickupSpawner sp;
  sp.pos = pos;
  sp.firstTicks = kFirst;
  sp.intervalTicks = 600;
  sp.maxAlive = 1;
  sp.tree = 1;
  return sp;
}

// A playing world of `players` with every hand set to `ammo` and every
// monkey far away from the spot, so no contact happens until a test puts
// one there.
[[nodiscard]] WorldState world(uint64_t seed, PlayerMask players, const ContentDB& c,
                               uint8_t ammo = 0) {
  WorldState s = makePlayingWorld(seed, players, c);
  uint8_t k = 0;
  s.players.forEachAlive([&](EntityId /*id*/, Player& p) {
    p.ammo = ammo;
    p.transform.pos = kFar + Vec2{Fixed::fromInt(2 * k), Fixed::zero()};
    p.transform.prevPos = p.transform.pos;
    p.transform.vel = Vec2::zero();
    ++k;
  });
  return s;
}

[[nodiscard]] std::vector<SimEvent> taken(const EventLog& log) {
  std::vector<SimEvent> out;
  for (const SimEvent& e : log) {
    if (e.kind == EventKind::PickupTaken) out.push_back(e);
  }
  return out;
}

// One idle tick; the events it pushed.
[[nodiscard]] EventLog tickOnce(WorldState& s, const ContentDB& c) {
  EventLog log;
  idle(s, 1, c, log);
  return log;
}

}  // namespace

TEST_CASE("pickup contest: the pickup rows are the taking rows", "[scenario][pickup-contest]") {
  for (const InteractionRule& r : interactionRules()) {
    INFO("row " << int{r.priority});
    CHECK((r.contend != nullptr) == (r.kindB == EntityKind::Pickup));
  }
}

TEST_CASE("pickup contest: two enemies at a tree on the tick it lays, each wins about half",
          "[scenario][pickup-contest]") {
  const ContentDB c = contestContent({tree(kSpot)});
  int west = 0;
  int east = 0;
  for (uint64_t seed = 1; seed <= kTrials; ++seed) {
    INFO("seed " << seed);
    WorldState s = world(seed, 0b11, c);
    placePlayer(s, kP0, kWest);
    placePlayer(s, kP1, kEast);
    EventLog log;
    for (int t = 0; t < 2 * kFirst && taken(log).empty(); ++t) {
      const EventLog one = tickOnce(s, c);
      log.insert(log.end(), one.begin(), one.end());
    }
    const std::vector<SimEvent> got = taken(log);
    REQUIRE(got.size() == 1);  // never both
    CHECK(countEvents(log, EventKind::PickupSpawned) == 1);
    CHECK(playerAt(s, kP0).ammo + playerAt(s, kP1).ammo == 1);
    (got[0].player == kP0 ? west : east) += 1;
  }
  INFO("west " << west << ", east " << east);
  CHECK(west + east == static_cast<int>(kTrials));
  CHECK(west >= kLow);
  CHECK(west <= kHigh);
}

TEST_CASE("pickup contest: one contender draws nothing", "[scenario][pickup-contest]") {
  const ContentDB c = contestContent();
  for (uint64_t seed = 1; seed <= 20; ++seed) {
    INFO("seed " << seed);
    WorldState s = world(seed, 0b111, c);
    spawnPickup(s, kSpot);
    placePlayer(s, kP1, kEast);  // slot 1 alone; slots 0 and 2 far away
    const Rng before = s.rng;
    const EventLog log = tickOnce(s, c);
    const std::vector<SimEvent> got = taken(log);
    REQUIRE(got.size() == 1);
    CHECK(got[0].player == kP1);
    CHECK(s.rng == before);
  }
  SECTION("and two that touch it draw one number") {
    WorldState s = world(5, 0b11, c);
    spawnPickup(s, kSpot);
    placePlayer(s, kP0, kWest);
    placePlayer(s, kP1, kEast);
    Rng once = s.rng;
    (void)once.below(2);
    (void)tickOnce(s, c);
    CHECK(s.rng == once);
  }
}

TEST_CASE("pickup contest: a full hand beside a hand with room is no contender",
          "[scenario][pickup-contest]") {
  const ContentDB c = contestContent();
  for (const bool lowFull : {true, false}) {
    INFO("the full hand is slot " << (lowFull ? 0 : 1));
    for (uint64_t seed = 1; seed <= 40; ++seed) {
      INFO("seed " << seed);
      WorldState s = world(seed, 0b11, c);
      spawnPickup(s, kSpot);
      placePlayer(s, kP0, kWest).ammo = lowFull ? 1 : 0;
      placePlayer(s, kP1, kEast).ammo = lowFull ? 0 : 1;
      const Rng before = s.rng;
      const EventLog log = tickOnce(s, c);
      const std::vector<SimEvent> got = taken(log);
      REQUIRE(got.size() == 1);
      CHECK(got[0].player == (lowFull ? kP1 : kP0));
      CHECK(playerAt(s, kP0).ammo == 1);
      CHECK(playerAt(s, kP1).ammo == 1);
      CHECK(s.rng == before);  // no draw
    }
  }
  SECTION("two full hands leave it lying and draw nothing") {
    WorldState s = world(3, 0b11, c, 1);
    const EntityId k = spawnPickup(s, kSpot);
    placePlayer(s, kP0, kWest);
    placePlayer(s, kP1, kEast);
    const Rng before = s.rng;
    const EventLog log = tickOnce(s, c);
    CHECK(taken(log).empty());
    CHECK(s.pickups.isAlive(k));
    CHECK(s.rng == before);
  }
}

TEST_CASE("pickup contest: a stack is taken in the drawn order, as far as each hand fits",
          "[scenario][pickup-contest]") {
  // A hand of three holding one has room for two: a stack of three gives
  // the first drawn two and the next one, and is gone.
  const ContentDB c = contestContent({}, 3);
  int lowFirst = 0;
  for (uint64_t seed = 1; seed <= kTrials; ++seed) {
    INFO("seed " << seed);
    WorldState s = world(seed, 0b11, c, 1);
    const EntityId k = spawnPickup(s, kSpot);
    s.pickups.items[k.index].count = 3;
    placePlayer(s, kP0, kWest);
    placePlayer(s, kP1, kEast);
    const EventLog log = tickOnce(s, c);
    const std::vector<SimEvent> got = taken(log);
    REQUIRE(got.size() == 2);
    const PlayerId first = got[0].player;
    const PlayerId second = got[1].player;
    REQUIRE(first != second);
    CHECK(playerAt(s, first).ammo == 3);
    CHECK(playerAt(s, second).ammo == 2);
    CHECK_FALSE(s.pickups.isAlive(k));
    if (first == kP0) ++lowFirst;
  }
  INFO("slot 0 first " << lowFirst << " of " << kTrials);
  CHECK(lowFirst >= kLow);
  CHECK(lowFirst <= kHigh);

  SECTION("a stack the first hand takes whole leaves the next nothing") {
    WorldState s = world(7, 0b11, c, 1);
    const EntityId k = spawnPickup(s, kSpot);
    s.pickups.items[k.index].count = 2;
    placePlayer(s, kP0, kWest);
    placePlayer(s, kP1, kEast);
    const EventLog log = tickOnce(s, c);
    const std::vector<SimEvent> got = taken(log);
    REQUIRE(got.size() == 1);
    CHECK(playerAt(s, got[0].player).ammo == 3);
    CHECK(playerAt(s, got[0].player == kP0 ? kP1 : kP0).ammo == 1);
  }
}

TEST_CASE("pickup contest: a power-up two monkeys touch goes to one, each about half",
          "[scenario][pickup-contest]") {
  const ContentDB c = contestContent();
  const uint8_t more = 0;  // the only power-up
  REQUIRE(c.powerUpCount == 1);
  int low = 0;
  for (uint64_t seed = 1; seed <= kTrials; ++seed) {
    INFO("seed " << seed);
    WorldState s = world(seed, 0b11, c);
    spawnPowerUpPickup(s, kSpot, more);
    placePlayer(s, kP0, kWest);
    placePlayer(s, kP1, kEast);
    const EventLog log = tickOnce(s, c);
    const std::vector<SimEvent> got = taken(log);
    REQUIRE(got.size() == 1);
    CHECK(got[0].a == static_cast<uint32_t>(PickupKind::PowerUp));
    const PlayerId winner = got[0].player;
    CHECK(playerAt(s, winner).modifiers.count == 1);
    CHECK(playerAt(s, winner == kP0 ? kP1 : kP0).modifiers.count == 0);
    if (winner == kP0) ++low;
  }
  INFO("slot 0 " << low << " of " << kTrials);
  CHECK(low >= kLow);
  CHECK(low <= kHigh);
}

TEST_CASE(
    "pickup contest: a flag two enemies of its team touch goes to one; its own team's "
    "monkey is no contender",
    "[scenario][pickup-contest]") {
  // A free-for-all: slot i is team i, so slot 2's flag (spawner 2) is
  // takeable by slots 0 and 1 and not by slot 2.
  const ContentDB c = contestContent();
  const auto flagAt = [](WorldState& s, uint8_t team) {
    const EntityId id = s.pickups.spawn();
    REQUIRE(id.isValid());
    Pickup& k = s.pickups.items[id.index];
    k.kind = PickupKind::Flag;
    k.pos = kSpot;
    k.spawner = team;
    return id;
  };
  int low = 0;
  for (uint64_t seed = 1; seed <= kTrials; ++seed) {
    INFO("seed " << seed);
    WorldState s = world(seed, 0b111, c);
    flagAt(s, 2);
    placePlayer(s, kP0, kWest);
    placePlayer(s, kP1, kEast);
    const EventLog log = tickOnce(s, c);
    const std::vector<SimEvent> got = taken(log);
    REQUIRE(got.size() == 1);
    CHECK(got[0].a == static_cast<uint32_t>(PickupKind::Flag));
    if (got[0].player == kP0) ++low;
  }
  INFO("slot 0 " << low << " of " << kTrials);
  CHECK(low >= kLow);
  CHECK(low <= kHigh);

  SECTION("the flag's own team beside an enemy: the enemy takes it, no draw") {
    for (uint64_t seed = 1; seed <= 20; ++seed) {
      WorldState s = world(seed, 0b111, c);
      flagAt(s, 0);
      placePlayer(s, kP0, kWest);  // its own flag
      placePlayer(s, kP2, kEast);
      const Rng before = s.rng;
      const EventLog log = tickOnce(s, c);
      const std::vector<SimEvent> got = taken(log);
      REQUIRE(got.size() == 1);
      CHECK(got[0].player == kP2);
      CHECK(s.rng == before);
    }
  }
}
