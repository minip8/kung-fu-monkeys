// Call end to end (architecture §5.5, §8, v0.35): the ability button shouts
// the press tick's aim byte. The sim emits MonkeyCalled{player, a = symbol,
// b = range} on the press and changes nothing else in the world: who hears
// it is the bot hosts' business (ai::AgentHost).
#include "kfm/sim/systems.hpp"
#include "loadout_util.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

const PlayerId kP0{0};
const PlayerId kP1{1};
constexpr uint8_t kAbility = buttonBit(Button::Ability1);

Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

}  // namespace

TEST_CASE("call: the press shouts the aim byte and nothing else moves", "[scenario][call]") {
  const ContentDB c = withExtraSlot(callDef());
  WorldState s = makePlayingWorld(21, 0b11, c);
  placePlayer(s, kP0, at(-3, 0));
  placePlayer(s, kP1, at(3, 0));
  EventLog log;
  idle(s, 2, c, log);
  const Player before = playerAt(s, kP0);
  const Player other = playerAt(s, kP1);
  const Tick pressTick = s.tick;
  pressOnce(s, kP0, kAbility, c, log, /*aim=*/77, /*subTick=*/40);

  REQUIRE(countEvents(log, EventKind::MonkeyCalled) == 1);
  const SimEvent& e = firstEvent(log, EventKind::MonkeyCalled);
  CHECK(e.player == kP0);
  CHECK(e.tick == pressTick);
  CHECK(e.entity == s.players.idAt(kP0.value));
  CHECK(e.a == 77);
  CHECK(e.b == static_cast<uint32_t>(Fixed::fromInt(4).raw));

  // Only the slot's own state moved: the cooldown and the press stamp.
  const Player& after = playerAt(s, kP0);
  CHECK(after.transform.pos == before.transform.pos);
  CHECK(after.hp == before.hp);
  CHECK(after.ammo == before.ammo);
  CHECK(after.tags.bits == before.tags.bits);
  CHECK(after.abilities[kAbilitySlotExtra].activatedTick == pressTick);
  CHECK(after.abilities[kAbilitySlotExtra].activatedSubTick == 40);
  CHECK(playerAt(s, kP1).transform.pos == other.transform.pos);
  CHECK(s.boomerangs.empty());
}

TEST_CASE("call: the cooldown spaces the shouts and a held button calls once", "[scenario][call]") {
  const ContentDB c = withExtraSlot(callDef());
  WorldState s = makePlayingWorld(22, 0b11, c);
  EventLog log;
  pressOnce(s, kP0, kAbility, c, log, 10);
  // Held for 20 ticks: one call (a press edge fires, a held button never).
  for (int i = 0; i < 20; ++i)
    pressOnce(s, kP0, kAbility, c, log, 11);
  CHECK(countEvents(log, EventKind::MonkeyCalled) == 1);
  // Released, then pressed inside the 15-tick cooldown: nothing.
  EventLog second;
  WorldState t = makePlayingWorld(22, 0b11, c);
  pressOnce(t, kP0, kAbility, c, second, 10);
  idle(t, 5, c, second);
  pressOnce(t, kP0, kAbility, c, second, 12);
  CHECK(countEvents(second, EventKind::MonkeyCalled) == 1);
  // After it: a second call with its own symbol.
  idle(t, 10, c, second);
  pressOnce(t, kP0, kAbility, c, second, 13);
  REQUIRE(countEvents(second, EventKind::MonkeyCalled) == 2);
  uint32_t lastSymbol = 0;
  for (const SimEvent& e : second) {
    if (e.kind == EventKind::MonkeyCalled) lastSymbol = e.a;
  }
  CHECK(lastSymbol == 13);
}

TEST_CASE("call: nobody calls before the round is playing", "[scenario][call]") {
  const ContentDB c = withExtraSlot(callDef());
  WorldState s = initialWorld(MatchConfig{23, 0b11});
  EventLog log;
  for (int i = 0; i < 30 && s.rules.phase != MatchPhase::Playing; ++i) {
    InputFrame f = frameFor(s);
    f.inputs[0].buttons = (i % 2 == 0) ? kAbility : uint8_t{0};
    runTicks(s, f, 1, c, log);
  }
  CHECK(countEvents(log, EventKind::MonkeyCalled) == 0);
}
