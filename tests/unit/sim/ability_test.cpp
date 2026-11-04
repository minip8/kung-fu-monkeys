// AbilitySystem + Dash/Throw behaviours (contract step 3, architecture §5.5).
// Tests drive inputSystem + abilitySystem directly and advance the tick by
// hand, so nothing downstream (movement, projectiles, collisions) interferes.
#include "kfm/sim/abilities/registry.hpp"
#include "kfm/sim/systems.hpp"
#include "sim_test_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kThrow = buttonBit(Button::Throw);
constexpr uint8_t kDash = buttonBit(Button::Dash);
const PlayerId kP0{0};

// One ability tick: decode the frame, run the ability system, advance the tick.
void abilityTick(WorldState& s, InputFrame f, const ContentDB& content, EventSink& sink) {
  f.tick = s.tick;
  sink.clear();
  inputSystem(s, content, f);
  abilitySystem(s, content, sink);
  ++s.tick;
}

InputFrame withButtons(const WorldState& s, uint8_t buttons, uint8_t aimDir = 0,
                       uint8_t moveDir = 0, uint8_t subTick = 0) {
  InputFrame f = frameFor(s);
  f.inputs[0] =
      PlayerInput{moveDir, static_cast<uint8_t>(moveDir != 0 ? 255 : 0), aimDir, buttons, subTick};
  return f;
}

const AbilityState& slot(const WorldState& s, uint8_t i) {
  return playerAt(s, kP0).abilities[i];
}

bool near(Fixed a, Fixed b, int32_t rawTol = 4) {
  return std::abs(a.raw - b.raw) <= rawTol;
}

}  // namespace

TEST_CASE("registry maps kinds to behaviours", "[sim][ability]") {
  STATIC_REQUIRE(kAbilitySlots == 3);
  CHECK(behaviorFor(AbilityKind::Dash).canActivate != nullptr);
  CHECK(behaviorFor(AbilityKind::Throw).onPhaseChange != nullptr);
  // Out-of-range kinds get the inert None behaviour, which never activates.
  const WorldState s = makePlayingWorld(1, 0b1);
  const Player& p = playerAt(s, kP0);
  CHECK_FALSE(behaviorFor(AbilityKind::kCount).canActivate(s, p, AbilityDef{}));
  CHECK_FALSE(behaviorFor(AbilityKind::None).canActivate(s, p, AbilityDef{}));
}

TEST_CASE("dash timeline: phases, PerfectDash and cooldown land on exact ticks", "[sim][ability]") {
  const ContentDB& content = defaultContent();
  const AbilityDef& dash = content.abilityForSlot(kAbilitySlotDash);
  REQUIRE(dash.startupTicks == 0);
  WorldState s = makePlayingWorld(2, 0b1);
  EventSink sink;

  const Tick t0 = s.tick;
  abilityTick(s, withButtons(s, kDash, 0, 0, 77), content, sink);  // press on tick T
  // Startup 0 is skipped: the press tick is the first Active tick.
  CHECK(slot(s, 0).phase == AbilityPhase::Active);
  CHECK(slot(s, 0).phaseTicks == 0);
  CHECK(slot(s, 0).activatedTick == t0);
  CHECK(slot(s, 0).activatedSubTick == 77);
  CHECK(slot(s, 0).cooldownTicks == dash.cooldownTicks);
  CHECK(countEvents(sink, EventKind::PlayerDashed) == 1);

  // Tick-by-tick: offset k = tick - T, for k in [0, A + R].
  const int a = dash.activeTicks;
  const int r = dash.recoveryTicks;
  const int perfect = dash.perfectWindowTicks;
  for (int k = 0; k <= a + r; ++k) {
    if (k > 0) abilityTick(s, withButtons(s, 0), content, sink);  // button released
    INFO("k = " << k);
    const Player& p = playerAt(s, kP0);
    AbilityPhase want = AbilityPhase::Idle;
    if (k < a) {
      want = AbilityPhase::Active;
    } else if (k < a + r) {
      want = AbilityPhase::Recovery;
    }
    CHECK(slot(s, 0).phase == want);
    CHECK(p.tags.has(Tag::Dashing) == (k < a));
    // Pressed at sub-tick 77: the window closes 77/256 into tick T + perfect,
    // so PerfectDash covers that tick too (the catch rule judges the instant).
    CHECK(p.tags.has(Tag::PerfectDash) == (k <= perfect));
    CHECK(slot(s, 0).cooldownTicks == dash.cooldownTicks - k);
  }

  // Cooldown blocks a new press until exactly T + cooldown.
  while (s.tick.minus(t0) < dash.cooldownTicks - 1) {
    abilityTick(s, withButtons(s, 0), content, sink);
  }
  abilityTick(s, withButtons(s, kDash), content, sink);  // tick T + C - 1: still cooling down
  CHECK(slot(s, 0).phase == AbilityPhase::Idle);
  CHECK(countEvents(sink, EventKind::PlayerDashed) == 0);
  abilityTick(s, withButtons(s, 0), content, sink);
  REQUIRE(s.tick.minus(t0) == dash.cooldownTicks + 1);
  // Release happened on T+C; press again on T+C+1 works.
  abilityTick(s, withButtons(s, kDash), content, sink);
  CHECK(slot(s, 0).phase == AbilityPhase::Active);
  CHECK(countEvents(sink, EventKind::PlayerDashed) == 1);
}

