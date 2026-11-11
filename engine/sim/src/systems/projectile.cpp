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
// v0.79: the outbound ticks and the return's turn rate are the mode's when
// it overrides them (MatchContent::bananaOutboundTicks / ::bananaTurnRateBam:
// ModeDef's, else the BoomerangDef's), so a mode reshapes the flight with
// nothing new in the world state.
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

}  // namespace

void projectileSystem(WorldState& s, const MatchContent& content, EventSink& sink) {
  const BoomerangDef& def = content.db.boomerang;
  const uint16_t outboundTicks = content.bananaOutboundTicks();
  const uint16_t turnRateBam = content.bananaTurnRateBam();
  const statics::MoveParams base{
      .radius = def.radius, .response = statics::Response::Bounce, .restitution = def.restitution};

  const bool playing = s.rules.phase == MatchPhase::Playing;
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
