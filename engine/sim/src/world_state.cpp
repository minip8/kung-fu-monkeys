#include "kfm/sim/world_state.hpp"

namespace kfm::sim {

WorldState initialWorld(const MatchConfig& config) {
  WorldState s{};
  s.tick = Tick{0};
  s.rng = Rng::seeded(config.seed);
  s.rules.phase = MatchPhase::Lobby;
  s.rules.present = static_cast<PlayerMask>(config.players & ((1u << kMaxPlayers) - 1u));
  s.rules.ticksInPhase = 0;
  return s;
}

}  // namespace kfm::sim
