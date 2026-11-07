// Falling into a pit (prio 60, architecture §5.6, §5.10, v0.57). CollisionSystem
// emits the pit contact once a player's whole contact disc lies over the
// arena's pits (their union, discOverPits); the row forbids Dashing, so a
// dash carries a monkey over a pit and how wide a pit it clears is the
// dash's reach. The handler starts the fall: Tag::Falling,
// Player::fallTicks = PlayerDef::fallTicks, its velocity, push and grid
// commitment dropped, every ability slot back to Idle without
// onPhaseChange or onEnd (a charging throw is dropped, not thrown; the next
// AbilitySystem pass clears the phase tags), its bananas away from it
// dropped, and PlayerFell. From then on no rule row touches it (kGone),
// InputSystem hands it an idle intent (so no slot activates again) and the
// spaces leave it where it is, and LifecycleSystem
// counts the fall down and eliminates it (eliminateFallen) as if hit by
// nobody: hp 0, Tag::Eliminated, PlayerEliminated with killer 0xFF (no
// PlayerHit), and the mode hears about it like any elimination
// (ModeBehavior::onEliminated with an invalid killer: a deathmatch counts
// the death and schedules the respawn, nobody scores). Other hazard kinds
// decline here: Slow acts through ModifierSystem's derived stats.
//
// The faller's bananas (v0.57): the one in its hand goes down with it; every
// one away from it (outbound or returning) drops where it is the moment the
// fall begins (dropBananasOf, as at any death), so LifecycleSystem lays it as
// a pickup this very tick, or sinks it if it came down over a pit.
#include "kfm/sim/modes/registry.hpp"
#include "kfm/sim/rules/handlers.hpp"
#include "rules/fall.hpp"

namespace kfm::sim {

bool fallIntoPit(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                 const Contact& /*contact*/, EventSink& sink) {
  if (b.index >= content.arena.hazardCount || b.index >= kMaxArenaHazards) return false;
  if (content.arena.hazards[b.index].kind != HazardKind::Pit) return false;
  Player* victim = s.players.get(a);
  if (victim == nullptr) return false;
  victim->tags.set(Tag::Falling);
  victim->fallTicks = content.db.player.fallTicks > 0 ? content.db.player.fallTicks : uint16_t{1};
  victim->transform.vel = Vec2::zero();
  victim->knockback = Vec2::zero();
  victim->knockbackTicks = 0;
  victim->moveTicks = 0;
  for (AbilityState& st : victim->abilities) {
    st.phase = AbilityPhase::Idle;
    st.phaseTicks = 0;
  }
  dropBananasOf(s, victim->id);  // it is doomed: they come down now
  sink.push(SimEvent{EventKind::PlayerFell, s.tick, victim->id, a, b.index, 0});
  return true;
}

void dropBananasOf(WorldState& s, PlayerId owner) {
  s.boomerangs.forEachAlive([&](EntityId /*id*/, Boomerang& banana) {
    if (banana.owner != owner) return;
    if (banana.phase != BoomerangPhase::Outbound && banana.phase != BoomerangPhase::Returning) {
      return;
    }
    banana.phase = BoomerangPhase::Grounded;
    banana.ticksInPhase = 0;
    banana.transform.vel = Vec2::zero();
  });
}

void eliminateFallen(WorldState& s, const MatchContent& content, Player& p, EventSink& sink) {
  p.hp = 0;
  p.fallTicks = 0;
  p.tags.set(Tag::Eliminated);
  sink.push(SimEvent{EventKind::PlayerEliminated, s.tick, p.id, s.players.idAt(p.id.value),
                     PlayerId::invalid().value, 0});
  modeBehaviorOf(content).onEliminated(s, content, p.id, PlayerId::invalid(), sink);
}

}  // namespace kfm::sim
