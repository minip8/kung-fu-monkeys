// Owner auto-catch of a returning banana (prio 40, architecture §5.6, §5.7).
// The banana is absorbed even at full ammo (it never lingers on its owner);
// v0.57: at full ammo it is not lost but drops to the floor at the catch
// point (rules/bananas.hpp), so the hand holds ammoMax and the extra banana
// lies there to be taken.
// v0.95: an own pulled (Recalled) banana caught into a hand with room goes
// on to the next pull while the button is held (continueRecall).
#include "kfm/sim/abilities/throw.hpp"
#include "kfm/sim/rules/handlers.hpp"
#include "rules/bananas.hpp"

namespace kfm::sim {

bool autoCatch(WorldState& s, const MatchContent& content, EntityId a, EntityId b,
               const Contact& contact, EventSink& sink) {
  Player* p = s.players.get(a);
  const Boomerang* banana = s.boomerangs.get(b);
  if (p == nullptr || banana == nullptr) return false;
  const bool pulled = banana->phase == BoomerangPhase::Recalled;  // read before the despawn
  s.boomerangs.despawn(b);
  bool kept = false;
  if (p->ammo < p->stats.ammoMax) {
    ++p->ammo;
    kept = true;
  } else {
    dropBanana(s, content, contact.point, p->id, sink);
  }
  sink.push(SimEvent{EventKind::BoomerangCaught, s.tick, p->id, b,
                     static_cast<uint32_t>(CatchHow::Auto), 0});
  if (pulled && kept) continueRecall(s, *p, content, sink);
  return true;
}

}  // namespace kfm::sim
