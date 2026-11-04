// ModeBehavior registry (architecture §5.3, v0.23): one constexpr row per
// ModeKind, in enum order, and the shared no-op hooks (v0.31).
#include "kfm/sim/modes/registry.hpp"

#include "kfm/sim/modes/modes.hpp"

#include <array>
#include <cstddef>

namespace kfm::sim {

void modeNoRoundStart(WorldState& /*s*/, const MatchContent& /*content*/, EventSink& /*sink*/) {}

void modeNoPickupTaken(WorldState& /*s*/, const MatchContent& /*content*/, PlayerId /*player*/,
                       const Pickup& /*pickup*/, EventSink& /*sink*/) {}

namespace {

constexpr std::array<ModeBehavior, static_cast<size_t>(ModeKind::kCount)> kBehaviors = {
    roundsBehavior(),          // ModeKind::Rounds
    deathmatchBehavior(),      // ModeKind::Deathmatch
    survivalBehavior(),        // ModeKind::Survival
    teamRoundsBehavior(),      // ModeKind::TeamRounds (v0.32)
    teamDeathmatchBehavior(),  // ModeKind::TeamDeathmatch (v0.32)
};

}  // namespace

const ModeBehavior& modeBehaviorFor(ModeKind kind) {
  const auto index = static_cast<size_t>(kind);
  if (index >= kBehaviors.size()) return kBehaviors[0];
  return kBehaviors[index];
}

}  // namespace kfm::sim
