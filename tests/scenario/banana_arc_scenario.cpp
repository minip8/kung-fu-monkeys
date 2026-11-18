// The arc flight and the recall (architecture §5.7, v0.89), through step().
// The arc: a throw slows to its apex and speeds home, the charge scales its
// reach and not its time, a banana that misses its moving thrower drops, a
// bounce out costs flight, a wall on the way home drops it. The recall: an
// empty hand held on Throw pulls the own banana off the floor, slow at first
// (harmless) then fast (it hits), into the hand without starting a throw;
// walls block it, letting go drops it, nobody else can pull it. The grid
// modes and the contest keep the classic flight and no recall.
#include "kfm/content/loader.hpp"
#include "kfm/sim/abilities/throw.hpp"
#include "kfm/sim/step.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <utility>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kThrow = buttonBit(Button::Throw);
constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};
constexpr int kTap = 1;    // ticks held: a press
constexpr int kFull = 42;  // the startup and the 40 ticks of a full charge

Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

// One tick with p0's (and p1's) input.
EventLog tick(WorldState& s, const MatchContent& m, PlayerInput p0, PlayerInput p1 = {}) {
  InputFrame f = frameFor(s);
  f.inputs[0] = p0;
  f.inputs[1] = p1;
  EventLog log;
  runTicks(s, f, 1, m, log);
  return log;
}

PlayerInput holding(uint8_t buttons, uint8_t aim = 0) {
  return PlayerInput{.moveDir = 0, .moveMag = 0, .aimDir = aim, .buttons = buttons};
}

PlayerInput walking(uint8_t dir) {
  return PlayerInput{.moveDir = dir, .moveMag = 255, .aimDir = 0, .buttons = 0};
}

struct Flight {
  float reach{0.0f};    // farthest the banana got from the thrower's centre
  int airTicks{-1};     // the throw's tick to the tick it was caught (-1: never)
  int groundedAt{-1};   // ticks after the throw it came down (-1: never)
  int returningAt{-1};  // ticksAlive when it turned for home
};

// p0 at `from`, standing still unless `run` (a moveDir it walks from the
// release on, for `runTicks`), throws east holding the button `held` ticks.
Flight throwEast(const ContentDB& c, int held, Vec2 from = at(-12, 0),
                 std::optional<uint8_t> run = std::nullopt, int runTicks = 1000) {
  const MatchContent m(c);
  WorldState s = makePlayingWorld(7, 0b1, c);
  placePlayer(s, kP0, from);
  playerAt(s, kP0).ammo = 1;
  Flight out;
  std::optional<Tick> thrown;
  EntityId id{};
  for (int k = 0; k < 400; ++k) {
    PlayerInput in = holding(k < held ? kThrow : 0);
    if (run && thrown && s.tick.minus(*thrown) <= runTicks) in = walking(*run);
    const EventLog log = tick(s, m, in);
    if (const SimEvent* e = findEvent(log, EventKind::BoomerangThrown)) {
      thrown = e->tick;
      id = e->entity;
    }
    if (!thrown) continue;
    const int since = static_cast<int>(s.tick.minus(*thrown)) - 1;
    if (const Boomerang* b = s.boomerangs.get(id)) {
      const Vec2 d = b->transform.pos - playerAt(s, kP0).transform.pos;
      out.reach = std::max(out.reach, length(d).toFloat());
      if (b->phase == BoomerangPhase::Returning && out.returningAt < 0) {
        out.returningAt = b->ticksAlive;
      }
    }
    if (findEvent(log, EventKind::BoomerangCaught) != nullptr) {
      out.airTicks = since;
      break;
    }
    if (findEvent(log, EventKind::BoomerangGrounded) != nullptr) {
      out.groundedAt = since;
      break;
    }
  }
  return out;
}

// p0 at `owner` with an empty hand and a banana of `lastOwner` lying at `lying`.
struct RecallLab {
  ContentDB db;
  WorldState s;
  EntityId pickup{};

  RecallLab(ContentDB content, Vec2 owner, Vec2 lying, PlayerId lastOwner = kP0,
            PlayerMask players = 0b11)
      : db(content), s(makePlayingWorld(11, players, db)) {
    placePlayer(s, kP0, owner);
    playerAt(s, kP0).ammo = 0;
    if ((players & 0b10) != 0) placePlayer(s, kP1, at(12, 7));
    pickup = spawnPickup(s, lying);
    s.pickups.get(pickup)->spawner = bananaLastOwnerByte(lastOwner);
  }
  EventLog step(PlayerInput p0, PlayerInput p1 = {}) { return tick(s, MatchContent(db), p0, p1); }
  [[nodiscard]] const Boomerang* pulled() const {
    const Boomerang* found = nullptr;
    s.boomerangs.forEachAlive([&](EntityId /*id*/, const Boomerang& b) {
      if (b.phase == BoomerangPhase::Recalled) found = &b;
    });
    return found;
  }
};

}  // namespace

