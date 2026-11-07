// Banana supply (architecture §5.6, §5.7, §12): what the M2 feel-tuning
// decision about a perfect catch at full ammo rests on.
//
// - A perfect catch needs room below ammoMax (v0.16): a full-ammo perfect
//   dash into a non-own banana declines the catch and takes the hit, and the
//   banana flies on through it like any other hit (v0.27).
// - Supply = ammo held + bananas in flight + pickups. It never reaches 0
//   while two or more players are alive (a banana that hits flies on and
//   comes home or grounds; a dash attack leaves the attacker holding its
//   ammo), so a round can always still end. Checked over scripted matches.
#include "kfm/core/rng.hpp"
#include "kfm/sim/scripted_inputs.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kAimLeft = 128;  // half a turn: toward -x
constexpr uint8_t kAimRight = 0;

[[nodiscard]] uint32_t supplyOf(const WorldState& s) {
  uint32_t supply = 0;
  s.players.forEachAlive([&](EntityId /*id*/, const Player& p) { supply += p.ammo; });
  supply += s.boomerangs.size();
  supply += s.pickups.size();
  return supply;
}

}  // namespace

TEST_CASE("a perfect dash at full ammo into a non-own banana takes the hit",
          "[scenario][catch][supply]") {
  const ContentDB& content = defaultContent();
  // Three players so the round goes on after the hit.
  WorldState s = makePlayingWorld(31, 0b111);
  placePlayer(s, PlayerId{0}, Vec2{Fixed::fromInt(-4), Fixed::zero()});
  placePlayer(s, PlayerId{1}, Vec2{Fixed::fromInt(4), Fixed::zero()});
  placePlayer(s, PlayerId{2}, Vec2{Fixed::zero(), Fixed::fromInt(7)});
  REQUIRE(playerAt(s, PlayerId{0}).ammo == content.player.ammoMax);
  REQUIRE(supplyOf(s) == 3);

  InputFrame f = frameFor(s);
  f.inputs[0].aimDir = kAimRight;
  f.inputs[1].aimDir = kAimLeft;
  f.inputs[1].buttons = buttonBit(Button::Throw);
  EventLog log;
  runTicks(s, f, 1, content, log);
  f.inputs[1].buttons = 0;

  // Player 0, holding its own banana, dashes into the incoming one at a gap
  // of 2 (inside the perfect window, as in catch_scenario.cpp).
  bool dashed = false;
  for (int t = 0; t < 60 && countEvents(log, EventKind::PlayerHit) == 0; ++t) {
    f.inputs[0].buttons = 0;
    const Player& p0 = playerAt(s, PlayerId{0});
    if (!dashed) {
      s.boomerangs.forEachAlive([&](EntityId /*id*/, const Boomerang& b) {
        if (b.transform.pos.x - p0.transform.pos.x <= Fixed::fromInt(2)) dashed = true;
      });
      if (dashed) f.inputs[0].buttons = buttonBit(Button::Dash);
    }
    runTicks(s, f, 1, content, log);
  }
  REQUIRE(dashed);
  CHECK(countEvents(log, EventKind::BoomerangCaught) == 0);
  const SimEvent* hit = findEvent(log, EventKind::PlayerHit);
  REQUIRE(hit != nullptr);
  CHECK(hit->player == PlayerId{0});
  CHECK(hit->a == 1);
  CHECK(countEvents(log, EventKind::PlayerEliminated) == 1);
  CHECK(s.playerBySlot(PlayerId{0}) == nullptr);
  REQUIRE(s.rules.phase == MatchPhase::Playing);

  // The hitting banana flies on toward its thrower; v0.57: the victim's held
  // banana dropped where it died instead of vanishing. Supply: all three.
  CHECK(playerAt(s, PlayerId{1}).ammo == 0);
  CHECK(s.boomerangs.size() == 1);
  CHECK(s.pickups.size() == 1);
  CHECK(countEvents(log, EventKind::BananaDropped) == 1);
  CHECK(supplyOf(s) == 3);
}

TEST_CASE("scripted matches never run out of bananas", "[scenario][supply]") {
  const ContentDB& content = defaultContent();
  for (const uint32_t players : {2u, 4u, 10u}) {
    for (uint64_t k = 0; k < 4; ++k) {
      const auto mask = static_cast<PlayerMask>((1u << players) - 1u);
      WorldState s = initialWorld(MatchConfig{0x5B000 + k, mask});
      Rng inputs = Rng::seeded(0x5B100 + k);
      EventSink sink;
      uint32_t playingTicks = 0;
      uint32_t minSupply = UINT32_MAX;
      for (int t = 0; t < 7200 && s.rules.phase != MatchPhase::MatchEnded; ++t) {
        const InputFrame f = scriptedFrame(inputs, s, content);
        sink.clear();
        step(s, f, content, sink);
        REQUIRE(sink.dropped == 0);
        if (s.rules.phase != MatchPhase::Playing || s.players.size() < 2) continue;
        ++playingTicks;
        const uint32_t supply = supplyOf(s);
        minSupply = std::min(minSupply, supply);
        INFO("players " << players << ", match " << k << ", tick " << s.tick.value);
        REQUIRE(supply >= 1);
      }
      CHECK(playingTicks > 0);
      UNSCOPED_INFO("players " << players << " match " << k << ": min supply " << minSupply);
    }
  }
}
