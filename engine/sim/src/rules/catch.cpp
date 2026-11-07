// Perfect-dash catch (prio 10, architecture §5.6, §5.11). Judged against the
// press sub-tick rather than the tick boundary: perfect iff
// 0 <= hitTime - pressTime < perfectWindowTicks, where
// pressTime = activatedTick + activatedSubTick / 256 and hitTime = tick + tHit.
// Outside the window the handler declines and the scan falls through to the
// damage rule.
//
// A catcher already at ammoMax also declines (v0.16, §12): a non-own banana
// in flight then falls through to row 30 (the full-ammo dasher is hit) and an
// own returning banana to row 40 (auto catch, absorbed even at full ammo).
//
// v0.57: a perfect catch of an enemy's banana (its thrower known and on
// another team; every other slot in a free-for-all) adds ModeDef::catchScore
// to the catcher's slot (ScoreChanged, reason Catch); the own banana or an
// ally's scores nothing. Only point-scoring kinds carry a catch score.
#include "kfm/sim/rules/handlers.hpp"
#include "kfm/sim/systems.hpp"
#include "modes/mode_flow.hpp"

namespace kfm::sim {

bool catchBoomerang(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
                    const Contact& contact, EventSink& sink) {
  Player* p = s.players.get(a);
  if (p == nullptr || !s.boomerangs.isAlive(b)) return false;
  // The slot whose perfect window is open (the base kit's dash slot); a tag
  // set by something else (a test) falls back to the dash slot.
  const AbilityState* dashSlot = slotGranting(*p, content, Tag::PerfectDash);
  const AbilityState& dash = dashSlot != nullptr ? *dashSlot : p->abilities[kAbilitySlotDash];
  const AbilityDef& def = content.db.abilityAt(dash.defIndex);

  // Decline reasons, in this order: (1) no room for another banana, (2) the
  // contact lies outside the perfect window. Both fall through the same way;
  // the order only matters to readers and tools counting decline reasons.
  if (p->ammo >= p->stats.ammoMax) return false;
  const Fixed delta = Fixed::fromInt(s.tick.minus(dash.activatedTick)) + contact.tHit -
                      Fixed::fromRatio(dash.activatedSubTick, 256);
  if (delta < Fixed::zero() || delta >= Fixed::fromInt(def.perfectWindowTicks)) return false;

  const PlayerId thrower = s.boomerangs.get(b)->owner;
  s.boomerangs.despawn(b);
  ++p->ammo;  // below ammoMax, checked above
  sink.push(SimEvent{EventKind::BoomerangCaught, s.tick, p->id, b,
                     static_cast<uint32_t>(CatchHow::Perfect), 0});
  if (thrower.isValid() && thrower.value < kMaxPlayers &&
      content.relation(thrower, p->id) == Relation::Enemy) {
    addScore(s, p->id, content.mode.catchScore, ScoreReason::Catch, sink);
  }
  return true;
}

}  // namespace kfm::sim