TEST_CASE("cooldown expires exactly cooldownTicks after activation", "[sim][ability]") {
  const ContentDB& content = defaultContent();
  const AbilityDef& dash = content.abilityForSlot(kAbilitySlotDash);
  WorldState s = makePlayingWorld(3, 0b1);
  EventSink sink;
  const Tick t0 = s.tick;
  abilityTick(s, withButtons(s, kDash), content, sink);
  while (s.tick.minus(t0) < dash.cooldownTicks) {
    abilityTick(s, withButtons(s, 0), content, sink);
  }
  REQUIRE(slot(s, 0).cooldownTicks == 1);                // decremented to 0 at the start of T + C
  abilityTick(s, withButtons(s, kDash), content, sink);  // press on T + C
  CHECK(countEvents(sink, EventKind::PlayerDashed) == 1);
  CHECK(slot(s, 0).activatedTick == t0.plus(dash.cooldownTicks));
}

TEST_CASE("held button does not re-fire", "[sim][ability]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(4, 0b1);
  EventSink sink;
  int dashes = 0;
  for (int k = 0; k < 200; ++k) {
    abilityTick(s, withButtons(s, kDash), content, sink);
    dashes += countEvents(sink, EventKind::PlayerDashed);
  }
  CHECK(dashes == 1);
}

TEST_CASE("abilities do not activate outside Playing", "[sim][ability]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(5, 0b1);
  s.rules.phase = MatchPhase::RoundStarting;
  EventSink sink;
  abilityTick(s, withButtons(s, kDash | kThrow), content, sink);
  CHECK(slot(s, 0).phase == AbilityPhase::Idle);
  CHECK(slot(s, 1).phase == AbilityPhase::Idle);
  CHECK(slot(s, 0).cooldownTicks == 0);
  CHECK(sink.count == 0);
}

TEST_CASE("zero-length phases are skipped in the same tick", "[sim][ability]") {
  ContentDB content = defaultContent();
  AbilityDef& thr = content.abilities[content.player.abilities[kAbilitySlotThrow]];
  thr.startupTicks = 0;
  thr.activeTicks = 0;
  thr.recoveryTicks = 0;
  thr.cooldownTicks = 0;
  WorldState s = makePlayingWorld(6, 0b1);
  EventSink sink;

  // Every phase is zero: the press spawns the banana and returns to Idle at once.
  abilityTick(s, withButtons(s, kThrow), content, sink);
  CHECK(slot(s, 1).phase == AbilityPhase::Idle);
  CHECK(countEvents(sink, EventKind::BoomerangThrown) == 1);
  CHECK(s.boomerangs.size() == 1);

  // Startup 0 alone: banana on the press tick, then Active for its duration.
  thr.activeTicks = 3;
  playerAt(s, kP0).ammo = 1;
  abilityTick(s, withButtons(s, 0), content, sink);
  abilityTick(s, withButtons(s, kThrow), content, sink);
  CHECK(slot(s, 1).phase == AbilityPhase::Active);
  CHECK(countEvents(sink, EventKind::BoomerangThrown) == 1);
}