TEST_CASE("arc: a throw slows to its apex, turns, and is caught in about 1.25 s",
          "[scenario][projectile][arc]") {
  const ContentDB& c = defaultContent();
  REQUIRE(c.modes[0].bananaFlight == BananaFlight::Arc);
  const Flight tap = throwEast(c, kTap);
  const Flight full = throwEast(c, kFull);
  INFO("tap reach " << tap.reach << " air " << tap.airTicks << ", full reach " << full.reach
                    << " air " << full.airTicks);
  // The reach is the classic one (the mean speed out is the classic speed):
  // a tap 0.6 x 9.6 + 1, a full charge 1.5 x 9.6 + 1, within a unit.
  CHECK(tap.reach > 5.5f);
  CHECK(tap.reach < 7.5f);
  CHECK(full.reach > 14.0f);
  CHECK(full.reach < 16.5f);
  // Caught by its thrower standing still, in the air 1.2 to 1.6 s whatever
  // the charge.
  for (const Flight& f : {tap, full}) {
    CHECK(f.airTicks >= 72);
    CHECK(f.airTicks <= 96);
    CHECK(f.returningAt == int{c.boomerang.arcOutboundTicks});
  }
  CHECK(std::abs(tap.airTicks - full.airTicks) <= 6);
}

TEST_CASE("arc: the speed falls evenly to the apex and rises evenly home",
          "[scenario][projectile][arc]") {
  const ContentDB& c = defaultContent();
  const BoomerangDef& d = c.boomerang;
  const MatchContent m(c);
  WorldState s = makePlayingWorld(3, 0b1, c);
  placePlayer(s, kP0, at(-12, 0));
  const EntityId id =
      spawnBanana(s, kP0, at(-11, 0), Vec2{d.arcLaunchSpeed, Fixed{}}, BoomerangPhase::Outbound, c);
  Fixed last = d.arcLaunchSpeed + Fixed::epsilon();
  for (int k = 1; k < int{d.arcOutboundTicks}; ++k) {
    (void)tick(s, m, PlayerInput{});
    const Boomerang* b = s.boomerangs.get(id);
    REQUIRE(b != nullptr);
    REQUIRE(b->phase == BoomerangPhase::Outbound);
    const Fixed speed = length(b->transform.vel);
    CHECK(speed < last);
    CHECK(speed >= d.arcApexSpeed - Fixed::fromRaw(8));
    last = speed;
  }
  (void)tick(s, m, PlayerInput{});  // the apex: it turns to face its thrower
  const Boomerang* apex = s.boomerangs.get(id);
  REQUIRE(apex != nullptr);
  CHECK(apex->phase == BoomerangPhase::Returning);
  CHECK(apex->transform.vel.x < Fixed{});
  last = Fixed{};
  for (int k = 1; k <= d.arcReturnAccelTicks + 4; ++k) {
    (void)tick(s, m, PlayerInput{});
    const Boomerang* b = s.boomerangs.get(id);
    if (b == nullptr) break;  // home already
    const Fixed speed = length(b->transform.vel);
    CHECK(speed >= last);
    CHECK(speed <= d.arcReturnSpeed + Fixed::fromRaw(8));
    last = speed;
  }
}

TEST_CASE("arc: a thrower who runs off is missed, and the banana drops within its budget",
          "[scenario][projectile][arc]") {
  const ContentDB& c = defaultContent();
  const BoomerangDef& d = c.boomerang;
  // North from the south side, south from the north side: room to run.
  for (const uint8_t dir : {uint8_t{64}, uint8_t{192}}) {
    INFO("run " << int{dir});
    const Flight f = throwEast(c, kFull, at(-12, dir == 64 ? -7 : 7), dir);
    CHECK(f.airTicks < 0);  // never caught
    REQUIRE(f.groundedAt >= 0);
    CHECK(f.groundedAt <= d.arcOutboundTicks + d.arcReturnTicks + 1);
    CHECK(f.groundedAt <= 150);  // never much beyond 2.5 s
    // A step of a fifth of a second aside is still caught: the steering
    // makes up for a step, not for a run.
    const Flight step = throwEast(c, kFull, at(-12, dir == 64 ? -7 : 7), dir, 12);
    CHECK(step.airTicks > 0);
  }
}

