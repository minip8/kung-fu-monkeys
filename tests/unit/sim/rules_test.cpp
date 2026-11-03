// RulesSystem, FFA mode (architecture §5.3 steps 1 and 10; M1 contract
// rulesPreTick / rulesPostTick).
#include "kfm/sim/systems.hpp"
#include "sim_test_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <initializer_list>
#include <set>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

// Steps one idle tick and returns its events.
EventLog stepIdle(WorldState& s, const ContentDB& content = defaultContent()) {
  EventLog log;
  runTicks(s, frameFor(s), 1, content, log);
  return log;
}

// Despawns players directly (what LifecycleSystem does after an elimination).
void removePlayers(WorldState& s, std::initializer_list<uint8_t> slots) {
  for (const uint8_t slot : slots) {
    s.players.despawn(s.players.idAt(slot));
  }
}

// The spawn-point offset that explains every present player's position, or
// -1 when the positions are not a rotation of the spawn list in slot order.
int spawnOffsetOf(const WorldState& s, const ContentDB& content) {
  const ArenaDef& arena = content.arena;
  for (size_t offset = 0; offset < arena.spawnPointCount; ++offset) {
    bool all = true;
    size_t rank = 0;
    for (uint8_t i = 0; i < kMaxPlayers; ++i) {
      if (!maskHas(s.rules.present, PlayerId{i})) continue;
      const Player* p = s.playerBySlot(PlayerId{i});
      if (p == nullptr ||
          p->transform.pos != arena.spawnPoints[(rank + offset) % arena.spawnPointCount]) {
        all = false;
      }
      ++rank;
    }
    if (all) return static_cast<int>(offset);
  }
  return -1;
}

}  // namespace

TEST_CASE("lobby -> RoundStarting -> Playing timeline", "[sim][rules]") {
  const ContentDB& content = defaultContent();
  WorldState s = initialWorld(MatchConfig{7, 0b1000000101});

  const EventLog first = stepIdle(s);
  CHECK(s.rules.phase == MatchPhase::RoundStarting);
  CHECK(s.rules.round == 1);
  CHECK(s.rules.ticksInPhase == 1);
  CHECK(s.players.size() == 3);
  REQUIRE(first.size() == 5);
  CHECK(first[0].kind == EventKind::MatchStarted);
  CHECK(first[1].kind == EventKind::PlayerSpawned);
  CHECK(first[1].player == PlayerId{0});
  CHECK(first[2].player == PlayerId{2});
  CHECK(first[3].player == PlayerId{9});
  CHECK(first[3].entity == s.players.idAt(9));
  CHECK(first[4].kind == EventKind::RoundStarted);
  CHECK(first[4].a == 1);
  for (const SimEvent& e : first)
    CHECK(e.tick == Tick{0});

  // The countdown lasts exactly roundStartTicks ticks (0 .. roundStartTicks-1).
  for (uint32_t t = 1; t < content.mode.roundStartTicks; ++t) {
    CHECK(stepIdle(s).empty());
    REQUIRE(s.rules.phase == MatchPhase::RoundStarting);
  }
  CHECK(s.tick == Tick{content.mode.roundStartTicks});
  CHECK(stepIdle(s).empty());
  CHECK(s.rules.phase == MatchPhase::Playing);
  CHECK(s.rules.ticksInPhase == 1);
}

TEST_CASE("spawned players get content stats and a full reset", "[sim][rules]") {
  const ContentDB& content = defaultContent();
  WorldState s = initialWorld(MatchConfig{3, 0b11});
  static_cast<void>(stepIdle(s));
  for (uint8_t i = 0; i < 2; ++i) {
    const Player& p = *s.playerBySlot(PlayerId{i});
    CHECK(p.id == PlayerId{i});
    CHECK(p.hp == content.player.maxHp);
    CHECK(p.ammo == content.player.ammoStart);
    CHECK(p.stats.moveSpeed == content.player.moveSpeed);
    CHECK(p.stats.maxHp == content.player.maxHp);
    CHECK(p.stats.ammoMax == content.player.ammoMax);
    CHECK(p.transform.prevPos == p.transform.pos);
    CHECK(p.transform.vel == Vec2::zero());
    CHECK(p.abilities[kAbilitySlotDash].defIndex == content.player.abilities[kAbilitySlotDash]);
    CHECK(p.abilities[kAbilitySlotThrow].defIndex == content.player.abilities[kAbilitySlotThrow]);
  }
  CHECK(spawnOffsetOf(s, content) >= 0);
}