TEST_CASE("dash direction comes from move, or facing when idle", "[sim][ability]") {
  const ContentDB& content = defaultContent();
  const AbilityDef& dash = content.abilityForSlot(kAbilitySlotDash);
  EventSink sink;

  SECTION("move direction wins over aim") {
    WorldState s = makePlayingWorld(7, 0b1);
    // Half-magnitude move toward +y (64), aim toward -x (128).
    InputFrame f = withButtons(s, kDash, 128, 64);
    f.inputs[0].moveMag = 128;
    abilityTick(s, f, content, sink);
    const Player& p = playerAt(s, kP0);
    CHECK(near(slot(s, 0).dir.x, Fixed::zero()));
    CHECK(near(slot(s, 0).dir.y, Fixed::one(), 64));  // normalised, not half length
    CHECK(near(length(p.transform.vel), dash.speed, 64));
    CHECK(p.transform.vel.y > Fixed::zero());
  }
  SECTION("idle move dashes along facing") {
    WorldState s = makePlayingWorld(8, 0b1);
    abilityTick(s, withButtons(s, kDash, 128), content, sink);  // aim -x
    const Player& p = playerAt(s, kP0);
    CHECK(near(slot(s, 0).dir.x, -Fixed::one()));
    CHECK(near(slot(s, 0).dir.y, Fixed::zero()));
    CHECK(near(p.transform.vel.x, -dash.speed));
    const SimEvent* e = findEvent(sink, EventKind::PlayerDashed);
    REQUIRE(e != nullptr);
    CHECK(e->player == kP0);
    CHECK(e->entity == s.players.idAt(0));
  }
}

TEST_CASE("dash velocity is pinned during Active and the slot leaves it after", "[sim][ability]") {
  const ContentDB& content = defaultContent();
  const AbilityDef& dash = content.abilityForSlot(kAbilitySlotDash);
  WorldState s = makePlayingWorld(9, 0b1);
  EventSink sink;
  abilityTick(s, withButtons(s, kDash, 0), content, sink);  // aim +x
  for (int k = 1; k < int{dash.activeTicks}; ++k) {
    playerAt(s, kP0).transform.vel = Vec2::zero();  // anything else touching vel is overridden
    abilityTick(s, withButtons(s, 0, 0), content, sink);
    CHECK(playerAt(s, kP0).transform.vel.x == dash.speed);
  }
  playerAt(s, kP0).transform.vel = Vec2::zero();
  abilityTick(s, withButtons(s, 0, 0), content, sink);  // first Recovery tick
  CHECK(slot(s, 0).phase == AbilityPhase::Recovery);
  CHECK(playerAt(s, kP0).transform.vel == Vec2::zero());
}

TEST_CASE("a press on the tick boundary keeps PerfectDash for exactly the window",
          "[sim][ability]") {
  const ContentDB& content = defaultContent();
  const int perfect = content.abilityForSlot(kAbilitySlotDash).perfectWindowTicks;
  WorldState s = makePlayingWorld(2, 0b1);
  EventSink sink;
  abilityTick(s, withButtons(s, kDash, 0, 0, 0), content, sink);  // sub-tick 0
  for (int k = 0; k <= perfect + 1; ++k) {
    if (k > 0) abilityTick(s, withButtons(s, 0), content, sink);
    INFO("k = " << k);
    CHECK(playerAt(s, kP0).tags.has(Tag::PerfectDash) == (k < perfect));
  }
}

