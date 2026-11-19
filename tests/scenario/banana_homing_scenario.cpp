// The arc's way home (architecture §5.7, v0.95), through step(): a thrower
// who walks off after a throw still gets the banana back, one who dashes off
// can lose it; a miss glides on past the thrower, slowing to rest, still in
// flight (caught on contact, it hits, it can be dash-caught), and lands
// without a second pass; a wall on the way home bounces it into a glide; the
// time in the air is bounded by the content's numbers, walls or not.
#include "kfm/sim/step.hpp"
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kThrow = buttonBit(Button::Throw);
constexpr uint8_t kDash = buttonBit(Button::Dash);
constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};
constexpr int kTap = 1;    // ticks held: a press
constexpr int kFull = 42;  // the startup and the 40 ticks of a full charge

Vec2 at(int32_t x, int32_t y) {
  return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)};
}

// The default content on an open floor: the dojo's box widened to 60 x 60
// units, no walls, so a thrower can walk for seconds.
ContentDB openFloor() {
  ContentDB c = defaultContent();
  c.arenas[0].halfExtents = at(60, 60);
  return c;
}

// The most ticks an arc banana is in the air: out, homing, the tick it gives
// up, the glide, and the tick it lands.
int airBound(const BoomerangDef& d) {
  return int{d.arcOutboundTicks} + int{d.arcReturnTicks} + int{d.arcGlideTicks} + 2;
}

// What the thrower does from the release on. Sidestep: stand until the
// banana on its way home is 3 units off, then dash `dir` once and walk on.
enum class Move : uint8_t { Stand, Walk, DashOnce, DashAway, Sidestep };

struct Plan {
  Move move{Move::Stand};
  uint8_t dir{0};  // moveDir (0 east, 64 north)
};

struct Flight {
  bool caught{false};
  bool landed{false};
  int airTicks{-1};        // release to the catch or the landing
  int glideAt{-1};         // ticks after the release it began to glide (-1: never)
  int bounces{0};          // BoomerangBounced events
  float reach{0.0f};       // farthest from the thrower
  float landedFrom{0.0f};  // the thrower's distance from where it landed
};

// p0 at `from` throws along `aim`, holding the button `held` ticks, then
// follows `plan` until the banana is caught or lands.
Flight throwAndMove(const ContentDB& c, int held, Plan plan, Vec2 from = Vec2{}, uint8_t aim = 0) {
  const MatchContent m(c);
  WorldState s = makePlayingWorld(7, 0b1, c);
  placePlayer(s, kP0, from);
  playerAt(s, kP0).ammo = 1;
  Flight out;
  int since = -1;
  EntityId id{};
  int dodged = -1;  // Sidestep: ticks after the release it dashed
  for (int k = 0; k < 600; ++k) {
    PlayerInput in{
        .moveDir = 0, .moveMag = 0, .aimDir = aim, .buttons = k < held ? kThrow : uint8_t{0}};
    if (plan.move == Move::Sidestep && since >= 0 && dodged < 0) {
      const Boomerang* b = s.boomerangs.get(id);
      if (b != nullptr && b->phase == BoomerangPhase::Returning &&
          length(b->transform.pos - playerAt(s, kP0).transform.pos) < Fixed::fromInt(3)) {
        dodged = since;
      }
    }
    if (since >= 0 && plan.move != Move::Stand && (plan.move != Move::Sidestep || dodged >= 0)) {
      in.moveDir = plan.dir;
      in.moveMag = 255;
      const bool dash = (plan.move == Move::DashOnce && since == 0) ||
                        plan.move == Move::DashAway ||
                        (plan.move == Move::Sidestep && since == dodged);
      // A press: the button down on even ticks only.
      if (dash && since % 2 == 0) in.buttons = kDash;
    }
    InputFrame f = frameFor(s);
    f.inputs[0] = in;
    EventLog log;
    runTicks(s, f, 1, m, log);
    if (const SimEvent* e = findEvent(log, EventKind::BoomerangThrown)) {
      id = e->entity;
      since = 0;
      out.bounces += countEvents(log, EventKind::BoomerangBounced);
      continue;
    }
    if (since < 0) continue;
    ++since;
    out.bounces += countEvents(log, EventKind::BoomerangBounced);
    const Vec2 owner = playerAt(s, kP0).transform.pos;
    if (const Boomerang* b = s.boomerangs.get(id)) {
      out.reach = std::max(out.reach, length(b->transform.pos - owner).toFloat());
      if (b->phase == BoomerangPhase::Gliding && out.glideAt < 0) out.glideAt = since;
    }
    if (findEvent(log, EventKind::BoomerangCaught) != nullptr) {
      out.caught = true;
      out.airTicks = since;
      break;
    }
    if (findEvent(log, EventKind::BoomerangGrounded) != nullptr) {
      out.landed = true;
      out.airTicks = since;
      s.pickups.forEachAlive([&](EntityId /*kid*/, const Pickup& p) {
        CHECK(bananaLastOwner(p) == kP0);  // its thrower's to recall
        out.landedFrom = length(p.pos - owner).toFloat();
      });
      break;
    }
  }
  return out;
}

