// The recall chain (architecture §5.5, v0.95), through step(): an empty hand
// held on Throw pulls its nearest own banana home, then, the button still
// held, the next nearest, until the hand is full or none is left on the
// floor; letting go drops the one in flight; a hand with a banana does not
// start a pull. The edges: the hand filled from elsewhere mid-chain, a stack,
// the ammo power-up running out, a fall, Fast Hands' throw.
#include "kfm/sim/abilities/throw.hpp"
#include "kfm/sim/step.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kThrow = buttonBit(Button::Throw);
constexpr PlayerId kP0{0};

Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

PlayerInput holding(uint8_t buttons) {
  return PlayerInput{.moveDir = 0, .moveMag = 0, .aimDir = 0, .buttons = buttons};
}

// p0 at the origin with an empty hand and `extra` more banana capacity (a
// power-up lasting `ticks`), and its own bananas lying where it says.
struct ChainLab {
  ContentDB db{defaultContent()};
  WorldState s;
  std::vector<EntityId> lying;

  ChainLab(std::initializer_list<Vec2> bananas, int32_t extra, uint16_t ticks = 6000)
      : s(makePlayingWorld(17, 0b1, db)) {
    if (extra > 0) {
      const uint8_t more = addPowerUp(
          db, "more", 6000, {statGrant(StatKind::AmmoMax, Fixed::one(), Fixed::fromInt(extra))});
      givePowerUp(playerAt(s, kP0), more, ticks);
    }
    placePlayer(s, kP0, Vec2{});
    playerAt(s, kP0).ammo = 0;
    for (const Vec2 pos : bananas) {
      const EntityId id = spawnPickup(s, pos);
      pickupAt(s, id).spawner = bananaLastOwnerByte(kP0);
      lying.push_back(id);
    }
  }
  EventLog step(PlayerInput in) {
    InputFrame f = frameFor(s);
    f.inputs[0] = in;
    EventLog log;
    runTicks(s, f, 1, MatchContent(db), log);
    return log;
  }
  // Holds Throw for up to n ticks (or until `until` returns true after a
  // tick), appending every event to `log`.
  template <class Until>
  void hold(int n, EventLog& log, Until until) {
    for (int k = 0; k < n; ++k) {
      const EventLog one = step(holding(kThrow));
      log.insert(log.end(), one.begin(), one.end());
      if (until()) return;
    }
  }
  void hold(int n, EventLog& log) {
    hold(n, log, [] { return false; });
  }
  [[nodiscard]] Player& p0() { return playerAt(s, kP0); }
  // Bananas anywhere: in the hand, in the air, on the floor.
  [[nodiscard]] int bananas() {
    int n = p0().ammo;
    s.boomerangs.forEachAlive([&](EntityId /*id*/, const Boomerang& /*b*/) { ++n; });
    s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& k) {
      if (k.kind == PickupKind::Banana) n += k.count;
    });
    return n;
  }
  [[nodiscard]] int pulling() const {
    int n = 0;
    s.boomerangs.forEachAlive([&](EntityId /*id*/, const Boomerang& b) {
      if (b.phase == BoomerangPhase::Recalled) ++n;
    });
    return n;
  }
};

// Holds Throw `ticks` ticks and returns the lab's lying bananas (indices into
// `lying`) in the order they were lifted off the floor.
std::vector<int> liftOrder(ChainLab& lab, int ticks) {
  std::vector<int> order;
  for (int k = 0; k < ticks; ++k) {
    std::vector<bool> before(lab.lying.size());
    for (size_t i = 0; i < lab.lying.size(); ++i)
      before[i] = lab.s.pickups.isAlive(lab.lying[i]);
    const EventLog one = lab.step(holding(kThrow));
    CHECK(lab.pulling() <= 1);  // one banana in the air per puller
    CHECK(countEvents(one, EventKind::BoomerangThrown) == 0);
    for (size_t i = 0; i < lab.lying.size(); ++i) {
      if (before[i] && !lab.s.pickups.isAlive(lab.lying[i])) order.push_back(static_cast<int>(i));
    }
  }
  return order;
}

}  // namespace

