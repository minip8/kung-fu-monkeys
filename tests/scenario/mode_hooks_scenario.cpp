// Mode foundations (architecture §5.3, v0.31): the ModeBehavior hooks
// onRoundStart and onPickupTaken and the mode-owned MatchRules::modeData.
// A probe mode (the rounds behaviour plus counters in modeData) runs through
// MatchContent::modeBehavior, the test seam that replaces the kind's row, so
// the call sites are exercised without a ModeKind of its own.
#include "kfm/core/bitstream.hpp"
#include "kfm/sim/modes/modes.hpp"
#include "kfm/sim/rules/handlers.hpp"
#include "kfm/sim/visit.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};

// modeData words the probe writes.
enum ProbeWord : uint8_t {
  RoundStarts = 0,    // onRoundStart calls
  StartRound,         // MatchRules::round seen by the last onRoundStart
  StartSpawned,       // monkeys alive when it ran
  StartCountdown,     // 1 when it ran in RoundStarting at ticksInPhase 0
  Pickups,            // onPickupTaken calls
  PickupKindPlus1,    // the last pickup's kind + 1
  PickupPlayerPlus1,  // the last taker's slot + 1
  PickupGone,         // 1 when the pickup was already despawned
};
static_assert(PickupGone < kModeDataWords);

void probeRoundStart(WorldState& s, const MatchContent& /*content*/, EventSink& /*sink*/) {
  std::array<uint32_t, kModeDataWords>& d = s.rules.modeData;
  ++d[RoundStarts];
  d[StartRound] = s.rules.round;
  d[StartSpawned] = static_cast<uint32_t>(s.players.size());
  d[StartCountdown] =
      s.rules.phase == MatchPhase::RoundStarting && s.rules.ticksInPhase == 0 ? 1 : 0;
}

void probePickupTaken(WorldState& s, const MatchContent& /*content*/, PlayerId player,
                      const Pickup& pickup, EventSink& /*sink*/) {
  std::array<uint32_t, kModeDataWords>& d = s.rules.modeData;
  ++d[Pickups];
  d[PickupKindPlus1] = static_cast<uint32_t>(pickup.kind) + 1;
  d[PickupPlayerPlus1] = uint32_t{player.value} + 1;
  bool gone = true;
  s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& p) {
    if (p.pos == pickup.pos) gone = false;
  });
  d[PickupGone] = gone ? 1 : 0;
}

constexpr ModeBehavior kProbe{&probeRoundStart, &roundsOnPlaying, &roundsOnEliminated,
                              &probePickupTaken};

[[nodiscard]] MatchContent probeContent(const ContentDB& db) {
  MatchContent content(db);
  content.modeBehavior = &kProbe;
  return content;
}

// A banana thrown by `attacker` lands on `victim` (one-hit kill by default).
void eliminate(WorldState& s, const MatchContent& content, PlayerId victim, PlayerId attacker) {
  const Player& p = playerAt(s, victim);
  const EntityId banana = spawnBanana(s, attacker, p.transform.pos, Vec2::zero());
  Contact c;
  c.kindA = EntityKind::Player;
  c.a = s.players.idAt(victim.value);
  c.kindB = EntityKind::Boomerang;
  c.b = banana;
  c.point = p.transform.pos;
  EventSink sink;
  REQUIRE(damagePlayer(s, content, c.a, c.b, c, sink));
}

}  // namespace

TEST_CASE("modes: onRoundStart runs after the shared spawn, before the countdown",
          "[scenario][modes][hooks]") {
  const MatchContent content = probeContent(defaultContent());
  WorldState s = initialWorld(MatchConfig{3, 0b11});
  EventLog log;
  runTicks(s, frameFor(s), 1, content, log);  // Lobby -> MatchStarted, round 1
  const std::array<uint32_t, kModeDataWords>& d = s.rules.modeData;
  CHECK(d[RoundStarts] == 1);
  CHECK(d[StartRound] == 1);
  CHECK(d[StartSpawned] == 2);  // both present slots already spawned
  CHECK(d[StartCountdown] == 1);
  CHECK(countEvents(log, EventKind::RoundStarted) == 1);

  // Through the countdown and play: no further call until the next round.
  for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i)
    runTicks(s, frameFor(s), 1, content, log);
  REQUIRE(s.rules.phase == MatchPhase::Playing);
  CHECK(d[RoundStarts] == 1);
  eliminate(s, content, kP1, kP0);
  runTicks(s, frameFor(s), 1, content, log);  // the round ends
  REQUIRE(s.rules.phase == MatchPhase::RoundEnded);
  for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::RoundStarting; ++i)
    runTicks(s, frameFor(s), 1, content, log);
  REQUIRE(s.rules.phase == MatchPhase::RoundStarting);
  CHECK(d[RoundStarts] == 2);
  CHECK(d[StartRound] == 2);
  CHECK(d[StartSpawned] == 2);
  CHECK(d[StartCountdown] == 1);
  CHECK(s.rules.ticksInPhase == 1);  // the post-tick already advanced the clock
}

