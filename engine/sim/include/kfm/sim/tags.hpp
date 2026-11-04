// Gameplay tags: a small bitset read by the interaction rules and granted by
// ability phases, power-ups and timers (architecture §5.5, §5.6).
#pragma once

#include <cstdint>

namespace kfm::sim {

enum class Tag : uint8_t {
  Dashing,
  PerfectDash,
  HitStun,
  Shielded,
  Invulnerable,
  Eliminated,
  Slamming,  // v0.21: a slam's impact tick (abilities/slam.cpp, rules/slam.cpp)
  Parrying,  // v0.21: a parry's window may be open (abilities/parry.cpp, rules/parry.cpp)
  // v0.32: allies' bananas, dash attacks and slam pushes reach this monkey
  // (the Ally rows of the rules table, rules/table.cpp). A mode grants it
  // through its baseline power-up; nothing else sets it.
  FriendlyFire,
  kCount,  // NOLINT(readability-identifier-naming): count sentinel
};
static_assert(static_cast<uint32_t>(Tag::kCount) <= 32);

[[nodiscard]] constexpr uint32_t tagBit(Tag t) {
  return uint32_t{1} << static_cast<uint32_t>(t);
}

// Visited as one u32 (visit.hpp).
struct Tags {
  uint32_t bits{0};

  [[nodiscard]] static constexpr Tags of(Tag t) { return Tags{tagBit(t)}; }

  [[nodiscard]] constexpr bool has(Tag t) const { return (bits & tagBit(t)) != 0; }
  constexpr void set(Tag t) { bits |= tagBit(t); }
  constexpr void clear(Tag t) { bits &= ~tagBit(t); }
  // Every tag in `required` is set (true for an empty set).
  [[nodiscard]] constexpr bool hasAll(Tags required) const {
    return (bits & required.bits) == required.bits;
  }
  // No tag in `forbidden` is set (true for an empty set).
  [[nodiscard]] constexpr bool hasNone(Tags forbidden) const {
    return (bits & forbidden.bits) == 0;
  }

  friend constexpr Tags operator|(Tags a, Tags b) { return Tags{a.bits | b.bits}; }
  friend constexpr Tags operator|(Tags a, Tag b) { return Tags{a.bits | tagBit(b)}; }

  constexpr bool operator==(const Tags&) const = default;
};

}  // namespace kfm::sim
