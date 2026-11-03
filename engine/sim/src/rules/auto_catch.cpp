// Owner auto-catch of a returning banana (prio 40, architecture §5.6, §5.7).
// The banana is absorbed even at full ammo (it never lingers on its owner).
#include "kfm/sim/rules/handlers.hpp"

#include <algorithm>

namespace kfm::sim {

bool autoCatch(WorldState& s, const MatchContent& /*content*/, EntityId a, EntityId b,
               const Contact& /*contact*/, EventSink& sink) {
  Player* p = s.players.get(a);
  if (p == nullptr || !s.boomerangs.isAlive(b)) return false;
  s.boomerangs.despawn(b);
  p->ammo = std::min<uint8_t>(static_cast<uint8_t>(p->ammo + 1), p->stats.ammoMax);
  sink.push(SimEvent{EventKind::BoomerangCaught, s.tick, p->id, b,
                     static_cast<uint32_t>(CatchHow::Auto), 0});
  return true;
}

}  // namespace kfm::sim