TEST_CASE("recall chain: three bananas out, held: all three come home, nearest first",
          "[scenario][recall][chain]") {
  // Listed far to near: the pool order is not the pull order.
  ChainLab lab({at(9, 0), at(0, -6), at(-3, 0)}, 2);
  (void)lab.step(PlayerInput{});  // the power-up's stats apply
  REQUIRE(lab.p0().stats.ammoMax == 3);
  REQUIRE(lab.bananas() == 3);
  const std::vector<int> order = liftOrder(lab, 400);
  CHECK(order == std::vector<int>{2, 1, 0});
  CHECK(lab.p0().ammo == 3);
  CHECK(lab.pulling() == 0);
  CHECK(lab.bananas() == 3);
  // Still held with a full hand: nothing more, and no throw.
  EventLog log;
  lab.hold(30, log);
  CHECK(countEvents(log, EventKind::BananaRecallStarted) == 0);
  CHECK(countEvents(log, EventKind::BoomerangThrown) == 0);
  CHECK(lab.p0().abilities[kAbilitySlotThrow].phase == AbilityPhase::Idle);
}

TEST_CASE("recall chain: each pull reaches the hand before the next starts, in one hold",
          "[scenario][recall][chain]") {
  ChainLab lab({at(4, 0), at(-8, 0)}, 1);
  (void)lab.step(PlayerInput{});
  EventLog log;
  lab.hold(400, log, [&] { return lab.p0().ammo == 2; });
  REQUIRE(countEvents(log, EventKind::BananaRecallStarted) == 2);
  CHECK(countEvents(log, EventKind::BananaRecallEnded) == 0);
  // The second starts on the tick the first is caught.
  const SimEvent& caught = firstEvent(log, EventKind::BoomerangCaught);
  int starts = 0;
  for (const SimEvent& e : log) {
    if (e.kind != EventKind::BananaRecallStarted) continue;
    if (++starts == 2) CHECK(e.tick == caught.tick);
  }
}

TEST_CASE("recall chain: letting go mid-chain drops the one in flight and ends it",
          "[scenario][recall][chain]") {
  ChainLab lab({at(3, 0), at(-8, 0), at(0, 8)}, 2);
  (void)lab.step(PlayerInput{});
  EventLog log;
  lab.hold(400, log, [&] { return lab.p0().ammo == 1; });
  REQUIRE(lab.p0().ammo == 1);
  REQUIRE(lab.pulling() == 1);  // the second is on its way
  lab.hold(20, log);
  const EventLog let = lab.step(PlayerInput{});
  CHECK(countEvents(let, EventKind::BananaRecallEnded) == 1);
  CHECK(lab.pulling() == 0);
  CHECK(lab.bananas() == 3);
  // Held again with a banana in the hand: it charges a throw, never pulls.
  EventLog again;
  lab.hold(20, again);
  CHECK(countEvents(again, EventKind::BananaRecallStarted) == 0);
  CHECK(lab.p0().abilities[kAbilitySlotThrow].phase == AbilityPhase::Startup);
}

TEST_CASE("recall chain: a full hand stops it; the rest stay on the floor",
          "[scenario][recall][chain]") {
  ChainLab lab({at(3, 0), at(-5, 0), at(0, 7)}, 1);  // room for two
  (void)lab.step(PlayerInput{});
  REQUIRE(lab.p0().stats.ammoMax == 2);
  EventLog log;
  lab.hold(500, log);
  CHECK(countEvents(log, EventKind::BananaRecallStarted) == 2);
  CHECK(lab.p0().ammo == 2);
  CHECK(lab.pulling() == 0);
  CHECK(lab.s.pickups.isAlive(lab.lying[2]));  // the farthest stays
  CHECK(lab.bananas() == 3);
}

TEST_CASE("recall chain: a stack comes home one banana at a time", "[scenario][recall][chain]") {
  ChainLab lab({at(5, 0)}, 2);
  pickupAt(lab.s, lab.lying[0]).count = 3;
  (void)lab.step(PlayerInput{});
  EventLog log;
  lab.hold(500, log);
  CHECK(countEvents(log, EventKind::BananaRecallStarted) == 3);
  CHECK(lab.p0().ammo == 3);
  CHECK_FALSE(lab.s.pickups.isAlive(lab.lying[0]));
  CHECK(lab.bananas() == 3);
}

