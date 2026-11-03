// MatchConfig::arena / ::mode select the defs a match runs with (architecture
// §5.10): the arena's spawn points and bounds, the mode's timers and win
// condition; extra arenas and modes in the content never change the default
// match.
#include "kfm/sim/scripted_inputs.hpp"
#include "scenario_util.hpp"

#include <algorithm>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

// The default content plus a small "pit" arena and a one-round "sudden" mode.
ContentDB twoOfEach() {
  ContentDB c = defaultContent();
  ArenaDef& pit = c.arenas[1];
  pit.name = ContentName::of("pit");
  pit.halfExtents = Vec2{Fixed::fromInt(5), Fixed::fromInt(3)};
  pit.spawnPoints[0] = Vec2{-Fixed::fromInt(2), Fixed::zero()};
  pit.spawnPoints[1] = Vec2{Fixed::fromInt(2), Fixed::zero()};
  pit.spawnPointCount = 2;
  c.arenaCount = 2;
  ModeDef& sudden = c.modes[1];
  sudden.name = ContentName::of("sudden");
  sudden.roundsToWin = 1;
  sudden.roundStartTicks = 10;
  sudden.roundEndTicks = 20;
  c.modeCount = 2;
  return c;
}

}  // namespace

TEST_CASE("extra arenas and modes leave the default match bit-identical", "[scenario][config]") {
  const ContentDB c = twoOfEach();
  const MatchConfig config{.seed = 0xC0F, .players = 0b1111};
  WorldState a = initialWorld(config);
  WorldState b = initialWorld(config);
  Rng ia = Rng::seeded(5);
  Rng ib = Rng::seeded(5);
  EventSink sink;
  for (int t = 0; t < 1500; ++t) {
    const InputFrame fa = scriptedFrame(ia, a, defaultContent());
    sink.clear();
    step(a, fa, defaultContent(), sink);
    const InputFrame fb = scriptedFrame(ib, b, matchContent(c, config));
    sink.clear();
    step(b, fb, matchContent(c, config), sink);
    REQUIRE(checksum(a) == checksum(b));
  }
}

TEST_CASE("the selected arena places and bounds the players", "[scenario][config]") {
  const ContentDB c = twoOfEach();
  const MatchConfig config{.seed = 0xA4E, .players = 0b11, .arena = 1, .mode = 1};
  const MatchContent m = matchContent(c, config);
  REQUIRE(&m.arena == &c.arenas[1]);
  REQUIRE(&m.mode == &c.modes[1]);

  WorldState s = initialWorld(config);
  EventLog log;
  runTicks(s, frameFor(s), 1, m, log);
  REQUIRE(s.rules.phase == MatchPhase::RoundStarting);
  for (uint8_t i = 0; i < 2; ++i) {
    const Vec2 p = playerAt(s, PlayerId{i}).transform.pos;
    CHECK((p == c.arenas[1].spawnPoints[0] || p == c.arenas[1].spawnPoints[1]));
  }
  // The mode's countdown, not the default one.
  runTicks(s, frameFor(s), m.mode.roundStartTicks, m, log);
  CHECK(s.rules.phase == MatchPhase::Playing);

  // Walking right stops at the small arena's wall.
  InputFrame right = frameFor(s);
  right.inputs[0].moveMag = 255;
  right.inputs[0].moveDir = 0;
  runTicks(s, right, 240, m, log);
  CHECK(playerAt(s, PlayerId{0}).transform.pos.x == m.arena.halfExtents.x - c.player.radius);
}

TEST_CASE("the selected mode decides when the match ends", "[scenario][config]") {
  const ContentDB c = twoOfEach();
  const MatchConfig config{.seed = 0x5DD, .players = 0b111, .arena = 0, .mode = 1};
  const MatchContent m = matchContent(c, config);
  WorldState s = initialWorld(config);
  Rng inputs = Rng::seeded(0x5DD);
  EventSink sink;
  int rounds = 0;
  for (int t = 0; t < 60 * 120 && s.rules.phase != MatchPhase::MatchEnded; ++t) {
    const InputFrame f = scriptedFrame(inputs, s, m);
    sink.clear();
    step(s, f, m, sink);
    rounds += static_cast<int>(std::count_if(sink.begin(), sink.end(), [](const SimEvent& e) {
      return e.kind == EventKind::RoundEnded;
    }));
  }
  REQUIRE(s.rules.phase == MatchPhase::MatchEnded);
  // One round win ends a "sudden" match (draws do not score).
  CHECK(*std::ranges::max_element(s.rules.scores) == 1);
  CHECK(rounds >= 1);
}