TEST_CASE("a zero-length countdown goes straight to Playing", "[sim][rules]") {
  ContentDB content = defaultContent();
  content.mode.roundStartTicks = 0;
  WorldState s = initialWorld(MatchConfig{1, 0b11});
  static_cast<void>(stepIdle(s, content));
  CHECK(s.rules.phase == MatchPhase::Playing);
}

TEST_CASE("round ends when one player is left; winner scores", "[sim][rules]") {
  WorldState s = makePlayingWorld(1, 0b1011);
  removePlayers(s, {3});
  EventSink sink;
  rulesPostTick(s, defaultContent(), sink);
  CHECK(s.rules.phase == MatchPhase::Playing);  // two still standing
  CHECK(sink.count == 0);

  removePlayers(s, {0});
  rulesPostTick(s, defaultContent(), sink);
  CHECK(s.rules.phase == MatchPhase::RoundEnded);
  CHECK(s.rules.ticksInPhase == 0);
  CHECK(s.rules.lastRoundWinner == PlayerId{1});
  CHECK(s.rules.scores[1] == 1);
  CHECK(s.rules.scores[0] == 0);
  REQUIRE(sink.count == 1);
  CHECK(sink.events[0].kind == EventKind::RoundEnded);
  CHECK(sink.events[0].player == PlayerId{1});
  CHECK(sink.events[0].a == 1);
}

TEST_CASE("round is a draw when nobody is left", "[sim][rules]") {
  WorldState s = makePlayingWorld(1, 0b11);
  removePlayers(s, {0, 1});
  EventSink sink;
  rulesPostTick(s, defaultContent(), sink);
  CHECK(s.rules.phase == MatchPhase::RoundEnded);
  CHECK_FALSE(s.rules.lastRoundWinner.isValid());
  CHECK(s.rules.scores[0] == 0);
  CHECK(s.rules.scores[1] == 0);
  REQUIRE(sink.count == 1);
  CHECK(sink.events[0].kind == EventKind::RoundEnded);
  CHECK_FALSE(sink.events[0].player.isValid());
}

TEST_CASE("a solo match never ends a round", "[sim][rules]") {
  WorldState s = makePlayingWorld(1, 0b1);
  EventLog log;
  runTicks(s, frameFor(s), 300, defaultContent(), log);
  CHECK(s.rules.phase == MatchPhase::Playing);
  CHECK(log.empty());
}

