// Simulation-wide constants (architecture §5).
#pragma once

#include <cstdint>

namespace kfm::sim {

inline constexpr uint32_t kTickHz = 60;
inline constexpr uint16_t kMaxPlayers = 10;
inline constexpr uint16_t kMaxBoomerangs = 64;
inline constexpr uint16_t kMaxPickups = 32;
inline constexpr uint16_t kMaxHazards = 16;
inline constexpr uint16_t kMaxEventsPerTick = 256;

}  // namespace kfm::sim
