// Falling into a pit (prio 60, architecture §5.6, §5.10). A player whose
// centre ends the tick inside a Pit hazard is eliminated at once, as if hit
// by nobody: hp 0, Tag::Eliminated, PlayerEliminated with killer 0xFF (no
// PlayerHit; LifecycleSystem despawns it), and the mode hears about it like
// any elimination (ModeBehavior::onEliminated with an invalid killer: a
// deathmatch counts the death and schedules the respawn, nobody scores).
// Other hazard kinds decline here: Slow acts through ModifierSystem's
// derived stats.
#include "kfm/sim/modes/registry.hpp"
#include "kfm/sim/rules/handlers.hpp"

namespace kfm::sim {

bool fallIntoPit(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                 const Contact& /*contact*/, EventSink& sink) {
  if (b.index >= content.arena.hazardCount || b.index >= kMaxArenaHazards) return false;
  if (content.arena.hazards[b.index].kind != HazardKind::Pit) return false;
  Player* victim = s.players.get(a);
  if (victim == nullptr) return false;
  victim->hp = 0;
  victim->tags.set(Tag::Eliminated);
  sink.push(
      SimEvent{EventKind::PlayerEliminated, s.tick, victim->id, a, PlayerId::invalid().value, 0});
  modeBehaviorOf(content).onEliminated(s, content, victim->id, PlayerId::invalid(), sink);
  return true;
}

}  // namespace kfm::sim
