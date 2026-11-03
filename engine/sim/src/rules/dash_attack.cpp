// Dash attack (prio 25, architecture §5.6, §5.10). A player dashing into an
// enemy while holding at least content.dashAttack.minAmmo bananas damages it
// like a banana hit (rules/apply_damage.hpp: hp, HitStun, PlayerEliminated at
// 0 hp), emitting PlayerDashHit instead of PlayerHit. Nothing grounds and no
// pickup spawns; the attacker spends a banana only when consumesAmmo is set.
//
// a is the attacker, b the victim. InteractionSystem tries a Player × Player
// contact in both orientations, so A is always the one dashing; the row's tags
// keep a stunned or eliminated player from attacking or being hit. Without
// enough ammo the handler declines (no other row matches, so nothing happens).
#include "kfm/sim/rules/handlers.hpp"
#include "rules/apply_damage.hpp"

namespace kfm::sim {

bool dashAttack(WorldState& s, const ContentDB& content, EntityId a, EntityId b,
                const Contact& /*contact*/, EventSink& sink) {
  Player* attacker = s.players.get(a);
  Player* victim = s.players.get(b);
  if (attacker == nullptr || victim == nullptr || a == b) return false;
  const DashAttackDef& def = content.dashAttack;
  if (attacker->ammo < def.minAmmo) return false;

  if (def.consumesAmmo != 0 && attacker->ammo > 0) --attacker->ammo;
  applyDamage(s.tick, content, *victim, b, def.damage,
              HitSource{EventKind::PlayerDashHit, a, attacker->id.value}, sink);
  return true;
}

}  // namespace kfm::sim
