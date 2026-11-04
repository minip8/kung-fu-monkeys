// Loadouts (architecture §5.10, v0.29, as built v0.31): a named set of
// ability defs, one per slot (dash button, throw button, ability button), a
// match axis of its own beside the arena and the mode. MatchConfig::loadout
// picks one by index; index 0 is the player's base kit ([player] abilities,
// named "base"), the [[loadout]] tables of content/loadouts.toml follow in
// load order. Team modes will hand them out per position as roles (step 2).
#pragma once

#include "kfm/sim/constants.hpp"
#include "kfm/sim/content/name.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace kfm::sim {

inline constexpr uint8_t kMaxLoadouts = 16;
// The name of loadout 0, the [player] abilities.
inline constexpr std::string_view kBaseLoadoutName = "base";

struct LoadoutDef {
  ContentName name{};
  // Def index per slot (into ContentDB::abilities; 0 = None). The dash and
  // throw slots always hold an ability (validated), the third may be None.
  std::array<uint8_t, kAbilitySlots> abilities{};
};

}  // namespace kfm::sim
