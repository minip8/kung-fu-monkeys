// The continuous space (architecture §5.3 steps 2 and 4, §5.12): the sim as
// built, bit for bit.
//
// latch: facing follows aim on every tick (aim always wins; movement never
// rotates the body). No commitment: Player::moveTarget and moveTicks stay
// zero.
//
// move, per player: prevPos = pos (the swept-collision start); velocity is
// zero outside MatchPhase::Playing, left as AbilitySystem set it while
// Dashing, and intent.move * stats.moveSpeed otherwise; HitStun overrides
// everything with zero. Then the player moves by vel through
// statics::moveCircle (Slide): interior walls stop the normal component and
// keep the tangential one, for stick movement and dash displacement alike (a
// dash stops at a wall while its phases run on), and pos is clamped per axis
// to the arena box shrunk by the player radius, zeroing the velocity
// component that was clamped. Without walls this is the M1
// integrate-and-clamp, bit for bit. A pending knockback (v0.21, a slam's
// push) is added to the velocity, stun or not, while it lasts.
#include "spaces/spaces.hpp"
#include "systems/statics.hpp"

namespace kfm::sim {

void continuousLatch(WorldState& s, const MatchContent& /*content*/, const InputFrame& /*frame*/) {
  s.players.forEachAlive([](EntityId /*id*/, Player& p) { p.transform.facing = p.intent.aim; });
}

void continuousMove(WorldState& s, const MatchContent& content) {
  const bool playing = s.rules.phase == MatchPhase::Playing;
  const statics::MoveParams params{.radius = content.db.player.radius,
                                   .response = statics::Response::Slide};
  s.players.forEachAlive([&](EntityId /*id*/, Player& p) {
    Transform& t = p.transform;
    t.prevPos = t.pos;
    if (!playing) {
      t.vel = Vec2::zero();
    } else if (!p.tags.has(Tag::Dashing)) {
      t.vel = p.intent.move * p.stats.moveSpeed;
    }
    if (p.tags.has(Tag::HitStun)) t.vel = Vec2::zero();
    if (playing && p.knockbackTicks > 0) {  // a slam's push (v0.21) adds on top of anything
      t.vel = t.vel + p.knockback;
      if (--p.knockbackTicks == 0) p.knockback = Vec2::zero();
    }
    t.pos = statics::moveCircle(content.arena, t.pos, t.vel, t.vel, params).pos;
  });
}

}  // namespace kfm::sim
