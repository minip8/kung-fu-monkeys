// ProjectileSystem (architecture §5.3 step 5, §5.7): boomerang flight, return
// steering, wall bounces, grounding. Grounded boomerangs stay put until
// LifecycleSystem turns them into pickups; this system emits only
// BoomerangBounced. Outside MatchPhase::Playing every boomerang is frozen
// (prevPos = pos, no timers advance), like the players: a banana still in
// flight when the round is decided cannot hit or return to anyone during the
// RoundEnded pause.
//
// Bounces (v0.22): a banana moves by its velocity through
// statics::moveCircle (Bounce), the one code path for interior walls and the
// outer box: the crossed face mirrors the overshoot and reflects that
// velocity component scaled by BoomerangDef::restitution, a wall corner
// reflects it about the corner normal. Hazards do not touch bananas (they
// fly over pits). A returning banana that bounces keeps homing; it is
// grounded by maxReturnTicks like any other, so no extra bounce cap is
// needed.
//
// v0.57: a banana sinking into a pit (Sinking, set by LifecycleSystem where a
// grounded banana came down over one) stays put and counts its ticks in
// every match phase, so it finishes sinking even if the round is decided;
// LifecycleSystem despawns it after BoomerangDef::pitSinkTicks.
//
// v0.80: the outbound ticks and the return's turn rate are the mode's when
// it overrides them (MatchContent::bananaOutboundTicks / ::bananaTurnRateBam:
// ModeDef's, else the BoomerangDef's), so a mode reshapes the flight with
// nothing new in the world state.
//
// v0.89, two flight models, chosen by the mode (ModeDef::bananaFlight):
// Classic is the flight above, untouched (the grid modes and the bot
// contest play it bit for bit). Arc (arcFlightTick): the speed is a function
// of the phase and its ticks, nothing new in the world state either. Out,
// arcLaunchSpeed slowing evenly to arcApexSpeed over the outbound ticks; at
// the apex it turns to face its owner at once (it is slow there: the hang
// of a boomerang), then speeds up evenly to arcReturnSpeed over
// arcReturnAccelTicks, steering by at most the turn rate. A returning banana
// drops (Grounded, then a pickup) when its owner is gone, when it has been
// returning longer than arcReturnTicks, when it has passed its owner (its
// velocity points away from the owner: dot < 0, judged before it moves, so
// one that reaches the owner this tick is still caught), and when it hits a
// wall: on the way back a bounce would aim it away from home, and a wall in
// the way is a miss. A wall on the way out bounces it as ever and ages the
// flight by arcBounceTicks (ticksInPhase), so every bounce costs speed and
// brings the apex nearer. Every number is the BoomerangDef's and times the
// banana's speedMul, so the charge scales the reach and not the time.
//
// v0.89, the recall (recallTick): a Recalled banana flies straight at its
// owner while the owner holds the recalling throw's button
// (heldRecallDef), from rest up to the def's recallSpeed (times the
// banana's speedMul) over recallAccelTicks, following the owner's current
// position. Walls stop it the way they stop a monkey (Slide: it slides
// along a face and stays stuck in a pocket): a pull is an attempt, not a
// teleport. Let go, or with the owner gone, falling or no longer able to
// pull, it drops where it is (Grounded, BananaRecallEnded) and
// LifecycleSystem lays it as a pickup that its owner may lift again.
#include "kfm/sim/abilities/throw.hpp"
#include "kfm/sim/systems.hpp"
#include "systems/angle_of.hpp"
#include "systems/statics.hpp"

#include <algorithm>
#include <cstdint>