TEST_CASE("RoundEnded lasts roundEndTicks, then everyone respawns", "[sim][rules]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(4, 0b111);
  spawnBanana(s, PlayerId{0}, Vec2::zero(), Vec2::zero());
  spawnPickup(s, Vec2::zero());
  removePlayers(s, {1, 2});
  // Player 0 survives the round battered: everything must be reset.
  Player& p0 = *s.playerBySlot(PlayerId{0});
  p0.hp = 0;
  p0.ammo = 0;
  p0.tags.set(Tag::HitStun);
  p0.hitStunTicks = 3;
  p0.modifiers.count = 1;
  p0.stats.moveSpeed = Fixed::zero();
  p0.transform.vel = Vec2{Fixed::one(), Fixed::one()};
  p0.abilities[kAbilitySlotDash].phase = AbilityPhase::Active;
  p0.abilities[kAbilitySlotDash].cooldownTicks = 30;

  const EventLog end = stepIdle(s);  // rulesPostTick sees one player left
  REQUIRE(countEvents(end, EventKind::RoundEnded) == 1);
  REQUIRE(s.rules.phase == MatchPhase::RoundEnded);
  // Player 0 holds Dash through the pause and into the next round.
  InputFrame holdDash = frameFor(s);
  holdDash.inputs[0].buttons = buttonBit(Button::Dash);
  for (uint32_t t = 0; t < content.mode.roundEndTicks; ++t) {
    REQUIRE(s.rules.phase == MatchPhase::RoundEnded);
    EventLog quiet;
    runTicks(s, holdDash, 1, content, quiet);
    CHECK(quiet.empty());
  }
  EventLog next;
  runTicks(s, holdDash, 1, content, next);
  CHECK(s.rules.phase == MatchPhase::RoundStarting);
  CHECK(s.rules.round == 2);
  CHECK(countEvents(next, EventKind::PlayerSpawned) == 3);
  REQUIRE(findEvent(next, EventKind::RoundStarted) != nullptr);
  CHECK(findEvent(next, EventKind::RoundStarted)->a == 2);
  CHECK(countEvents(next, EventKind::MatchStarted) == 0);
  CHECK(s.players.size() == 3);
  CHECK(s.boomerangs.empty());
  CHECK(s.pickups.empty());
  CHECK(spawnOffsetOf(s, content) >= 0);

  const Player& r0 = *s.playerBySlot(PlayerId{0});
  CHECK(r0.hp == content.player.maxHp);
  CHECK(r0.ammo == content.player.ammoStart);
  CHECK(r0.tags == Tags{});
  CHECK(r0.hitStunTicks == 0);
  CHECK(r0.modifiers.count == 0);
  CHECK(r0.stats.moveSpeed == content.player.moveSpeed);
  CHECK(r0.transform.vel == Vec2::zero());
  CHECK(r0.abilities[kAbilitySlotDash].phase == AbilityPhase::Idle);
  CHECK(r0.abilities[kAbilitySlotDash].cooldownTicks == 0);
  // A button held across the boundary is not a fresh press.
  CHECK(r0.intent.held == buttonBit(Button::Dash));
  CHECK(r0.intent.pressed == 0);
  // Scores survive the new round.
  CHECK(s.rules.scores[0] == 1);
}

TEST_CASE("match ends when a player reaches roundsToWin", "[sim][rules]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(1, 0b11);
  s.rules.scores[1] = static_cast<uint8_t>(content.mode.roundsToWin - 1);
  s.rules.round = content.mode.roundsToWin;
  removePlayers(s, {0});
  const EventLog log = stepIdle(s);
  CHECK(s.rules.phase == MatchPhase::MatchEnded);
  CHECK(s.rules.scores[1] == content.mode.roundsToWin);
  REQUIRE(log.size() == 2);
  CHECK(log[0].kind == EventKind::RoundEnded);
  CHECK(log[0].player == PlayerId{1});
  CHECK(log[1].kind == EventKind::MatchEnded);
  CHECK(log[1].player == PlayerId{1});

  // Terminal: the sim keeps ticking, nothing restarts.
  EventLog after;
  runTicks(s, frameFor(s), content.mode.roundEndTicks * 3, content, after);
  CHECK(after.empty());
  CHECK(s.rules.phase == MatchPhase::MatchEnded);
}

TEST_CASE("spawn offset is drawn from the world RNG, deterministically", "[sim][rules]") {
  const ContentDB& content = defaultContent();
  std::set<int> offsets;
  for (uint64_t seed = 0; seed < 32; ++seed) {
    WorldState a = initialWorld(MatchConfig{seed, 0b1111});
    WorldState b = initialWorld(MatchConfig{seed, 0b1111});
    const Rng before = a.rng;
    static_cast<void>(stepIdle(a));
    static_cast<void>(stepIdle(b));
    REQUIRE(checksum(a) == checksum(b));
    CHECK(a.rng != before);
    const int offset = spawnOffsetOf(a, content);
    REQUIRE(offset >= 0);
    offsets.insert(offset);
  }
  CHECK(offsets.size() > 1);
}

TEST_CASE("spawn uses ranks among present slots, not raw slots", "[sim][rules]") {
  const ContentDB& content = defaultContent();
  WorldState s = initialWorld(MatchConfig{9, 0b1000100000});
  static_cast<void>(stepIdle(s));
  const int offset = spawnOffsetOf(s, content);
  REQUIRE(offset >= 0);
  const auto first = static_cast<size_t>(offset);
  CHECK(s.playerBySlot(PlayerId{5})->transform.pos == content.arena.spawnPoints[first]);
  CHECK(s.playerBySlot(PlayerId{9})->transform.pos ==
        content.arena.spawnPoints[(first + 1) % content.arena.spawnPointCount]);
}
