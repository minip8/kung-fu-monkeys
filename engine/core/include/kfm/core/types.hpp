// Strong typedefs shared by every layer. Never pass raw integers where one
// of these is expected (CLAUDE.md, working conventions).
#pragma once

#include <compare>
#include <cstdint>

namespace kfm {

// A value type that is distinct from every other instantiation. Equality
// and ordering are defaulted; arithmetic is only provided where it makes
// sense for the specific alias below.
template <class Tag, class Rep>
struct Strong {
  using rep = Rep;
  Rep value{};

  constexpr auto operator<=>(const Strong&) const = default;
};

// Simulation tick counter. Ticks are sequential, so a few arithmetic
// helpers are provided; the difference between ticks is a plain integer.
struct Tick {
  using rep = uint32_t;
  rep value{};

  constexpr auto operator<=>(const Tick&) const = default;

  constexpr Tick& operator++() {
    ++value;
    return *this;
  }
  constexpr Tick operator++(int) {
    const Tick old = *this;
    ++value;
    return old;
  }
  [[nodiscard]] constexpr Tick plus(int32_t delta) const {
    return Tick{static_cast<rep>(static_cast<int64_t>(value) + delta)};
  }
  // Signed distance from `other` to this tick.
  [[nodiscard]] constexpr int32_t minus(Tick other) const {
    return static_cast<int32_t>(static_cast<int64_t>(value) - static_cast<int64_t>(other.value));
  }
};

// Index of a player slot in a match (0..kMaxPlayers-1).
struct PlayerId {
  using rep = uint8_t;
  rep value{};

  static constexpr rep kInvalidValue = 0xFF;
  [[nodiscard]] static constexpr PlayerId invalid() { return PlayerId{kInvalidValue}; }
  [[nodiscard]] constexpr bool isValid() const { return value != kInvalidValue; }

  constexpr auto operator<=>(const PlayerId&) const = default;
};

// Generational handle into a Pool. index selects the slot; generation
// detects stale handles after the slot has been reused.
struct EntityId {
  uint16_t index{kInvalidIndex};
  uint16_t generation{0};

  static constexpr uint16_t kInvalidIndex = 0xFFFF;
  [[nodiscard]] static constexpr EntityId invalid() { return EntityId{}; }
  [[nodiscard]] constexpr bool isValid() const { return index != kInvalidIndex; }

  constexpr auto operator<=>(const EntityId&) const = default;
};

}  // namespace kfm
