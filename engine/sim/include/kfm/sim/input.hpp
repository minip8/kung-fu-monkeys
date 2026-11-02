// Player input and the per-tick input frame (architecture §5.4). This is
// also the bot and reinforcement-learning action space.
#pragma once

#include "kfm/core/types.hpp"
#include "kfm/sim/constants.hpp"

#include <array>
#include <compare>
#include <cstdint>

namespace kfm::sim {

enum class Button : uint8_t {
  Throw = 1u << 0,
  Dash = 1u << 1,
  Ability1 = 1u << 2,
  Ability2 = 1u << 3,
  Taunt = 1u << 4,
};

[[nodiscard]] constexpr uint8_t buttonBit(Button b) {
  return static_cast<uint8_t>(b);
}

struct PlayerInput {
  uint8_t moveDir{0};  // 0 = idle, otherwise 1/256 turn (Angle::fromInput8)
  uint8_t moveMag{0};  // analog magnitude, 255 for digital
  uint8_t aimDir{0};   // 1/256 turn
  uint8_t buttons{0};  // Button bits
  uint8_t subTick{0};  // fraction of the tick at which the latest button edge occurred

  [[nodiscard]] constexpr bool has(Button b) const { return (buttons & buttonBit(b)) != 0; }

  constexpr auto operator<=>(const PlayerInput&) const = default;
};
static_assert(sizeof(PlayerInput) == 5);

// Bit i set means player slot i is present in the match this tick.
using PlayerMask = uint16_t;
static_assert(kMaxPlayers <= 16);

[[nodiscard]] constexpr bool maskHas(PlayerMask m, PlayerId p) {
  return p.isValid() && p.value < kMaxPlayers && ((static_cast<uint32_t>(m) >> p.value) & 1u) != 0;
}

struct InputFrame {
  Tick tick{};
  std::array<PlayerInput, kMaxPlayers> inputs{};
  PlayerMask present{0};

  // Equality ignores inputs of absent players, matching what the visitor
  // encodes, so a decoded frame compares equal to the one that was sent.
  [[nodiscard]] constexpr bool operator==(const InputFrame& o) const {
    if (tick != o.tick || present != o.present) return false;
    for (uint16_t i = 0; i < kMaxPlayers; ++i) {
      const bool here = ((static_cast<uint32_t>(present) >> i) & 1u) != 0;
      if (here && inputs[i] != o.inputs[i]) return false;
    }
    return true;
  }
};

}  // namespace kfm::sim