TEST_CASE("arc: a missed banana lies there as its thrower's to recall",
          "[scenario][projectile][arc][recall]") {
  const ContentDB& c = defaultContent();
  const MatchContent m(c);
  WorldState s = makePlayingWorld(5, 0b1, c);
  placePlayer(s, kP0, at(-12, -7));
  bool grounded = false;
  for (int k = 0; k < 300 && !grounded; ++k) {
    const PlayerInput in = k < kFull ? holding(kThrow) : walking(64);
    grounded = findEvent(tick(s, m, in), EventKind::BoomerangGrounded) != nullptr;
  }
  REQUIRE(grounded);
  int lying = 0;
  s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& k) {
    CHECK(bananaLastOwner(k) == kP0);
    ++lying;
  });
  CHECK(lying == 1);
}

TEST_CASE("arc: a bounce on the way out costs flight; a wall on the way home drops it",
          "[scenario][projectile][arc]") {
  const ContentDB& c = defaultContent();
  const BoomerangDef& d = c.boomerang;
  // A tap from 13 units east of the centre reaches the outer wall at 16.
  const Flight open = throwEast(c, kTap);
  const Flight bounced = throwEast(c, kTap, at(12, 0));
  REQUIRE(open.returningAt > 0);
  REQUIRE(bounced.returningAt > 0);
  // The bounce aged the flight by arc_bounce_ticks: it turned that much sooner.
  CHECK(bounced.returningAt == open.returningAt - int{d.arcBounceTicks});

  // A returning banana that meets a wall drops at it.
  ContentDB walled = c;
  walled.arenas[0].walls[0] = Wall{at(2, -3), at(3, 3)};
  walled.arenas[0].wallCount = 1;
  const MatchContent m(walled);
  WorldState s = makePlayingWorld(9, 0b1, walled);
  placePlayer(s, kP0, at(-2, 0));
  Boomerang& b = s.boomerangs.items[spawnBanana(s, kP0, at(8, 0), Vec2{-Fixed::half(), Fixed{}},
                                                BoomerangPhase::Returning, walled)
                                        .index];
  b.ticksInPhase = 4;
  b.transform.facing = Angle::fromDegrees(180);
  EventLog log;
  for (int k = 0; k < 40 && findEvent(log, EventKind::BoomerangGrounded) == nullptr; ++k) {
    const EventLog one = tick(s, m, PlayerInput{});
    log.insert(log.end(), one.begin(), one.end());
  }
  CHECK(countEvents(log, EventKind::BoomerangBounced) == 1);
  CHECK(countEvents(log, EventKind::BoomerangCaught) == 0);
  REQUIRE(countEvents(log, EventKind::BoomerangGrounded) == 1);
  s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& k) {
    CHECK(k.pos.x >= Fixed::fromInt(3));  // on the far side, against the wall
    CHECK(k.pos.x < Fixed::fromInt(4));
  });
}

TEST_CASE("recall: an empty hand held on Throw pulls the own banana home, into the hand",
          "[scenario][recall]") {
  RecallLab lab(defaultContent(), at(-8, 0), at(0, 0), kP0, 0b1);
  const AbilityDef& thr = lab.db.abilityForSlot(kAbilitySlotThrow);
  REQUIRE(throwRecalls(thr));
  EventLog log = lab.step(holding(kThrow));
  REQUIRE(countEvents(log, EventKind::BananaRecallStarted) == 1);
  CHECK(!lab.s.pickups.isAlive(lab.pickup));
  REQUIRE(lab.pulled() != nullptr);
  CHECK(lab.pulled()->owner == kP0);
  // From rest, speeding up evenly to recall_speed, never past it, closing in.
  Fixed lastSpeed{};
  Fixed lastDistance = Fixed::fromInt(100);
  int caughtAt = -1;
  for (int k = 1; k < 200; ++k) {
    log = lab.step(holding(kThrow));
    CHECK(countEvents(log, EventKind::BoomerangThrown) == 0);
    if (findEvent(log, EventKind::BoomerangCaught) != nullptr) {
      caughtAt = k;
      break;
    }
    const Boomerang* b = lab.pulled();
    REQUIRE(b != nullptr);
    const Fixed speed = length(b->transform.vel);
    CHECK(speed >= lastSpeed);
    CHECK(speed <= thr.recallSpeed + Fixed::fromRaw(8));
    const Fixed distance = length(playerAt(lab.s, kP0).transform.pos - b->transform.pos);
    CHECK(distance < lastDistance);
    lastSpeed = speed;
    lastDistance = distance;
  }
  // 8 units, less the 0.85 of contact: 4.5 while speeding up, then 12 a second.
  INFO("caught after " << caughtAt << " ticks");
  CHECK(caughtAt > thr.recallAccelTicks);
  CHECK(caughtAt < 75);
  CHECK(lastSpeed == thr.recallSpeed);
  CHECK(playerAt(lab.s, kP0).ammo == 1);
  // Still held: no throw starts until the button is pressed again.
  for (int k = 0; k < 30; ++k) {
    CHECK(countEvents(lab.step(holding(kThrow)), EventKind::BoomerangThrown) == 0);
    CHECK(playerAt(lab.s, kP0).abilities[kAbilitySlotThrow].phase == AbilityPhase::Idle);
  }
  (void)lab.step(holding(0));
  log = lab.step(holding(kThrow));
  for (int k = 0; k < 4; ++k) {
    const EventLog one = lab.step(holding(0));
    log.insert(log.end(), one.begin(), one.end());
  }
  CHECK(countEvents(log, EventKind::BoomerangThrown) == 1);
}