TEST_CASE("tag rebuild leaves HitStun and Eliminated alone", "[sim][ability]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(10, 0b1);
  EventSink sink;
  Player& p = playerAt(s, kP0);
  p.tags.set(Tag::HitStun);
  p.tags.set(Tag::Eliminated);
  p.tags.set(Tag::Dashing);  // stale: no dash is running
  abilityTick(s, withButtons(s, 0), content, sink);
  CHECK(p.tags.has(Tag::HitStun));
  CHECK(p.tags.has(Tag::Eliminated));
  CHECK_FALSE(p.tags.has(Tag::Dashing));
  CHECK_FALSE(p.tags.has(Tag::PerfectDash));
}

TEST_CASE("throw timeline: banana spawns on the Startup -> Active tick", "[sim][ability]") {
  const ContentDB& content = defaultContent();
  const AbilityDef& thr = content.abilityForSlot(kAbilitySlotThrow);
  REQUIRE(thr.startupTicks == 2);
  WorldState s = makePlayingWorld(11, 0b1);
  const Vec2 origin{Fixed::fromInt(1), Fixed::fromInt(-2)};
  placePlayer(s, kP0, origin).ammo = 1;
  EventSink sink;

  // Press on T aiming +y (64). Ticks T and T+1 are Startup; on T+2 the banana
  // spawns. T+3 begins Recovery (active 1), Idle from T+9.
  const Tick t0 = s.tick;
  abilityTick(s, withButtons(s, kThrow, 64), content, sink);
  CHECK(slot(s, 1).phase == AbilityPhase::Startup);
  CHECK(sink.count == 0);
  CHECK(playerAt(s, kP0).ammo == 1);
  // Aim changes during startup do not change the throw direction.
  abilityTick(s, withButtons(s, 0, 0), content, sink);  // T+1
  CHECK(slot(s, 1).phase == AbilityPhase::Startup);
  CHECK(s.boomerangs.empty());
  abilityTick(s, withButtons(s, 0, 0), content, sink);  // T+2
  CHECK(slot(s, 1).phase == AbilityPhase::Active);
  REQUIRE(s.boomerangs.size() == 1);
  CHECK(playerAt(s, kP0).ammo == 0);

  const SimEvent* e = findEvent(sink, EventKind::BoomerangThrown);
  REQUIRE(e != nullptr);
  CHECK(e->tick == t0.plus(2));
  CHECK(e->player == kP0);
  const Boomerang* b = s.boomerangs.get(e->entity);
  REQUIRE(b != nullptr);
  CHECK(b->owner == kP0);
  CHECK(b->phase == BoomerangPhase::Outbound);
  CHECK(b->damage == content.boomerang.damage);
  CHECK(b->ticksAlive == 0);
  CHECK(near(b->transform.pos.x, origin.x));
  CHECK(near(b->transform.pos.y, origin.y + content.boomerang.spawnOffset));
  CHECK(b->transform.prevPos == b->transform.pos);
  CHECK(near(b->transform.vel.x, Fixed::zero()));
  CHECK(near(b->transform.vel.y, content.boomerang.speed));
  CHECK(std::abs(b->transform.facing.signedDeltaFrom(Angle::fromInput8(64))) <= 4);

  abilityTick(s, withButtons(s, 0), content, sink);  // T+3
  CHECK(slot(s, 1).phase == AbilityPhase::Recovery);
  for (int k = 4; k < 9; ++k)
    abilityTick(s, withButtons(s, 0), content, sink);
  CHECK(slot(s, 1).phase == AbilityPhase::Recovery);
  abilityTick(s, withButtons(s, 0), content, sink);  // T+9
  CHECK(slot(s, 1).phase == AbilityPhase::Idle);
  CHECK(s.boomerangs.size() == 1);  // exactly one banana per throw
}

