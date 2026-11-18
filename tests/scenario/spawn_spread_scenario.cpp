// Free-for-all round starts spread the players (architecture §5.3, v0.91):
// the n present slots start on the n spawn points that lie farthest apart,
// dealt over them afresh each round by the world Rng. Measured on the
// shipped dojo (ten points on a 12 x 6 ellipse) and marsh (the largest
// arena) for 2 to 10 players: the closest two monkeys at a round start are
// never nearer than the best the arena's points allow for that count. The
// M1 deal (one Rng offset into the list) put two of five or more players on
// the dojo 4.2 units apart on some offsets; 9 and 10 players fill every
// point, so their floor is the points' own.
#include "kfm/content/loader.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

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

// The closest pair of present monkeys, squared (raw Q32.32).
uint64_t closestPairSq(const WorldState& s) {
  uint64_t best = std::numeric_limits<uint64_t>::max();
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const Player* a = s.playerBySlot(PlayerId{i});
    if (a == nullptr) continue;
    for (uint8_t j = static_cast<uint8_t>(i + 1); j < kMaxPlayers; ++j) {
      const Player* b = s.playerBySlot(PlayerId{j});
      if (b == nullptr) continue;
      best = std::min(best, lengthSqRaw(a->transform.pos - b->transform.pos));
    }
  }
  return best;
}

uint64_t unitsSq(int32_t tenths) {
  return lengthSqRaw(Vec2{Fixed::fromRatio(tenths, 10), Fixed::zero()});
}

// Plays the countdown of `rounds` round starts (the round ended by hand) and
// checks every one against the floor; returns the points slot 0 started on.
std::set<std::pair<int32_t, int32_t>> checkRounds(uint8_t arena, uint32_t players,
                                                  int32_t floorTenths, int rounds) {
  const MatchConfig config{.seed = 0x5B12 + players,
                           .players = static_cast<PlayerMask>((1u << players) - 1u),
                           .arena = arena};
  const MatchContent m = matchContent(shipped(), config);
  WorldState s = initialWorld(config);
  EventLog log;
  std::set<std::pair<int32_t, int32_t>> starts;
  for (int r = 0; r < rounds; ++r) {
    for (int i = 0; i < 2000 && s.rules.phase != MatchPhase::RoundStarting; ++i)
      runTicks(s, frameFor(s), 1, m, log);
    REQUIRE(s.rules.phase == MatchPhase::RoundStarting);
    REQUIRE(s.players.size() == players);
    INFO("round " << r);
    CHECK(closestPairSq(s) >= unitsSq(floorTenths));
    const Vec2 p0 = playerAt(s, PlayerId{0}).transform.pos;
    starts.insert({p0.x.raw, p0.y.raw});
    // End the round as a draw and wait out the pause.
    s.rules.phase = MatchPhase::RoundEnded;
    s.rules.ticksInPhase = 0;
  }
  return starts;
}

}  // namespace

TEST_CASE("free-for-all round starts spread 2 to 10 players on the dojo (v0.91)",
          "[scenario][spawn][spread]") {
  // The best closest pair the dojo's ten points allow, in tenths of a unit
  // (rounded down): 2 players the long axis, 4 the four points at 36 and 144
  // degrees from it, ...
  constexpr std::array<int32_t, 11> kFloor = {0, 0, 240, 114, 110, 74, 74, 63, 63, 42, 42};
  const std::optional<uint8_t> dojo = shipped().findArena("dojo");
  REQUIRE(dojo);
  for (uint32_t n = 2; n <= 10; ++n) {
    INFO(n << " players");
    const auto starts = checkRounds(dojo.value_or(0), n, kFloor[n], 6);
    // Each round deals the points afresh: slot 0 does not always start on
    // the same one.
    CHECK(starts.size() > 1);
  }
}

TEST_CASE("free-for-all round starts spread 2 to 10 players on marsh (v0.91)",
          "[scenario][spawn][spread]") {
  constexpr std::array<int32_t, 11> kFloor = {0, 0, 480, 271, 220, 160, 160, 136, 136, 136, 136};
  const std::optional<uint8_t> marsh = shipped().findArena("marsh");
  REQUIRE(marsh);
  for (uint32_t n = 2; n <= 10; ++n) {
    INFO(n << " players");
    static_cast<void>(checkRounds(marsh.value_or(0), n, kFloor[n], 3));
  }
}
