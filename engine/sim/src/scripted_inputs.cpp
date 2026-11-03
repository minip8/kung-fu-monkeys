// Scripted input generator (see scripted_inputs.hpp). Reads WorldState,
// never writes it; all randomness comes from the caller's Rng.
#include "kfm/sim/scripted_inputs.hpp"

#include "kfm/core/vec2.hpp"
#include "systems/angle_of.hpp"

#include <cstdint>
#include <limits>

namespace kfm::sim {
namespace {

// Nearest 1/256 turn of an angle (the PlayerInput direction encoding).
[[nodiscard]] uint8_t toInput8(Angle a) {
  return static_cast<uint8_t>(((uint32_t{a.bam} + 128u) >> 8) & 0xFFu);
}

[[nodiscard]] uint8_t directionTo(Vec2 from, Vec2 to) {
  return toInput8(angleOf(to - from));
}

[[nodiscard]] uint64_t squareRaw(Fixed r) {
  const auto v = static_cast<uint64_t>(r.raw < 0 ? -int64_t{r.raw} : int64_t{r.raw});
  return v * v;
}

[[nodiscard]] bool oneIn(Rng& rng, uint16_t n) {
  return n != 0 && rng.below(n) == 0;
}

[[nodiscard]] bool ready(const AbilityState& st) {
  return st.phase == AbilityPhase::Idle && st.cooldownTicks == 0;
}

// Wander direction: the current stick direction (decoded intent), a random
// new one now and then, and back toward the interior near a wall.
[[nodiscard]] uint8_t wanderDirection(Rng& rng, const Player& p, const ContentDB& content,
                                      const ScriptedInputConfig& config) {
  const Vec2 move = p.intent.move;
  uint8_t dir =
      move == Vec2::zero() ? static_cast<uint8_t>(rng.below(256)) : toInput8(angleOf(move));
  if (oneIn(rng, config.turnOneIn)) dir = static_cast<uint8_t>(rng.below(256));

  const Vec2 pos = p.transform.pos;
  const Vec2 limit = content.arena.halfExtents - Vec2{config.wallMargin, config.wallMargin};
  const Vec2 heading = Vec2::fromAngle(Angle::fromInput8(dir));
  const bool outX = (pos.x > limit.x && heading.x > Fixed::zero()) ||
                    (pos.x < -limit.x && heading.x < Fixed::zero());
  const bool outY = (pos.y > limit.y && heading.y > Fixed::zero()) ||
                    (pos.y < -limit.y && heading.y < Fixed::zero());
  if (outX || outY) {
    // Toward the centre, give or take an eighth of a turn.
    const int32_t turn = rng.range(-32, 32);
    dir = static_cast<uint8_t>((int32_t{directionTo(pos, Vec2::zero())} + turn) & 0xFF);
  }
  return dir;
}

}  // namespace

PlayerInput scriptedInput(Rng& rng, const WorldState& s, const ContentDB& content, PlayerId id,
                          const ScriptedInputConfig& config) {
  const Player* self = s.playerBySlot(id);
  if (self == nullptr) return PlayerInput{};
  const Player& p = *self;
  const Vec2 pos = p.transform.pos;

  PlayerInput in;
  in.moveMag = 255;
  in.moveDir = wanderDirection(rng, p, content, config);

  // Out of ammo: go for the nearest banana lying within reach.
  if (p.ammo == 0) {
    uint64_t best = squareRaw(config.pickupRadius);
    bool found = false;
    Vec2 target{};
    s.pickups.forEachAlive([&](EntityId /*kid*/, const Pickup& k) {
      const uint64_t d = lengthSqRaw(k.pos - pos);
      if (d <= best) {
        best = d;
        target = k.pos;
        found = true;
      }
    });
    if (found && target != pos) in.moveDir = directionTo(pos, target);
  }

  // Aim at the nearest other alive player, with a little jitter.
  uint64_t nearest = std::numeric_limits<uint64_t>::max();  // stays max with nobody else alive
  Vec2 aimAt = pos + Vec2::fromAngle(p.transform.facing);
  Vec2 enemyAt{};
  s.players.forEachAlive([&](EntityId /*pid*/, const Player& other) {
    if (other.id == p.id) return;
    const uint64_t d = lengthSqRaw(other.transform.pos - pos);
    if (d < nearest) {
      nearest = d;
      aimAt = other.transform.pos;
      enemyAt = other.transform.pos;
    }
  });
  const int32_t jitter = rng.range(-int32_t{config.aimJitter}, int32_t{config.aimJitter});
  in.aimDir = static_cast<uint8_t>((int32_t{directionTo(pos, aimAt)} + jitter) & 0xFF);

  // Buttons: only meaningful while playing, and only as fresh edges.
  if (s.rules.phase == MatchPhase::Playing) {
    const uint8_t throwBit = buttonBit(Button::Throw);
    const uint8_t dashBit = buttonBit(Button::Dash);
    const bool throwUp = (p.prevButtons & throwBit) == 0;
    const bool dashUp = (p.prevButtons & dashBit) == 0;

    if (throwUp && p.ammo > 0 && ready(p.abilities[kAbilitySlotThrow]) &&
        oneIn(rng, config.throwOneIn)) {
      in.buttons |= throwBit;
    }

    if (dashUp && ready(p.abilities[kAbilitySlotDash])) {
      // A non-own banana in flight, close and approaching: dash into it.
      const uint64_t threatSq = squareRaw(config.threatRadius);
      bool threat = false;
      Vec2 threatPos{};
      s.boomerangs.forEachAlive([&](EntityId /*bid*/, const Boomerang& b) {
        if (threat || b.phase == BoomerangPhase::Grounded || b.owner == p.id) return;
        const Vec2 rel = b.transform.pos - pos;
        if (lengthSqRaw(rel) <= threatSq && dot(rel, b.transform.vel) < Fixed::zero()) {
          threat = true;
          threatPos = b.transform.pos;
        }
      });
      // A close enemy and enough ammo for a dash attack: dash straight at it.
      const bool enemyClose = nearest <= squareRaw(config.dashAttackRadius);
      if (threat && rng.below(256) < config.dodgePer256) {
        in.buttons |= dashBit;
        if (threatPos != pos) in.moveDir = directionTo(pos, threatPos);
      } else if (enemyClose && p.ammo >= content.dashAttack.minAmmo &&
                 oneIn(rng, config.dashAttackOneIn)) {
        in.buttons |= dashBit;
        if (enemyAt != pos) in.moveDir = directionTo(pos, enemyAt);
      } else if (oneIn(rng, config.dashOneIn)) {
        in.buttons |= dashBit;
      }
    }
  }
  if (in.buttons != 0) in.subTick = static_cast<uint8_t>(rng.below(256));
  return in;
}

InputFrame scriptedFrame(Rng& rng, const WorldState& s, const ContentDB& content,
                         const ScriptedInputConfig& config) {
  InputFrame f;
  f.tick = s.tick;
  f.present = s.rules.present;
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const PlayerId id{i};
    if (maskHas(f.present, id)) f.inputs[i] = scriptedInput(rng, s, content, id, config);
  }
  return f;
}

}  // namespace kfm::sim
