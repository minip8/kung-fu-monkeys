// Arena zones (kfm/sim/zones.hpp).
#include "kfm/sim/zones.hpp"

namespace kfm::sim {

PlayerMask zoneOccupants(const WorldState& s, const ArenaDef& arena, uint8_t k) {
  if (k >= arena.zoneCount || k >= kMaxArenaZones) return 0;
  const Zone& zone = arena.zones[k];
  PlayerMask mask = 0;
  s.players.forEachAlive([&](EntityId /*id*/, const Player& p) {
    if (p.tags.has(Tag::Eliminated) || !p.id.isValid() || p.id.value >= kMaxPlayers) return;
    if (boxContains(zone.min, zone.max, p.transform.pos))
      mask = static_cast<PlayerMask>(mask | (1u << p.id.value));
  });
  return mask;
}

}  // namespace kfm::sim