struct NamedPlan {
  const char* name{""};
  Plan plan;
};

constexpr NamedPlan kWalks[] = {
    {"stand", {Move::Stand, 0}},    {"walk E (out)", {Move::Walk, 0}},
    {"walk NE", {Move::Walk, 32}},  {"walk N", {Move::Walk, 64}},
    {"walk NW", {Move::Walk, 96}},  {"walk W (away)", {Move::Walk, 128}},
    {"walk SW", {Move::Walk, 160}}, {"walk S", {Move::Walk, 192}},
    {"walk SE", {Move::Walk, 224}},
};

constexpr NamedPlan kDashes[] = {
    {"dash once W", {Move::DashOnce, 128}}, {"dash once N", {Move::DashOnce, 64}},
    {"dash away W", {Move::DashAway, 128}}, {"dash away N", {Move::DashAway, 64}},
    {"sidestep N", {Move::Sidestep, 64}},
};

// p0 at the origin with an empty hand, its own banana of `phase` at `pos`
// moving along x at `vel` (units/tick), `ticksInPhase` into the phase.
struct GlideLab {
  ContentDB db{openFloor()};
  WorldState s;
  EntityId id{};

  GlideLab(Vec2 pos, Vec2 vel, BoomerangPhase phase, uint16_t ticksInPhase,
           PlayerMask players = 0b1)
      : s(makePlayingWorld(13, players, db)) {
    placePlayer(s, kP0, Vec2{});
    playerAt(s, kP0).ammo = 0;
    if ((players & 0b10) != 0) placePlayer(s, kP1, at(30, 30));
    id = spawnBanana(s, kP0, pos, vel, phase, db);
    Boomerang& b = s.boomerangs.items[id.index];
    b.ticksInPhase = ticksInPhase;
    b.transform.facing = Angle::fromDegrees(vel.x < Fixed{} ? 180 : 0);
  }
  EventLog step(PlayerInput p0 = {}, PlayerInput p1 = {}) {
    InputFrame f = frameFor(s);
    f.inputs[0] = p0;
    f.inputs[1] = p1;
    EventLog log;
    runTicks(s, f, 1, MatchContent(db), log);
    return log;
  }
  [[nodiscard]] const Boomerang* banana() const { return s.boomerangs.get(id); }
};

}  // namespace

TEST_CASE("arc homing: a thrower who walks off in any direction gets the banana back",
          "[scenario][projectile][arc][homing]") {
  const ContentDB c = openFloor();
  for (const int held : {kTap, kFull}) {
    for (const NamedPlan& p : kWalks) {
      INFO((held == kTap ? "tap, " : "full charge, ") << p.name);
      const Flight f = throwAndMove(c, held, p.plan);
      CHECK(f.caught);
      CHECK(f.airTicks <= airBound(c.boomerang));
      // Homed in, not picked up on a glide: the full-speed walks of the
      // measured table are caught at least 7 ticks before the homing ends.
      CHECK(f.glideAt < 0);
      CHECK(f.airTicks + 7 <= int{c.boomerang.arcOutboundTicks} + c.boomerang.arcReturnTicks);
    }
  }
}

TEST_CASE("arc homing: a thrower who dashes away loses it; it lands, its own to recall",
          "[scenario][projectile][arc][homing]") {
  const ContentDB c = openFloor();
  for (const int held : {kTap, kFull}) {
    INFO((held == kTap ? "tap" : "full charge"));
    const Flight f = throwAndMove(c, held, Plan{Move::DashAway, 128});
    CHECK_FALSE(f.caught);
    REQUIRE(f.landed);
    CHECK(f.glideAt > 0);
    CHECK(f.airTicks <= airBound(c.boomerang));
    CHECK(f.airTicks <= 150);  // the owner's 2.5 s
    CHECK(f.landedFrom > 3.0f);
  }
  // One dash straight back is made up for: the top speed home is never
  // below arc_return_min_speed (18 units/s), even for a tap.
  for (const int held : {kTap, kFull}) {
    CHECK(throwAndMove(c, held, Plan{Move::DashOnce, 128}).caught);
  }
}

