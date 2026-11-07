// Falling into a pit (prio 60, architecture §5.6, §5.10, v0.57). CollisionSystem
// emits the pit contact once a player's whole contact disc lies over the
// arena's pits (their union, discOverPits); the row forbids Dashing, so a
// dash carries a monkey over a pit and how wide a pit it clears is the
// dash's reach. The handler starts the fall: Tag::Falling,
// Player::fallTicks = PlayerDef::fallTicks, its velocity, push and grid
// commitment dropped, every ability slot back to Idle without
// onPhaseChange or onEnd (a charging throw is dropped, not thrown; the next
// AbilitySystem pass clears the phase tags), its own bananas still outbound
// turned for home at once, and PlayerFell. From then on no rule row but 41
// touches it (kGone), InputSystem hands it an idle intent (so no slot
// activates again) and the spaces leave it where it is, and LifecycleSystem
// counts the fall down and eliminates it (eliminateFallen) as if hit by
// nobody: hp 0, Tag::Eliminated, PlayerEliminated with killer 0xFF (no
// PlayerHit), and the mode hears about it like any elimination
// (ModeBehavior::onEliminated with an invalid killer: a deathmatch counts
// the death and schedules the respawn, nobody scores). Other hazard kinds
// decline here: Slow acts through ModifierSystem's derived stats.
//
// The faller's bananas go down the pit with it and never become pickups
// (v0.57): one coming home meets it (row 41, bananaFallsWithOwner: one more
// in the hand, BoomerangFell{WithOwner}); whatever is still away when the
// fall ends is lost where it is (eliminateFallen, BoomerangFell{Lost}), and
// one that grounds while its owner falls is lost too (LifecycleSystem).
#include "kfm/sim/modes/registry.hpp"
#include "kfm/sim/rules/handlers.hpp"
#include "rules/fall.hpp"

#include <algorithm>

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
  const PlayerId owner = victim->id;
  s.boomerangs.forEachAlive([&](EntityId /*id*/, Boomerang& banana) {
    if (banana.owner != owner || banana.phase != BoomerangPhase::Outbound) return;
    banana.phase = BoomerangPhase::Returning;
    banana.ticksInPhase = 0;
  });
  sink.push(SimEvent{EventKind::PlayerFell, s.tick, owner, a, b.index, 0});
  return true;
}

bool bananaFallsWithOwner(WorldState& s, const MatchContent& /*content*/, EntityId a, EntityId b,
                          const Contact& /*contact*/, EventSink& sink) {
  Player* p = s.players.get(a);
  if (p == nullptr || !s.boomerangs.isAlive(b)) return false;
  s.boomerangs.despawn(b);
  p->ammo = std::min<uint8_t>(static_cast<uint8_t>(p->ammo + 1), p->stats.ammoMax);
  sink.push(SimEvent{EventKind::BoomerangFell, s.tick, p->id, b,
                     static_cast<uint32_t>(BananaFall::WithOwner), 0});
  return true;
}

void loseBanana(WorldState& s, EntityId banana, EventSink& sink) {
  const Boomerang* b = s.boomerangs.get(banana);
  if (b == nullptr) return;
  const PlayerId owner = b->owner;
  s.boomerangs.despawn(banana);
  sink.push(SimEvent{EventKind::BoomerangFell, s.tick, owner, banana,
                     static_cast<uint32_t>(BananaFall::Lost), 0});
}

void eliminateFallen(WorldState& s, const MatchContent& content, Player& p, EventSink& sink) {
  p.hp = 0;
  p.fallTicks = 0;
  p.tags.set(Tag::Eliminated);
  sink.push(SimEvent{EventKind::PlayerEliminated, s.tick, p.id, s.players.idAt(p.id.value),
                     PlayerId::invalid().value, 0});
  const PlayerId owner = p.id;
  s.boomerangs.forEachAlive([&](EntityId id, const Boomerang& banana) {
    if (banana.owner == owner) loseBanana(s, id, sink);
  });
  modeBehaviorOf(content).onEliminated(s, content, p.id, PlayerId::invalid(), sink);
}

}  // namespace kfm::sim