namespace kfm::sim {
namespace {

// Cosmetic spin per tick in flight. Presentation-only, so not a tuning value
// in ContentDB, but it is part of the visited state.
constexpr uint16_t kSpinBamPerTick = 3000;

constexpr void saturatingIncrement(uint16_t& v) {
  if (v < UINT16_MAX) ++v;
}

constexpr void enterPhase(Boomerang& b, BoomerangPhase phase) {
  b.phase = phase;
  b.ticksInPhase = 0;
}

// Turns facing toward `target` by at most turnRateBam and sets the velocity
// along the new facing at `speed`. A target at the boomerang's own position
// leaves the facing unchanged.
void steerToward(Boomerang& b, Vec2 target, uint16_t turnRateBam, Fixed speed) {
  Transform& t = b.transform;
  const Vec2 toTarget = target - t.pos;
  if (toTarget != Vec2::zero()) {
    const Angle desired = angleOf(toTarget);
    const int32_t rate = turnRateBam;
    const int32_t delta = std::clamp<int32_t>(desired.signedDeltaFrom(t.facing), -rate, rate);
    t.facing += Angle::fromBam(static_cast<uint16_t>(delta));
  }
  t.vel = Vec2::fromAngle(t.facing, speed);
}

void ground(Boomerang& b) {
  enterPhase(b, BoomerangPhase::Grounded);
  b.transform.vel = Vec2::zero();
}

// The arc's speed (units/tick, speedMul 1) out on its n-th outbound tick,
// from arcLaunchSpeed down to arcApexSpeed at n = outbound.
[[nodiscard]] Fixed arcOutboundSpeed(const BoomerangDef& def, uint16_t outbound, uint16_t n) {
  const int32_t total = std::max<int32_t>(outbound, 1);
  const int32_t k = std::min<int32_t>(n, total);
  return def.arcLaunchSpeed - ((def.arcLaunchSpeed - def.arcApexSpeed) * k / total);
}

// The arc's speed on its n-th returning tick (0 = the apex), from
// arcApexSpeed up to arcReturnSpeed at n = arcReturnAccelTicks.
[[nodiscard]] Fixed arcReturnSpeed(const BoomerangDef& def, uint16_t n) {
  const int32_t total = std::max<int32_t>(def.arcReturnAccelTicks, 1);
  const int32_t k = std::min<int32_t>(n, total);
  return def.arcApexSpeed + ((def.arcReturnSpeed - def.arcApexSpeed) * k / total);
}

// Whether v points away from `to` (an exact int64 dot product; coordinates
// stay within +-1024 units, velocities within a few units a tick).
[[nodiscard]] constexpr bool pointsAway(Vec2 v, Vec2 to) {
  return (int64_t{v.x.raw} * to.x.raw) + (int64_t{v.y.raw} * to.y.raw) < 0;
}

// One Playing tick of an Outbound or Returning banana in an arc mode (the
// header). ticksInPhase has already advanced.
void arcFlightTick(WorldState& s, const MatchContent& content, EntityId id, Boomerang& b,
                   EventSink& sink) {
  const BoomerangDef& def = content.db.boomerang;
  const uint16_t outbound = content.bananaOutboundTicks();
  Transform& t = b.transform;
  if (b.phase == BoomerangPhase::Outbound && b.ticksInPhase >= outbound) {
    enterPhase(b, BoomerangPhase::Returning);
    // The apex: slow enough to turn for home at once.
    if (const Player* owner = s.playerBySlot(b.owner)) {
      steerToward(b, owner->transform.pos, UINT16_MAX, Fixed{});
    }
  }

  if (b.phase == BoomerangPhase::Returning) {
    const Player* owner = s.playerBySlot(b.owner);
    if (owner == nullptr || b.ticksInPhase > def.arcReturnTicks) {
      ground(b);
      return;
    }
    // Passed its owner: it flew by (the owner moved off its line), so it
    // drops instead of circling back.
    if (b.ticksInPhase > 0 && pointsAway(t.vel, owner->transform.pos - t.pos)) {
      ground(b);
      return;
    }
    steerToward(b, owner->transform.pos, content.bananaTurnRateBam(),
                arcReturnSpeed(def, b.ticksInPhase) * b.speedMul);
  } else {
    t.vel = Vec2::fromAngle(t.facing, arcOutboundSpeed(def, outbound, b.ticksInPhase) * b.speedMul);
  }

  const statics::MoveParams params{.radius = def.radius * b.radiusMul,
                                   .response = statics::Response::Bounce,
                                   .restitution = def.restitution};
  const statics::MoveResult moved = statics::moveCircle(content.arena, t.pos, t.vel, t.vel, params);
  t.pos = moved.pos;
  if (moved.hitWall || moved.hitOuter) {
    sink.push(SimEvent{.kind = EventKind::BoomerangBounced, .tick = s.tick, .entity = id});
    if (b.phase == BoomerangPhase::Returning) {
      ground(b);  // a wall on the way home: it drops there
    } else {
      if (t.vel != Vec2::zero()) t.facing = angleOf(t.vel);
      const uint32_t aged = uint32_t{b.ticksInPhase} + def.arcBounceTicks;
      b.ticksInPhase = static_cast<uint16_t>(std::min<uint32_t>(aged, UINT16_MAX));
    }
  }
}

// One Playing tick of a Recalled banana (the header). ticksInPhase has
// already advanced.
void recallTick(WorldState& s, const MatchContent& content, EntityId id, Boomerang& b,
                EventSink& sink) {
  const Player* owner = s.playerBySlot(b.owner);
  const AbilityDef* def = owner != nullptr ? heldRecallDef(*owner, content) : nullptr;
  if (def == nullptr) {
    ground(b);
    sink.push(SimEvent{EventKind::BananaRecallEnded, s.tick, b.owner, id, 0, 0});
    return;
  }
  Transform& t = b.transform;
  const int32_t accel = std::max<int32_t>(def->recallAccelTicks, 1);
  const int32_t k = std::min<int32_t>(b.ticksInPhase, accel);
  const Fixed speed = def->recallSpeed * b.speedMul * k / accel;
  const Vec2 toOwner = owner->transform.pos - t.pos;
  if (toOwner != Vec2::zero()) t.facing = angleOf(toOwner);
  t.vel = Vec2::fromAngle(t.facing, speed);
  const statics::MoveParams params{.radius = content.db.boomerang.radius * b.radiusMul,
                                   .response = statics::Response::Slide};
  t.pos = statics::moveCircle(content.arena, t.pos, t.vel, t.vel, params).pos;
}

}  // namespace

void projectileSystem(WorldState& s, const MatchContent& content, EventSink& sink) {
  const BoomerangDef& def = content.db.boomerang;
  const uint16_t outboundTicks = content.bananaOutboundTicks();
  const uint16_t turnRateBam = content.bananaTurnRateBam();
  const statics::MoveParams base{
      .radius = def.radius, .response = statics::Response::Bounce, .restitution = def.restitution};

  const bool playing = s.rules.phase == MatchPhase::Playing;
  const bool arc = content.arcFlight();
  s.boomerangs.forEachAlive([&](EntityId id, Boomerang& b) {
    Transform& t = b.transform;
    t.prevPos = t.pos;
    if (b.phase == BoomerangPhase::Sinking) {  // v0.57, in every phase
      t.vel = Vec2::zero();
      saturatingIncrement(b.ticksAlive);
      saturatingIncrement(b.ticksInPhase);
      b.spin += Angle::fromBam(kSpinBamPerTick);
      return;
    }
    if (!playing) return;
    saturatingIncrement(b.ticksAlive);
    saturatingIncrement(b.ticksInPhase);
    if (b.phase == BoomerangPhase::Grounded) return;
    if (b.phase == BoomerangPhase::Recalled || arc) {  // v0.89
      b.spin += Angle::fromBam(kSpinBamPerTick);
      if (b.phase == BoomerangPhase::Recalled) {
        recallTick(s, content, id, b, sink);
      } else {
        arcFlightTick(s, content, id, b, sink);
      }
      return;
    }

    if (b.phase == BoomerangPhase::Outbound && b.ticksInPhase >= outboundTicks) {
      enterPhase(b, BoomerangPhase::Returning);
    }

    if (b.phase == BoomerangPhase::Returning) {
      const Player* owner = s.playerBySlot(b.owner);
      if (owner == nullptr || b.ticksInPhase > def.maxReturnTicks) {
        enterPhase(b, BoomerangPhase::Grounded);
        t.vel = Vec2::zero();
        return;
      }
      steerToward(b, owner->transform.pos, turnRateBam, def.speed * b.speedMul);
    } else if (t.vel != Vec2::zero()) {  // Outbound: straight line
      t.facing = angleOf(t.vel);
    }

    // A projectile modifier scales this banana's contact radius (§5.8).
    statics::MoveParams params = base;
    params.radius = def.radius * b.radiusMul;
    const statics::MoveResult moved =
        statics::moveCircle(content.arena, t.pos, t.vel, t.vel, params);
    t.pos = moved.pos;
    if (moved.hitWall || moved.hitOuter) {
      if (t.vel != Vec2::zero()) t.facing = angleOf(t.vel);
      // One event per bounce tick, even in a corner (presentation plays one hit).
      sink.push(SimEvent{.kind = EventKind::BoomerangBounced, .tick = s.tick, .entity = id});
    }
    b.spin += Angle::fromBam(kSpinBamPerTick);
  });
}

}  // namespace kfm::sim