TEST_CASE("recall: letting go drops it where it is, as the owner's again", "[scenario][recall]") {
  RecallLab lab(defaultContent(), at(-8, 0), at(0, 0), kP0, 0b1);
  for (int k = 0; k < 30; ++k)
    (void)lab.step(holding(kThrow));
  const Boomerang* b = lab.pulled();
  REQUIRE(b != nullptr);
  const Vec2 where = b->transform.pos;
  CHECK(where.x < Fixed{});  // it moved toward the owner
  const EventLog log = lab.step(holding(0));
  CHECK(countEvents(log, EventKind::BananaRecallEnded) == 1);
  CHECK(countEvents(log, EventKind::BoomerangGrounded) == 1);
  CHECK(lab.pulled() == nullptr);
  int lying = 0;
  lab.s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& k) {
    ++lying;
    CHECK(bananaLastOwner(k) == kP0);
    CHECK(length(k.pos - where) < Fixed::half());
  });
  CHECK(lying == 1);
  // Held again: it lifts once more.
  CHECK(countEvents(lab.step(holding(kThrow)), EventKind::BananaRecallStarted) == 1);
}

TEST_CASE("recall: a wall in the way stops the pull", "[scenario][recall]") {
  ContentDB c = defaultContent();
  c.arenas[0].walls[0] = Wall{at(-4, -3), at(-3, 3)};
  c.arenas[0].wallCount = 1;
  RecallLab lab(c, at(-8, 0), at(0, 0), kP0, 0b1);
  for (int k = 0; k < 300; ++k) {
    const EventLog log = lab.step(holding(kThrow));
    CHECK(countEvents(log, EventKind::BoomerangCaught) == 0);
    const Boomerang* b = lab.pulled();
    REQUIRE(b != nullptr);
    // Never into the wall: its face is at x = -3, the banana's radius 0.35.
    CHECK(b->transform.pos.x >= Fixed::fromRatio(-265, 100) - Fixed::fromRaw(2));
  }
  CHECK(playerAt(lab.s, kP0).ammo == 0);
}

TEST_CASE("recall: only the last owner pulls; anyone may pick it up", "[scenario][recall]") {
  // p1's banana lies by p0, whose hand is empty: holding Throw does nothing.
  RecallLab lab(defaultContent(), at(-8, 0), at(0, 0), kP1);
  playerAt(lab.s, kP1).ammo = 1;
  EventLog log;
  for (int k = 0; k < 20; ++k) {
    const EventLog one = lab.step(holding(kThrow));
    log.insert(log.end(), one.begin(), one.end());
  }
  CHECK(countEvents(log, EventKind::BananaRecallStarted) == 0);
  CHECK(lab.s.pickups.isAlive(lab.pickup));
  // A banana nobody may recall stays put too.
  lab.s.pickups.get(lab.pickup)->spawner = 0;
  CHECK(countEvents(lab.step(holding(kThrow)), EventKind::BananaRecallStarted) == 0);
  // But p0 takes it by walking over it.
  lab.s.pickups.get(lab.pickup)->spawner = bananaLastOwnerByte(kP1);
  placePlayer(lab.s, kP0, at(0, 0));
  log = lab.step(PlayerInput{});
  CHECK(countEvents(log, EventKind::PickupTaken) == 1);
  CHECK(playerAt(lab.s, kP0).ammo == 1);
}