TEST_CASE("arc homing: a miss glides on past its thrower, slowing to rest, and lands",
          "[scenario][projectile][arc][homing]") {
  // Homing at full speed toward a thrower at the origin, but 1.2 units off
  // its line, 2 units out: too late to turn in, it flies by.
  const BoomerangDef& d = openFloor().boomerang;
  GlideLab lab(Vec2{Fixed::fromInt(2), Fixed::fromRatio(12, 10)}, Vec2{-d.arcReturnSpeed, Fixed{}},
               BoomerangPhase::Returning, 40);
  int glideStart = -1;
  int ticks = 0;
  Angle heading{};
  Fixed lastSpeed{};
  Fixed lastDistance{};
  Vec2 glideFrom{};
  EventLog log;
  for (; ticks < 120; ++ticks) {
    const EventLog one = lab.step();
    log.insert(log.end(), one.begin(), one.end());
    const Boomerang* b = lab.banana();
    if (b == nullptr || b->phase == BoomerangPhase::Grounded) break;
    if (b->phase != BoomerangPhase::Gliding) {
      REQUIRE(glideStart < 0);  // never back to homing once it glides
      continue;
    }
    const Fixed speed = length(b->transform.vel);
    const Fixed distance = length(b->transform.pos);
    if (glideStart < 0) {
      glideStart = ticks;
      heading = b->transform.facing;
      glideFrom = b->transform.prevPos;
      CHECK(speed > d.arcReturnSpeed / 2);  // it carries on at speed
    } else {
      CHECK(b->transform.facing == heading);  // no steering: one pass
      CHECK(speed < lastSpeed);               // slowing evenly
      CHECK(distance > lastDistance);         // away from its thrower
    }
    lastSpeed = speed;
    lastDistance = distance;
  }
  REQUIRE(glideStart >= 0);
  CHECK(countEvents(log, EventKind::BoomerangCaught) == 0);
  REQUIRE(countEvents(log, EventKind::BoomerangGrounded) == 1);
  // The glide lasted arc_glide_ticks and covered about speed x ticks / 2.
  CHECK(ticks - glideStart == int{d.arcGlideTicks});
  int lying = 0;
  lab.s.pickups.forEachAlive([&](EntityId /*kid*/, const Pickup& k) {
    ++lying;
    CHECK(bananaLastOwner(k) == kP0);
    const float glided = length(k.pos - glideFrom).toFloat();
    const float expected = d.arcReturnSpeed.toFloat() * static_cast<float>(d.arcGlideTicks) / 2.0f;
    CHECK(glided > expected * 0.8f);
    CHECK(glided < expected * 1.2f);
  });
  CHECK(lying == 1);
}

TEST_CASE("arc homing: a gliding banana is in flight: caught by its owner, it hits, it is caught",
          "[scenario][projectile][arc][homing]") {
  const BoomerangDef& d = openFloor().boomerang;
  const Vec2 east{d.arcReturnSpeed / 2, Fixed{}};
  {
    // Gliding into its owner: caught on contact, as a returning banana.
    GlideLab lab(at(-2, 0), east, BoomerangPhase::Gliding, 2);
    EventLog log;
    for (int k = 0; k < 10 && findEvent(log, EventKind::BoomerangCaught) == nullptr; ++k) {
      const EventLog one = lab.step();
      log.insert(log.end(), one.begin(), one.end());
    }
    const SimEvent* caught = findEvent(log, EventKind::BoomerangCaught);
    REQUIRE(caught != nullptr);
    CHECK(caught->player == kP0);
    CHECK(playerAt(lab.s, kP0).ammo == 1);
  }
  {
    // Gliding into an enemy: it hits.
    GlideLab lab(at(-2, 10), east, BoomerangPhase::Gliding, 2, 0b11);
    placePlayer(lab.s, kP1, at(0, 10));
    EventLog log;
    for (int k = 0; k < 10 && findEvent(log, EventKind::PlayerHit) == nullptr; ++k) {
      const EventLog one = lab.step();
      log.insert(log.end(), one.begin(), one.end());
    }
    const SimEvent* hit = findEvent(log, EventKind::PlayerHit);
    REQUIRE(hit != nullptr);
    CHECK(hit->player == kP1);
  }
  {
    // A perfect dash into it catches it (row 10).
    GlideLab lab(at(-2, 10), east, BoomerangPhase::Gliding, 2, 0b11);
    placePlayer(lab.s, kP1, at(0, 10));
    playerAt(lab.s, kP1).ammo = 0;
    const PlayerInput dashWest{.moveDir = 128, .moveMag = 255, .aimDir = 128, .buttons = kDash};
    EventLog log = lab.step({}, dashWest);
    for (int k = 0; k < 6 && findEvent(log, EventKind::BoomerangCaught) == nullptr; ++k) {
      const EventLog one = lab.step({}, PlayerInput{.moveDir = 128, .moveMag = 255});
      log.insert(log.end(), one.begin(), one.end());
    }
    const SimEvent* caught = findEvent(log, EventKind::BoomerangCaught);
    REQUIRE(caught != nullptr);
    CHECK(caught->player == kP1);
    CHECK(countEvents(log, EventKind::PlayerHit) == 0);
  }
}