TEST_CASE("modes: onPickupTaken runs right after a pickup row applied",
          "[scenario][modes][hooks]") {
  ContentDB db = defaultContent();
  const uint8_t boost =
      addPowerUp(db, "boost", 120, {statGrant(StatKind::MoveSpeed, Fixed::fromInt(2), Fixed{})});
  const MatchContent content = probeContent(db);
  WorldState s = makePlayingWorld(5, 0b11, db);
  s.rules.modeData = {};  // the probe's round start counted already
  EventLog log;

  SECTION("a banana pickup (row 50)") {
    Player& p = placePlayer(s, kP1, Vec2{Fixed::fromInt(4), Fixed::fromInt(2)});
    p.ammo = 0;
    spawnPickup(s, p.transform.pos);
    runTicks(s, frameFor(s), 1, content, log);
    REQUIRE(countEvents(log, EventKind::PickupTaken) == 1);
    CHECK(s.rules.modeData[Pickups] == 1);
    CHECK(s.rules.modeData[PickupKindPlus1] == static_cast<uint32_t>(PickupKind::Banana) + 1);
    CHECK(s.rules.modeData[PickupPlayerPlus1] == 2);
    CHECK(s.rules.modeData[PickupGone] == 1);
  }
  SECTION("a power-up pickup (row 51)") {
    const Player& p = placePlayer(s, kP0, Vec2{-Fixed::fromInt(4), Fixed::fromInt(2)});
    spawnPowerUpPickup(s, p.transform.pos, boost);
    runTicks(s, frameFor(s), 1, content, log);
    REQUIRE(countEvents(log, EventKind::PickupTaken) == 1);
    CHECK(s.rules.modeData[Pickups] == 1);
    CHECK(s.rules.modeData[PickupKindPlus1] == static_cast<uint32_t>(PickupKind::PowerUp) + 1);
    CHECK(s.rules.modeData[PickupPlayerPlus1] == 1);
    CHECK(s.rules.modeData[PickupGone] == 1);
  }
  SECTION("a pickup the row declines (full ammo) calls nothing") {
    const Player& p = placePlayer(s, kP1, Vec2{Fixed::fromInt(4), Fixed::fromInt(2)});
    REQUIRE(p.ammo >= p.stats.ammoMax);
    spawnPickup(s, p.transform.pos);
    runTicks(s, frameFor(s), 1, content, log);
    CHECK(countEvents(log, EventKind::PickupTaken) == 0);
    CHECK(s.rules.modeData[Pickups] == 0);
  }
}

TEST_CASE("modes: modeData is cleared at match start and the shipped kinds leave it alone",
          "[scenario][modes][hooks]") {
  WorldState s = initialWorld(MatchConfig{9, 0b111});
  s.rules.modeData = {1, 2, 3, 4, 5, 6, 7, 8};  // left over from nothing: a fresh match clears it
  EventLog log;
  runTicks(s, frameFor(s), 1, defaultContent(), log);
  CHECK(s.rules.modeData == std::array<uint32_t, kModeDataWords>{});
  // The rounds kind's hooks are no-ops: a whole scripted stretch keeps it 0.
  runTicks(s, frameFor(s), 600, defaultContent(), log);
  CHECK(s.rules.modeData == std::array<uint32_t, kModeDataWords>{});
}

TEST_CASE("modes: modeData is part of the checksum and the snapshot", "[scenario][modes][hooks]") {
  const WorldState a = makePlayingWorld(4, 0b11);
  WorldState b = a;
  CHECK(checksum(a) == checksum(b));
  b.rules.modeData[kModeDataWords - 1] = 1;
  CHECK(checksum(a) != checksum(b));

  std::array<std::byte, 16384> buf{};
  BitWriter w(buf);
  REQUIRE(writeTo(w, b));
  WorldState c{};
  BitReader r(w.written());
  REQUIRE(readFrom(r, c));
  CHECK(c.rules.modeData == b.rules.modeData);
  CHECK(checksum(c) == checksum(b));
}
