// Simulation-wide constants (architecture §5).
#pragma once

#include <cstdint>

namespace kfm::sim {

inline constexpr uint32_t kTickHz = 60;
inline constexpr uint16_t kMaxPlayers = 10;
inline constexpr uint16_t kMaxBoomerangs = 64;
inline constexpr uint16_t kMaxPickups = 32;
inline constexpr uint16_t kMaxEventsPerTick = 256;
// Ability slots per player (§5.5): dash button, throw button and (v0.21) the
// ability button. The base kit leaves the third slot on the inert None def.
inline constexpr uint8_t kAbilitySlots = 3;

}  // namespace kfm::sim