TEST_CASE("arc homing: a wall on the way home bounces it into a glide",
          "[scenario][projectile][arc][homing]") {
  // Homing west toward its thrower at x = -2 with a wall in between (x 2..3).
  ContentDB walled = openFloor();
  walled.arenas[0].walls[0] = Wall{at(2, -3), at(3, 3)};
  walled.arenas[0].wallCount = 1;
  const MatchContent m(walled);
  WorldState s = makePlayingWorld(9, 0b1, walled);
  placePlayer(s, kP0, at(-2, 0));
  const EntityId id = spawnBanana(s, kP0, at(8, 0), Vec2{-Fixed::half(), Fixed{}},
                                  BoomerangPhase::Returning, walled);
  Boomerang& b = s.boomerangs.items[id.index];
  b.ticksInPhase = 4;
  b.transform.facing = Angle::fromDegrees(180);
  EventLog log;
  int ticks = 0;
  bool glided = false;
  for (; ticks < 200 && findEvent(log, EventKind::BoomerangGrounded) == nullptr; ++ticks) {
    runTicks(s, frameFor(s), 1, m, log);
    if (const Boomerang* now = s.boomerangs.get(id)) {
      if (now->phase == BoomerangPhase::Gliding) glided = true;
      if (glided) CHECK(now->phase != BoomerangPhase::Returning);
    }
  }
  CHECK(glided);
  CHECK(countEvents(log, EventKind::BoomerangBounced) == 1);
  CHECK(countEvents(log, EventKind::BoomerangCaught) == 0);
  REQUIRE(countEvents(log, EventKind::BoomerangGrounded) == 1);
  CHECK(ticks <= int{walled.boomerang.arcReturnTicks} + walled.boomerang.arcGlideTicks + 2);
  s.pickups.forEachAlive([&](EntityId /*kid*/, const Pickup& k) {
    CHECK(bananaLastOwner(k) == kP0);
    CHECK(k.pos.x > Fixed::fromInt(3));  // on the far side of the wall, bounced back
    CHECK(k.pos.x < Fixed::fromInt(8));
  });
}

TEST_CASE("arc homing: no orbit, no rattle: walls or not, the air time stays bounded",
          "[scenario][projectile][arc][homing]") {
  // A small room (10 x 6 units): every throw bounces; whatever the thrower
  // does, the banana is home or down within the content's bound.
  ContentDB room = defaultContent();
  room.arenas[0].halfExtents = at(5, 3);
  room.arenas[0].walls[0] = Wall{at(-1, -1), at(1, 1)};  // a pillar in the middle
  room.arenas[0].wallCount = 1;
  const int bound = airBound(room.boomerang);
  REQUIRE(bound <= 150);
  int thrown = 0;
  for (const int held : {kTap, kFull}) {
    for (const uint8_t aim : {uint8_t{0}, uint8_t{40}, uint8_t{64}, uint8_t{100}, uint8_t{128}}) {
      for (const Plan plan : {Plan{Move::Stand, 0}, Plan{Move::Walk, 64}, Plan{Move::DashAway, 0},
                              Plan{Move::DashAway, 128}, Plan{Move::Walk, 200}}) {
        INFO("held " << held << " aim " << int{aim} << " move " << int(plan.move) << " dir "
                     << int{plan.dir});
        const Flight f = throwAndMove(room, held, plan, at(-3, 0), aim);
        CHECK((f.caught || f.landed));
        CHECK(f.airTicks <= bound);
        CHECK(f.bounces <= 8);  // a few off the walls, never a rattle
        ++thrown;
      }
    }
  }
  CHECK(thrown == 50);
}

// Not run by default: the measured table of architecture §5.7 and the v0.95
// changelog (caught or not, the time in the air, for each move).
TEST_CASE("arc homing: measure catches and air time", "[.][measure]") {
  const ContentDB c = openFloor();
  const auto print = [&](int held, const NamedPlan& p) {
    const Flight f = throwAndMove(c, held, p.plan);
    std::printf("%-4s %-14s %-6s air %3d ticks (%.2f s) glide from %3d, reach %5.2f\n",
                held == kTap ? "tap" : "full", p.name, f.caught ? "caught" : "missed", f.airTicks,
                static_cast<double>(f.airTicks) / 60.0, f.glideAt, static_cast<double>(f.reach));
  };
  for (const int held : {kTap, kFull}) {
    for (const NamedPlan& p : kWalks)
      print(held, p);
    for (const NamedPlan& p : kDashes)
      print(held, p);
  }
  CHECK(true);
}
