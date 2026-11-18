// A slow pull passes (prio 5, architecture §5.6, §5.7, v0.89):
// Player[] × Boomerang[Recalled, !own]. A banana its owner pulls back is in
// flight for every row below (it hits, is caught, parried, slammed or
// blocked like a returning banana), but only once it flies at least the
// owner's recalling def's recallHarmSpeed: slower, this row takes the
// contact and nothing happens (it drifts through the monkey). The owner's
// own pull is never stopped here (row 40 catches it at any speed).
#include "kfm/sim/abilities/throw.hpp"
#include "kfm/sim/rules/handlers.hpp"

namespace kfm::sim {

bool slowRecallPasses(WorldState& s, const MatchContent& content, EntityId /*a*/, EntityId b,
                      const Contact& /*contact*/, EventSink& /*sink*/) {
  const Boomerang* banana = s.boomerangs.get(b);
  if (banana == nullptr) return false;
  const Player* owner = s.playerBySlot(banana->owner);
  const AbilityDef* def = owner != nullptr ? recallDefOf(*owner, content) : nullptr;
  if (def == nullptr) return false;
  const Fixed harm = def->recallHarmSpeed * banana->speedMul;
  return lengthSqRaw(banana->transform.vel) < lengthSqRaw(Vec2{harm, Fixed{}});
}

}  // namespace kfm::sim