TEST_CASE("throw heading matches aim in every direction", "[sim][ability]") {
  ContentDB content = defaultContent();
  content.abilities[content.player.abilities[kAbilitySlotThrow]].startupTicks = 0;
  for (int aim = 0; aim < 256; aim += 7) {
    WorldState s = makePlayingWorld(12, 0b1);
    playerAt(s, kP0).ammo = 1;
    EventSink sink;
    abilityTick(s, withButtons(s, kThrow, static_cast<uint8_t>(aim)), content, sink);
    const SimEvent* e = findEvent(sink, EventKind::BoomerangThrown);
    REQUIRE(e != nullptr);
    const Boomerang& b = *s.boomerangs.get(e->entity);
    INFO("aim = " << aim);
    CHECK(std::abs(b.transform.facing.signedDeltaFrom(
              Angle::fromInput8(static_cast<uint8_t>(aim)))) <= 4);
  }
}

TEST_CASE("throw with no ammo does nothing", "[sim][ability]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(13, 0b1);
  playerAt(s, kP0).ammo = 0;
  EventSink sink;
  for (int k = 0; k < 5; ++k)
    abilityTick(s, withButtons(s, k == 0 ? kThrow : 0), content, sink);
  CHECK(slot(s, 1).phase == AbilityPhase::Idle);
  CHECK(slot(s, 1).cooldownTicks == 0);  // a refused press does not start the cooldown
  CHECK(s.boomerangs.empty());
}

TEST_CASE("throw into a full boomerang pool spends no ammo", "[sim][ability]") {
  const ContentDB& content = defaultContent();
  WorldState s = makePlayingWorld(14, 0b1);
  while (!s.boomerangs.full()) {
    spawnBanana(s, kP0, Vec2::zero(), Vec2::zero());
  }
  playerAt(s, kP0).ammo = 1;
  EventSink sink;
  int thrown = 0;
  for (int k = 0; k < 4; ++k) {
    abilityTick(s, withButtons(s, k == 0 ? kThrow : 0), content, sink);
    thrown += countEvents(sink, EventKind::BoomerangThrown);
  }
  CHECK(thrown == 0);
  CHECK(playerAt(s, kP0).ammo == 1);
  CHECK(s.boomerangs.size() == kMaxBoomerangs);
}

TEST_CASE("dash and throw run independently in the same tick", "[sim][ability]") {
  ContentDB content = defaultContent();
  content.abilities[content.player.abilities[kAbilitySlotThrow]].startupTicks = 0;
  WorldState s = makePlayingWorld(15, 0b1);
  EventSink sink;
  abilityTick(s, withButtons(s, kDash | kThrow, 0), content, sink);
  CHECK(slot(s, 0).phase == AbilityPhase::Active);
  CHECK(slot(s, 1).phase == AbilityPhase::Active);
  // Slot order: the dash event precedes the throw event.
  REQUIRE(sink.count == 2);
  CHECK(sink.events[0].kind == EventKind::PlayerDashed);
  CHECK(sink.events[1].kind == EventKind::BoomerangThrown);
}

TEST_CASE("ability ticks are deterministic", "[sim][ability]") {
  const ContentDB& content = defaultContent();
  WorldState a = makePlayingWorld(16, 0b11);
  WorldState b = makePlayingWorld(16, 0b11);
  EventSink sink;
  for (int k = 0; k < 120; ++k) {
    InputFrame f = frameFor(a);
    f.inputs[0] = PlayerInput{static_cast<uint8_t>(k * 13), 255, static_cast<uint8_t>(k * 5),
                              static_cast<uint8_t>(k % 3), static_cast<uint8_t>(k)};
    f.inputs[1] = PlayerInput{0, 0, static_cast<uint8_t>(k * 3), static_cast<uint8_t>(k % 4), 0};
    abilityTick(a, f, content, sink);
    abilityTick(b, f, content, sink);
    REQUIRE(checksum(a) == checksum(b));
  }
}
