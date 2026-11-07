// End-to-end dash attack through step() (architecture §5.6, §9): dashing into
// an enemy while holding a banana damages it. Exercises Dash (abilitySystem,
// which grants Tag::Dashing), Player × Player swept collision and the
// row-25 rule, including both orientations and the tie-break.
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kAimLeft = 128;  // half a turn: toward -x
constexpr uint8_t kAimRight = 0;
const PlayerId kP0{0};
const PlayerId kP1{1};

Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

struct Press {
  int tick{-1};  // relative tick of the Dash press; -1 = never
  uint8_t subTick{0};
};

// Player 0 at (-2, 0) faces +x, player 1 at (2, 0) faces -x; both sticks
// idle, so a dash goes along the facing, straight at the other. A third
// player stands far away so an elimination does not end the round.
struct Duel {
  WorldState s;
  EventLog log;
};

Duel dashDuel(Press p0, Press p1, int ticks = 30, const ContentDB& content = defaultContent()) {
  Duel d{makePlayingWorld(61, 0b111, content), {}};
  placePlayer(d.s, kP0, at(-2, 0));
  placePlayer(d.s, kP1, at(2, 0));
  placePlayer(d.s, PlayerId{2}, at(-12, 7));
  for (int k = 0; k < ticks; ++k) {
    InputFrame f = frameFor(d.s);
    f.inputs[0].aimDir = kAimRight;
    f.inputs[1].aimDir = kAimLeft;
    if (k == p0.tick) {
      f.inputs[0].buttons = buttonBit(Button::Dash);
      f.inputs[0].subTick = p0.subTick;
    }
    if (k == p1.tick) {
      f.inputs[1].buttons = buttonBit(Button::Dash);
      f.inputs[1].subTick = p1.subTick;
    }
    runTicks(d.s, f, 1, content, d.log);
  }
  return d;
}

}  // namespace

TEST_CASE("dashing into an enemy with a banana in hand eliminates it", "[scenario][dash_attack]") {
  const ContentDB& content = defaultContent();
  REQUIRE(content.dashAttack.minAmmo == 1);
  Duel d = dashDuel(Press{0, 0}, Press{});
  REQUIRE(countEvents(d.log, EventKind::PlayerDashed) == 1);
  const SimEvent& hit = firstEvent(d.log, EventKind::PlayerDashHit);
  CHECK(hit.player == kP1);
  CHECK(hit.a == kP0.value);
  CHECK(hit.b == 0);
  CHECK(countEvents(d.log, EventKind::PlayerHit) == 0);  // not a banana hit
  const SimEvent& eliminated = firstEvent(d.log, EventKind::PlayerEliminated);
  CHECK(eliminated.player == kP1);
  CHECK(eliminated.a == kP0.value);
  CHECK(eliminated.tick == hit.tick);
  // The hit lands while the dash is active (12 ticks from the press).
  const SimEvent& dashed = firstEvent(d.log, EventKind::PlayerDashed);
  CHECK(hit.tick.minus(dashed.tick) < content.abilityForSlot(kAbilitySlotDash).activeTicks);

  CHECK(d.s.playerBySlot(kP1) == nullptr);
  CHECK(playerAt(d.s, kP0).ammo == 1);  // the attacker keeps its banana
  // v0.57: the victim's held banana drops where it died.
  CHECK(countEvents(d.log, EventKind::BananaDropped) == 1);
  CHECK(firstEvent(d.log, EventKind::BananaDropped).player == kP1);
  CHECK(d.s.rules.phase == MatchPhase::Playing);  // player 2 is still in
}

TEST_CASE("a dash without a banana passes through", "[scenario][dash_attack]") {
  const ContentDB& content = defaultContent();
  Duel d{makePlayingWorld(61, 0b111), {}};
  placePlayer(d.s, kP0, at(-2, 0)).ammo = 0;
  placePlayer(d.s, kP1, at(2, 0));
  placePlayer(d.s, PlayerId{2}, at(-12, 7));
  for (int k = 0; k < 30; ++k) {
    InputFrame f = frameFor(d.s);
    f.inputs[0].aimDir = kAimRight;
    if (k == 0) f.inputs[0].buttons = buttonBit(Button::Dash);
    runTicks(d.s, f, 1, content, d.log);
  }
  CHECK(countEvents(d.log, EventKind::PlayerDashed) == 1);
  CHECK(countEvents(d.log, EventKind::PlayerDashHit) == 0);
  CHECK(countEvents(d.log, EventKind::PlayerEliminated) == 0);
  // It went right through: player 0 ends past player 1.
  CHECK(playerAt(d.s, kP0).transform.pos.x > playerAt(d.s, kP1).transform.pos.x);
}

TEST_CASE("a stunned victim is not hit again", "[scenario][dash_attack]") {
  ContentDB content = defaultContent();
  content.player.maxHp = 3;
  Duel d{makePlayingWorld(61, 0b111, content), {}};
  placePlayer(d.s, kP0, at(-2, 0));
  Player& victim = placePlayer(d.s, kP1, at(2, 0));
  placePlayer(d.s, PlayerId{2}, at(-12, 7));
  victim.tags.set(Tag::HitStun);
  victim.hitStunTicks = 60;  // outlasts the dash
  for (int k = 0; k < 20; ++k) {
    InputFrame f = frameFor(d.s);
    f.inputs[0].aimDir = kAimRight;
    if (k == 0) f.inputs[0].buttons = buttonBit(Button::Dash);
    runTicks(d.s, f, 1, content, d.log);
  }
  CHECK(countEvents(d.log, EventKind::PlayerDashed) == 1);
  CHECK(countEvents(d.log, EventKind::PlayerDashHit) == 0);
  CHECK(playerAt(d.s, kP1).hp == 3);
}

