// Slam (prio 16 and 26, architecture §5.6, v0.21). On its impact tick a
// slammer (Tag::Slamming) reaches def.param0 from its centre: CollisionSystem
// widens its contact radius and marks contacts that exist only through that
// reach (Contact::reach); only rows with reachA accept them, so the wide
// radius never catches, damages, picks up or dash-attacks.
//
// - Row 16, Player[Slamming, !HitStun] × Boomerang[InFlight, !own]: the
//   banana grounds where it is (and becomes a pickup like any grounded
//   banana). Above the shield and damage rows, so a banana touching the
//   slammer itself is knocked down too; below the catch and parry rows,
//   which reward the timed defence.
// - Row 26, Player[Slamming, !HitStun] × Player[!Eliminated]: the victim is
//   pushed away from the slammer's centre by param1 units spread over the
//   slam's activeTicks, through Player::knockback (MovementSystem adds it to
//   the velocity on the following ticks). No damage, no tag condition on the
//   victim: a push is not a hit, so shields and spawn protection ignore it.
//   PlayerSlammed per pushed monkey.
#include "kfm/sim/rules/handlers.hpp"
#include "kfm/sim/systems.hpp"

namespace kfm::sim {
namespace {

// The def of the slot whose impact is this tick, or null.
[[nodiscard]] const AbilityDef* slamDef(const Player& p, const MatchContent& content,
                                        const AbilityState*& slot) {
  slot = slotGranting(p, content, Tag::Slamming);
  return slot != nullptr ? &content.db.abilityAt(slot->defIndex) : nullptr;
}

}  // namespace

bool slamGroundsBanana(WorldState& s, const MatchContent& /*content*/, EntityId a, EntityId b,
                       const Contact& contact, EventSink& /*sink*/) {
  const Player* slammer = s.players.get(a);
  Boomerang* banana = s.boomerangs.get(b);
  if (slammer == nullptr || banana == nullptr) return false;
  banana->phase = BoomerangPhase::Grounded;
  banana->ticksInPhase = 0;
  banana->transform.pos = contact.point;
  banana->transform.prevPos = contact.point;
  banana->transform.vel = Vec2::zero();
  return true;
}

bool slamPushesPlayer(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                      const Contact& /*contact*/, EventSink& sink) {
  const Player* slammer = s.players.get(a);
  Player* victim = s.players.get(b);
  if (slammer == nullptr || victim == nullptr || a == b) return false;
  const AbilityState* slot = nullptr;
  const AbilityDef* def = slamDef(*slammer, content, slot);
  if (def == nullptr || slot == nullptr || def->activeTicks == 0) return false;
  Vec2 dir = normalized(victim->transform.pos - slammer->transform.pos);
  if (dir == Vec2::zero()) dir = slot->dir;
  victim->knockback = dir * def->param1 / static_cast<int32_t>(def->activeTicks);
  victim->knockbackTicks = def->activeTicks;
  sink.push(SimEvent{EventKind::PlayerSlammed, s.tick, victim->id, a, slammer->id.value, 0});
  return true;
}

}  // namespace kfm::sim
