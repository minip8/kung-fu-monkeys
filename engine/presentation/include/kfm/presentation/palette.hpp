// Player colours (architecture §7): one per slot, a material parameter rather
// than a texture variant. Linear-ish RGB in [0, 1]; RenderPlayer::colorIndex
// is the slot. Chosen for mutual distinctness at a glance on a mid-grey floor:
//
//   0 red      1 blue     2 yellow   3 green    4 orange
//   5 purple   6 cyan     7 pink     8 white    9 charcoal
//
// Slots 0..3 (the usual local and small online matches) use the four most
// separable hues.
#pragma once

#include "kfm/sim/constants.hpp"

#include <array>
#include <cstdint>

namespace kfm::presentation {

struct Color {
  float r{0.0f};
  float g{0.0f};
  float b{0.0f};
};

inline constexpr std::array<Color, sim::kMaxPlayers> kPlayerColors = {{
    {0.90f, 0.18f, 0.18f},  // 0 red
    {0.18f, 0.42f, 0.95f},  // 1 blue
    {0.98f, 0.84f, 0.12f},  // 2 yellow
    {0.16f, 0.72f, 0.28f},  // 3 green
    {1.00f, 0.52f, 0.08f},  // 4 orange
    {0.58f, 0.28f, 0.86f},  // 5 purple
    {0.10f, 0.84f, 0.88f},  // 6 cyan
    {0.98f, 0.46f, 0.74f},  // 7 pink
    {0.94f, 0.94f, 0.94f},  // 8 white
    {0.22f, 0.22f, 0.25f},  // 9 charcoal
}};

// Colour for a slot; out-of-range slots wrap.
[[nodiscard]] constexpr Color playerColor(uint32_t slot) {
  return kPlayerColors[slot % kPlayerColors.size()];
}

}  // namespace kfm::presentation