TEST_CASE("two players dashing into each other: exactly one hit, the earlier press wins",
          "[scenario][dash_attack]") {
  SECTION("slot 0 pressed earlier in the tick") {
    const Duel d = dashDuel(Press{0, 10}, Press{0, 200});
    REQUIRE(countEvents(d.log, EventKind::PlayerDashed) == 2);
    REQUIRE(countEvents(d.log, EventKind::PlayerDashHit) == 1);
    CHECK(firstEvent(d.log, EventKind::PlayerDashHit).player == kP1);
    CHECK(countEvents(d.log, EventKind::PlayerEliminated) == 1);
    CHECK(d.s.playerBySlot(kP0) != nullptr);
    CHECK(d.s.playerBySlot(kP1) == nullptr);
  }
  SECTION("slot 1 pressed earlier in the tick: the slot does not decide") {
    const Duel d = dashDuel(Press{0, 200}, Press{0, 10});
    REQUIRE(countEvents(d.log, EventKind::PlayerDashHit) == 1);
    CHECK(firstEvent(d.log, EventKind::PlayerDashHit).player == kP0);
    CHECK(d.s.playerBySlot(kP0) == nullptr);
    CHECK(d.s.playerBySlot(kP1) != nullptr);
  }
  SECTION("same press instant: the lower slot wins") {
    const Duel d = dashDuel(Press{0, 99}, Press{0, 99});
    REQUIRE(countEvents(d.log, EventKind::PlayerDashHit) == 1);
    CHECK(firstEvent(d.log, EventKind::PlayerDashHit).player == kP1);
  }
}

TEST_CASE("walking into a player does nothing", "[scenario][dash_attack]") {
  const ContentDB& content = defaultContent();
  Duel d{makePlayingWorld(61, 0b111), {}};
  placePlayer(d.s, kP0, at(-2, 0));
  placePlayer(d.s, kP1, at(2, 0));
  placePlayer(d.s, PlayerId{2}, at(-12, 7));
  InputFrame f = frameFor(d.s);
  f.inputs[0] = PlayerInput{0, 255, kAimRight, 0, 0};  // walk +x, right through player 1
  runTicks(d.s, f, 60, content, d.log);
  CHECK(countEvents(d.log, EventKind::PlayerDashHit) == 0);
  CHECK(countEvents(d.log, EventKind::PlayerHit) == 0);
  CHECK(countEvents(d.log, EventKind::PlayerEliminated) == 0);
  CHECK(playerAt(d.s, kP0).transform.pos.x > playerAt(d.s, kP1).transform.pos.x);
}

TEST_CASE("consumesAmmo spends the attacker's banana", "[scenario][dash_attack]") {
  ContentDB content = defaultContent();
  content.dashAttack.consumesAmmo = 1;
  const Duel d = dashDuel(Press{0, 0}, Press{}, 30, content);
  REQUIRE(countEvents(d.log, EventKind::PlayerDashHit) == 1);
  // v0.57: the spent banana drops at the contact (as does the victim's held
  // one), it never vanishes; the attacker may pick it straight back up.
  int spent = 0;
  for (const SimEvent& e : d.log) {
    if (e.kind == EventKind::BananaDropped && e.player == kP0) ++spent;
  }
  CHECK(spent == 1);
}

TEST_CASE("rollback across a dash attack re-simulates identically", "[scenario][dash_attack]") {
  // Snapshot by memcpy just before the dashes, run through the mutual dash
  // attack, roll back and re-simulate: checksums and events match tick for
  // tick (§5.1).
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(61, 0b111);
  placePlayer(s, kP0, at(-2, 0));
  placePlayer(s, kP1, at(2, 0));
  placePlayer(s, PlayerId{2}, at(-12, 7));
  const auto frameAt = [](const WorldState& w, int k) {
    InputFrame f = frameFor(w);
    f.inputs[0].aimDir = kAimRight;
    f.inputs[1].aimDir = kAimLeft;
    if (k == 1) {
      f.inputs[0].buttons = buttonBit(Button::Dash);
      f.inputs[0].subTick = 140;
      f.inputs[1].buttons = buttonBit(Button::Dash);
      f.inputs[1].subTick = 30;
    }
    return f;
  };

  WorldState saved{};
  std::memcpy(&saved, &s, sizeof(WorldState));
  constexpr int kTicks = 20;
  std::vector<uint32_t> sums;
  EventLog first;
  for (int k = 0; k < kTicks; ++k) {
    runTicks(s, frameAt(s, k), 1, content, first);
    sums.push_back(checksum(s));
  }
  REQUIRE(countEvents(first, EventKind::PlayerDashHit) == 1);
  CHECK(firstEvent(first, EventKind::PlayerDashHit).player == kP0);  // slot 1 pressed first

  std::memcpy(&s, &saved, sizeof(WorldState));
  EventLog second;
  for (int k = 0; k < kTicks; ++k) {
    runTicks(s, frameAt(s, k), 1, content, second);
    INFO("re-simulated tick " << k);
    REQUIRE(checksum(s) == sums[static_cast<size_t>(k)]);
  }
  REQUIRE(second.size() == first.size());
  for (size_t i = 0; i < first.size(); ++i) {
    CHECK(second[i].kind == first[i].kind);
    CHECK(second[i].tick == first[i].tick);
    CHECK(second[i].player == first[i].player);
    CHECK(second[i].entity == first[i].entity);
  }
}
