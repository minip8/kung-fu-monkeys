// ModeBehavior registry (architecture §5.3, v0.23): one constexpr row per
// ModeKind, in enum order, and the shared no-op hooks (v0.31).
#include "kfm/sim/modes/registry.hpp"

#include "kfm/core/assert.hpp"
#include "kfm/core/field_count.hpp"
#include "kfm/sim/modes/modes.hpp"

#include <algorithm>
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
    hillBehavior(),            // ModeKind::Hill (v0.33)
    captureBehavior(),         // ModeKind::Capture (v0.33)
    controlBehavior(),         // ModeKind::Control (v0.80)
};

// Every hook is called unconditionally: a row missing from the list above (a
// new ModeKind) or a behaviour that leaves a hook unset is a null call at the
// first dispatch, so it is a compile error instead.
constexpr bool rowComplete(const ModeBehavior& b) {
  return b.onRoundStart != nullptr && b.onPlaying != nullptr && b.onEliminated != nullptr &&
         b.onPickupTaken != nullptr;
}
static_assert(fieldCount<ModeBehavior>() == 4, "check the new hook in rowComplete");
KFM_STATIC_ASSERT_HOOKS(std::ranges::all_of(kBehaviors, rowComplete),
                        "every ModeKind needs a row with every hook set");

}  // namespace

const ModeBehavior& modeBehaviorFor(ModeKind kind) {
  const auto index = static_cast<size_t>(kind);
  if (index >= kBehaviors.size()) return kBehaviors[0];
  return kBehaviors[index];
}

}  // namespace kfm::sim
