// Perfect-dash catch (prio 10, architecture §5.6, §5.11). Judged against the
// press sub-tick rather than the tick boundary: perfect iff
// 0 <= hitTime - pressTime < perfectWindowTicks, where
// pressTime = activatedTick + activatedSubTick / 256 and hitTime = tick + tHit.
// Outside the window the handler declines and the scan falls through to the
// damage rule.
#include "kfm/sim/rules/handlers.hpp"

#include <algorithm>

namespace kfm::sim {

bool catchBoomerang(WorldState& s, const ContentDB& content, EntityId a, EntityId b,
                    const Contact& contact, EventSink& sink) {
  Player* p = s.players.get(a);
  if (p == nullptr || !s.boomerangs.isAlive(b)) return false;
  const AbilityState& dash = p->abilities[kAbilitySlotDash];
  const AbilityDef& def = content.abilityAt(dash.defIndex);

  const Fixed delta = Fixed::fromInt(s.tick.minus(dash.activatedTick)) + contact.tHit -
                      Fixed::fromRatio(dash.activatedSubTick, 256);
  if (delta < Fixed::zero() || delta >= Fixed::fromInt(def.perfectWindowTicks)) return false;

  s.boomerangs.despawn(b);
  p->ammo = std::min<uint8_t>(static_cast<uint8_t>(p->ammo + 1), p->stats.ammoMax);
  sink.push(SimEvent{EventKind::BoomerangCaught, s.tick, p->id, b,
                     static_cast<uint32_t>(CatchHow::Perfect), 0});
  return true;
}

}  // namespace kfm::sim
