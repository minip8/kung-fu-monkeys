// InputSystem (architecture §5.3 step 2, §5.4): decodes each player's
// PlayerInput into Intent and detects button press edges. Absent players
// get a zero input, so a held button never re-fires after a dropout. The
// stick is idle iff moveMag == 0; moveDir is always an angle (0 = +x).
// The frame's present mask is the authoritative roster (v0.17): it becomes
// MatchRules::present, so a player who joins or leaves mid-match is spawned
// or removed by RulesSystem at the next round start (§6.6).
#include "kfm/sim/systems.hpp"

namespace kfm::sim {

void inputSystem(WorldState& s, const InputFrame& in) {
  s.rules.present = static_cast<PlayerMask>(in.present & ((1u << kMaxPlayers) - 1u));
  s.players.forEachAlive([&](EntityId /*id*/, Player& p) {
    const PlayerInput pi = maskHas(in.present, p.id) ? in.inputs[p.id.value] : PlayerInput{};
    Intent intent;
    if (pi.moveMag != 0) {
      intent.move =
          Vec2::fromAngle(Angle::fromInput8(pi.moveDir), Fixed::fromRatio(pi.moveMag, 255));
    }
    intent.aim = Angle::fromInput8(pi.aimDir);
    intent.pressed = static_cast<uint8_t>(pi.buttons & ~p.prevButtons);
    intent.held = pi.buttons;
    intent.subTick = pi.subTick;
    p.intent = intent;
    p.prevButtons = pi.buttons;
    p.transform.facing = intent.aim;  // aim always wins; movement does not rotate the body
  });
}

}  // namespace kfm::sim
