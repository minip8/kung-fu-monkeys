// ProjectileSystem (architecture §5.3 step 5, §5.7): boomerang flight, return
// steering, wall bounces, grounding. Grounded boomerangs stay put until
// LifecycleSystem turns them into pickups; this system emits only
// BoomerangBounced.
#include "angle_of.hpp"
#include "kfm/sim/systems.hpp"

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
void steerToward(Boomerang& b, Vec2 target, const BoomerangDef& def) {
  Transform& t = b.transform;
  const Vec2 toTarget = target - t.pos;
  if (toTarget != Vec2::zero()) {
    const Angle desired = angleOf(toTarget);
    const int32_t rate = def.turnRateBam;
    int32_t delta = desired.signedDeltaFrom(t.facing);
    if (delta > rate) delta = rate;
    if (delta < -rate) delta = -rate;
    t.facing += Angle::fromBam(static_cast<uint16_t>(delta));
  }
  t.vel = Vec2::fromAngle(t.facing, def.speed);
}

// Reflects one axis off the walls at ±limit. Returns true on a bounce.
bool reflectAxis(Fixed& pos, Fixed& vel, Fixed limit, Fixed restitution) {
  bool bounced = false;
  if (pos > limit) {
    pos = (limit * 2) - pos;
    vel = -vel * restitution;
    bounced = true;
  } else if (pos < -limit) {
    pos = (-limit * 2) - pos;
    vel = -vel * restitution;
    bounced = true;
  }
  // An overshoot larger than the whole box would mirror past the far wall.
  pos = clamp(pos, -limit, limit);
  return bounced;
}

}  // namespace

void projectileSystem(WorldState& s, const ContentDB& content, EventSink& sink) {
  const BoomerangDef& def = content.boomerang;
  const Vec2 limit = Vec2{max(content.arena.halfExtents.x - def.radius, Fixed::zero()),
                          max(content.arena.halfExtents.y - def.radius, Fixed::zero())};

  s.boomerangs.forEachAlive([&](EntityId id, Boomerang& b) {
    Transform& t = b.transform;
    t.prevPos = t.pos;
    saturatingIncrement(b.ticksAlive);
    saturatingIncrement(b.ticksInPhase);
    if (b.phase == BoomerangPhase::Grounded) return;

    if (b.phase == BoomerangPhase::Outbound && b.ticksInPhase >= def.outboundTicks) {
      enterPhase(b, BoomerangPhase::Returning);
    }

    if (b.phase == BoomerangPhase::Returning) {
      const Player* owner = s.playerBySlot(b.owner);
      if (owner == nullptr || b.ticksInPhase > def.maxReturnTicks) {
        enterPhase(b, BoomerangPhase::Grounded);
        t.vel = Vec2::zero();
        return;
      }
      steerToward(b, owner->transform.pos, def);
    } else if (t.vel != Vec2::zero()) {  // Outbound: straight line
      t.facing = angleOf(t.vel);
    }

    t.pos += t.vel;
    const bool bx = reflectAxis(t.pos.x, t.vel.x, limit.x, def.restitution);
    const bool by = reflectAxis(t.pos.y, t.vel.y, limit.y, def.restitution);
    if (bx || by) {
      if (t.vel != Vec2::zero()) t.facing = angleOf(t.vel);
      // One event per bounce tick, even in a corner (presentation plays one hit).
      sink.push(SimEvent{.kind = EventKind::BoomerangBounced, .tick = s.tick, .entity = id});
    }
    b.spin += Angle::fromBam(kSpinBamPerTick);
  });
}

}  // namespace kfm::sim