TEST_CASE("recall: the slow start drifts through a monkey, the fast pull hits",
          "[scenario][recall]") {
  const ContentDB& c = defaultContent();
  const AbilityDef& thr = c.abilityForSlot(kAbilitySlotThrow);
  REQUIRE(thr.recallHarmSpeed > Fixed{});
  {
    // p1 stands right by the lifted banana: it leaves at a crawl.
    RecallLab lab(c, at(-8, 0), at(0, 0));
    placePlayer(lab.s, kP1, Vec2{Fixed::fromRatio(-6, 10), Fixed{}});
    EventLog log;
    for (int k = 0; k < 20; ++k) {
      const EventLog one = lab.step(holding(kThrow));
      log.insert(log.end(), one.begin(), one.end());
    }
    REQUIRE(lab.pulled() != nullptr);
    CHECK(length(lab.pulled()->transform.vel) < thr.recallHarmSpeed);
    CHECK(countEvents(log, EventKind::PlayerHit) == 0);
    CHECK(playerAt(lab.s, kP1).hp > 0);
  }
  {
    // p1 stands by the puller: the banana arrives at full pull.
    RecallLab lab(c, at(-8, 0), at(0, 0));
    placePlayer(lab.s, kP1, at(-6, 0));
    EventLog log;
    for (int k = 0; k < 90 && findEvent(log, EventKind::PlayerHit) == nullptr; ++k) {
      const EventLog one = lab.step(holding(kThrow));
      log.insert(log.end(), one.begin(), one.end());
    }
    const SimEvent* hit = findEvent(log, EventKind::PlayerHit);
    REQUIRE(hit != nullptr);
    CHECK(hit->player == kP1);
    CHECK(hit->a == kP0.value);
  }
}

TEST_CASE("recall: a hand with a banana throws, it does not pull", "[scenario][recall]") {
  RecallLab lab(defaultContent(), at(-8, 0), at(0, 0), kP0, 0b1);
  playerAt(lab.s, kP0).ammo = 1;
  EventLog log;
  for (int k = 0; k < 30; ++k) {
    const EventLog one = lab.step(holding(kThrow));
    log.insert(log.end(), one.begin(), one.end());
  }
  CHECK(countEvents(log, EventKind::BananaRecallStarted) == 0);
  CHECK(playerAt(lab.s, kP0).abilities[kAbilitySlotThrow].phase == AbilityPhase::Startup);
}

TEST_CASE("the grid modes and the contest fly the classic banana and recall nothing",
          "[scenario][projectile][arc][grid]") {
  ContentDB db{};
  content::Diagnostics diag;
  REQUIRE(content::loadEmbeddedContent(db, diag));
  int grid = 0;
  for (uint8_t i = 0; i < db.modeCount; ++i) {
    const ModeDef& mode = db.modes[i];
    INFO("mode " << mode.name.view());
    if (mode.space != Space::Grid) {
      CHECK(mode.bananaFlight == BananaFlight::Arc);
      continue;
    }
    ++grid;
    CHECK(mode.bananaFlight == BananaFlight::Classic);
    for (uint8_t r = 0; r < mode.roleCount; ++r) {
      for (const uint8_t a : db.loadoutAbilities(mode.roles[r])) {
        CHECK_FALSE(throwRecalls(db.abilityAt(a)));
      }
    }
  }
  CHECK(grid == 4);
  // Quick Hands, the grid's power-up throw, does not recall either.
  bool quick = false;
  for (uint8_t a = 0; a < db.abilityCount; ++a) {
    if (db.abilities[a].name.view() != "quick_throw") continue;
    quick = true;
    CHECK_FALSE(throwRecalls(db.abilities[a]));
  }
  CHECK(quick);
}

// Not run by default: the reach and air time of a tap and a full charge in
// each flight (the numbers in architecture §5.7 and the v0.89 changelog).
TEST_CASE("arc: measure the reach and the air time", "[.][measure]") {
  const ContentDB& arc = defaultContent();
  const ContentDB& classic = classicContent();
  for (const auto& [name, c] : {std::pair{"classic", &classic}, std::pair{"arc", &arc}}) {
    for (const int held : {kTap, kFull}) {
      const Flight f = throwEast(*c, held);
      std::printf("%-8s %-4s reach %.2f units, in the air %d ticks, returning from tick %d\n", name,
                  held == kTap ? "tap" : "full", static_cast<double>(f.reach), f.airTicks,
                  f.returningAt);
    }
  }
  CHECK(true);
}
