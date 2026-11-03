// MovementSystem (architecture §5.3 step 4): intent × stats -> velocity,
// integrate, arena clamp.
//
// Per player: prevPos = pos (the swept-collision start); velocity is zero
// outside MatchPhase::Playing, left as AbilitySystem set it while Dashing, and
// intent.move * stats.moveSpeed otherwise; HitStun overrides everything with
// zero. Then pos += vel and pos is clamped per axis to the arena box shrunk by
// the player radius, zeroing the velocity component that was clamped.
#include "kfm/sim/systems.hpp"

namespace kfm::sim {
namespace {

// Clamps one axis to [-limit, limit]; zeroes the velocity component on contact.
void clampAxis(Fixed& pos, Fixed& vel, Fixed limit) {
  if (pos > limit) {
    pos = limit;
    vel = Fixed::zero();
  } else if (pos < -limit) {
    pos = -limit;
    vel = Fixed::zero();
  }
}

}  // namespace

void movementSystem(WorldState& s, const MatchContent& content) {
  const bool playing = s.rules.phase == MatchPhase::Playing;
  const Vec2 limit =
      content.arena.halfExtents - Vec2{content.db.player.radius, content.db.player.radius};
  s.players.forEachAlive([&](EntityId /*id*/, Player& p) {
    Transform& t = p.transform;
    t.prevPos = t.pos;
    if (!playing) {
      t.vel = Vec2::zero();
    } else if (!p.tags.has(Tag::Dashing)) {
      t.vel = p.intent.move * p.stats.moveSpeed;
    }
    if (p.tags.has(Tag::HitStun)) t.vel = Vec2::zero();
    t.pos += t.vel;
    clampAxis(t.pos.x, t.vel.x, limit.x);
    clampAxis(t.pos.y, t.vel.y, limit.y);
  });
}

}  // namespace kfm::sim
