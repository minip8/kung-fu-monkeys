#include "kfm/sim/step.hpp"

#include "kfm/core/rng.hpp"
#include "kfm/sim/world_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <type_traits>
#include <vector>

using namespace kfm;
using namespace kfm::sim;

namespace {
InputFrame emptyFrame(const WorldState& s) {
  InputFrame f;
  f.tick = s.tick;
  f.present = s.rules.present;
  return f;
}
}  // namespace

TEST_CASE("WorldState is trivially copyable and small", "[sim][step]") {
  STATIC_REQUIRE(std::is_trivially_copyable_v<WorldState>);
  STATIC_REQUIRE(sizeof(WorldState) < size_t{64} * 1024);
  STATIC_REQUIRE(sizeof(PlayerInput) == 5);
}

TEST_CASE("initialWorld is deterministic and starts in the lobby", "[sim][step]") {
  const WorldState a = initialWorld(MatchConfig{1, 0x3FF});
  const WorldState b = initialWorld(MatchConfig{1, 0x3FF});
  // Compared through the visitor: padding bytes are not deterministic, so a
  // memcmp of two independently built states is not a valid equality test.
  CHECK(checksum(a) == checksum(b));
  CHECK(a.tick == Tick{0});
  CHECK(a.rules.phase == MatchPhase::Lobby);
  CHECK(a.rules.present == 0x3FF);
  CHECK(a.players.empty());
  const WorldState c = initialWorld(MatchConfig{2, 0x3FF});
  CHECK(a.rng != c.rng);
}

TEST_CASE("first step spawns present players into their slots", "[sim][step]") {
  WorldState s = initialWorld(MatchConfig{7, 0b1000000101});
  EventSink sink;
  step(s, emptyFrame(s), defaultContent(), sink);
  CHECK(s.tick == Tick{1});
  // The first tick starts round 1: players spawn and the countdown begins.
  CHECK(s.rules.phase == MatchPhase::RoundStarting);
  CHECK(s.players.size() == 3);
  REQUIRE(s.playerBySlot(PlayerId{0}) != nullptr);
  REQUIRE(s.playerBySlot(PlayerId{2}) != nullptr);
  REQUIRE(s.playerBySlot(PlayerId{9}) != nullptr);
  CHECK(s.playerBySlot(PlayerId{1}) == nullptr);
  CHECK(s.playerBySlot(PlayerId::invalid()) == nullptr);
  CHECK(s.playerBySlot(PlayerId{2})->id == PlayerId{2});
  // Spawned at the arena spawn point of their slot, with content stats.
  const ContentDB& content = defaultContent();
  const Player& p9 = *s.playerBySlot(PlayerId{9});
  // Spawn points go by rank among present slots plus a per-round offset drawn
  // from the world RNG: seed 7 draws offset 2, slot 9 has rank 2 -> point 4.
  {
    Rng rng = Rng::seeded(7);
    REQUIRE(rng.below(content.arena.spawnPointCount) == 2);
  }
  CHECK(p9.transform.pos == content.arena.spawnPoints[4]);
  CHECK(p9.hp == content.player.maxHp);
  CHECK(p9.ammo == content.player.ammoStart);
  CHECK(p9.stats.moveSpeed == content.player.moveSpeed);
  CHECK(p9.abilities[kAbilitySlotDash].defIndex == static_cast<uint8_t>(AbilityKind::Dash));
  CHECK(p9.abilities[kAbilitySlotThrow].defIndex == static_cast<uint8_t>(AbilityKind::Throw));

  int spawned = 0;
  int started = 0;
  for (const SimEvent& e : sink) {
    if (e.kind == EventKind::PlayerSpawned) ++spawned;
    if (e.kind == EventKind::MatchStarted) ++started;
  }
  CHECK(spawned == 3);
  CHECK(started == 1);
  CHECK(sink.dropped == 0);

  // Second step: no respawn, no new events.
  sink.clear();
  step(s, emptyFrame(s), defaultContent(), sink);
  CHECK(s.players.size() == 3);
  CHECK(sink.count == 0);
  CHECK(s.rules.ticksInPhase == 2);
}

TEST_CASE("stepping the same inputs from the same seed is bit-identical", "[sim][step]") {
  WorldState a = initialWorld(MatchConfig{99, 0x3FF});
  WorldState b = initialWorld(MatchConfig{99, 0x3FF});
  EventSink sink;
  for (int i = 0; i < 120; ++i) {
    InputFrame f = emptyFrame(a);
    f.inputs[3].moveDir = static_cast<uint8_t>(i);
    f.inputs[3].aimDir = static_cast<uint8_t>(i);
    f.inputs[3].buttons = static_cast<uint8_t>(i % 4);
    step(a, f, defaultContent(), sink);
    step(b, f, defaultContent(), sink);
    REQUIRE(checksum(a) == checksum(b));
  }
  CHECK(a.playerBySlot(PlayerId{3})->transform.facing == Angle::fromInput8(119));
}

TEST_CASE("rollback by memcpy restores an identical state", "[sim][step]") {
  // Reach Playing so movement, abilities and bananas are live, then snapshot,
  // run a timeline with moving, throwing and dashing inputs, roll back and
  // re-simulate it: the re-simulated timeline must match tick for tick.
  WorldState s = initialWorld(MatchConfig{5, 0x3});
  EventSink sink;
  for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i) {
    sink.clear();
    step(s, emptyFrame(s), defaultContent(), sink);
  }
  REQUIRE(s.rules.phase == MatchPhase::Playing);

  const auto frameAt = [](const WorldState& w, int i) {
    InputFrame f = emptyFrame(w);
    f.inputs[0].moveDir = static_cast<uint8_t>(200 + i);
    f.inputs[0].moveMag = 255;
    f.inputs[0].aimDir = static_cast<uint8_t>(i * 7);
    f.inputs[0].buttons = (i % 8 < 2) ? buttonBit(Button::Throw) : uint8_t{0};
    f.inputs[1].moveDir = 64;
    f.inputs[1].moveMag = 128;
    f.inputs[1].buttons = (i % 10 == 3) ? buttonBit(Button::Dash) : uint8_t{0};
    f.inputs[1].subTick = static_cast<uint8_t>(i * 31);
    return f;
  };

  WorldState saved{};
  std::memcpy(&saved, &s, sizeof(WorldState));
  const uint32_t savedSum = checksum(s);
  constexpr int kTicks = 40;
  std::vector<uint32_t> original;
  for (int i = 0; i < kTicks; ++i) {
    sink.clear();
    step(s, frameAt(s, i), defaultContent(), sink);
    original.push_back(checksum(s));
  }
  CHECK(checksum(s) != savedSum);

  std::memcpy(&s, &saved, sizeof(WorldState));
  CHECK(checksum(s) == savedSum);
  for (int i = 0; i < kTicks; ++i) {
    sink.clear();
    step(s, frameAt(s, i), defaultContent(), sink);
    INFO("re-simulated tick " << i);
    REQUIRE(checksum(s) == original[static_cast<size_t>(i)]);
  }
}