TEST_CASE("recall chain: a hand filled from elsewhere mid-pull loses no banana",
          "[scenario][recall][chain]") {
  // Room for one: a pull is on its way when a banana nobody may recall is
  // picked up underfoot. The pull still arrives and drops at the catch point.
  ChainLab lab({at(8, 0), at(-8, 0)}, 0);
  EventLog log;
  lab.hold(20, log);
  REQUIRE(lab.pulling() == 1);
  (void)spawnPickup(lab.s, Vec2{});
  lab.hold(1, log);
  REQUIRE(lab.p0().ammo == 1);
  lab.hold(200, log, [&] { return lab.pulling() == 0; });
  CHECK(countEvents(log, EventKind::BoomerangCaught) == 1);
  CHECK(countEvents(log, EventKind::BananaRecallStarted) == 1);  // no second pull
  CHECK(lab.p0().ammo == 1);
  CHECK(lab.bananas() == 3);
}

TEST_CASE("recall chain: the ammo power-up running out ends it; the pull in flight drops",
          "[scenario][recall][chain]") {
  // The extra capacity lasts until after the first catch.
  ChainLab lab({at(3, 0), at(-9, 0), at(0, 9)}, 2, 70);
  (void)lab.step(PlayerInput{});
  EventLog log;
  lab.hold(400, log, [&] { return lab.p0().ammo == 1; });
  REQUIRE(lab.p0().ammo == 1);
  REQUIRE(lab.pulling() == 1);
  lab.hold(400, log, [&] { return lab.pulling() == 0; });
  CHECK(lab.p0().stats.ammoMax == 1);
  CHECK(lab.p0().ammo == 1);
  // The second came home to a full hand and dropped there; no third pull.
  CHECK(countEvents(log, EventKind::BananaRecallStarted) == 2);
  CHECK(lab.bananas() == 3);
}

TEST_CASE("recall chain: a fall mid-pull drops the banana in flight", "[scenario][recall][chain]") {
  ChainLab lab({at(3, 0), at(-9, 0)}, 1);
  (void)lab.step(PlayerInput{});
  EventLog log;
  lab.hold(400, log, [&] { return lab.p0().ammo == 1; });
  REQUIRE(lab.pulling() == 1);
  Player& p = lab.p0();
  p.tags.set(Tag::Falling);
  p.fallTicks = 30;
  EventLog after;
  lab.hold(5, after);
  CHECK(lab.pulling() == 0);
  CHECK(countEvents(after, EventKind::BananaRecallStarted) == 0);
  int lying = 0;
  lab.s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& /*k*/) { ++lying; });
  CHECK(lying == 1);  // the second, dropped where it was; the hand's goes down with p0
}

TEST_CASE("recall chain: Fast Hands' throw chains too", "[scenario][recall][chain]") {
  ChainLab lab({at(3, 0), at(-6, 0)}, 1);
  // A fast throw def (the base throw's recall, a shorter cooldown) in the
  // throw slot, as the fast_hands power-up grants it.
  AbilityDef fast = lab.db.abilityForSlot(kAbilitySlotThrow);
  fast.name = ContentName::of("throw_fast");
  fast.cooldownTicks = 8;
  REQUIRE(lab.db.abilityCount < kMaxAbilityDefs);
  const auto index = lab.db.abilityCount++;
  lab.db.abilities[index] = fast;
  const uint8_t hands = addPowerUp(lab.db, "hands", 6000, {abilityGrant(kAbilitySlotThrow, index)});
  givePowerUp(lab.p0(), hands, 6000);
  (void)lab.step(PlayerInput{});
  REQUIRE(lab.p0().abilities[kAbilitySlotThrow].defIndex == index);
  EventLog log;
  lab.hold(400, log);
  CHECK(countEvents(log, EventKind::BananaRecallStarted) == 2);
  CHECK(lab.p0().ammo == 2);
}
