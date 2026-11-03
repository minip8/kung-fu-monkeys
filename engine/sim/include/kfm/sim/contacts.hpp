// Contacts emitted by CollisionSystem and consumed by InteractionSystem
// (architecture §5.3, §5.6). Derived per-tick data: lives in step()'s local
// scope, never in WorldState.
#pragma once

#include "kfm/core/fixed.hpp"
#include "kfm/core/types.hpp"
#include "kfm/core/vec2.hpp"

#include <array>
#include <compare>
#include <cstdint>

namespace kfm::sim {

inline constexpr uint16_t kMaxContacts = 256;
// Players × (boomerangs + pickups) bounds every M1 candidate pair, so the
// broadphase can never drop one.
inline constexpr uint16_t kMaxCandidatePairs = 1024;

enum class EntityKind : uint8_t { Player = 0, Boomerang, Pickup };

struct Contact {
  EntityKind kindA{EntityKind::Player};
  EntityId a{};
  EntityKind kindB{EntityKind::Player};
  EntityId b{};
  Fixed tHit{};   // time of impact as a fraction of the tick, in [0, 1]; 0 = overlapping at start
  Vec2 point{};   // contact point at tHit
  Vec2 normal{};  // unit normal from A toward B at tHit
};

struct Contacts {
  std::array<Contact, kMaxContacts> items{};
  uint16_t count{0};
  uint16_t dropped{0};

  constexpr void clear() {
    count = 0;
    dropped = 0;
  }
  constexpr void push(const Contact& c) {
    if (count < kMaxContacts) {
      items[count++] = c;
    } else {
      ++dropped;
    }
  }
  [[nodiscard]] constexpr const Contact* begin() const { return items.data(); }
  [[nodiscard]] constexpr const Contact* end() const { return items.data() + count; }
};

// Broadphase output: a candidate pair by pool index, kindA <= kindB. The two
// broadphases (grid and brute force) must produce identical, sorted,
// deduplicated lists ordered by (kindA, indexA, kindB, indexB).
struct CandidatePair {
  EntityKind kindA{EntityKind::Player};
  uint16_t indexA{0};
  EntityKind kindB{EntityKind::Player};
  uint16_t indexB{0};

  constexpr auto operator<=>(const CandidatePair&) const = default;
};

struct CandidatePairs {
  std::array<CandidatePair, kMaxCandidatePairs> items{};
  uint16_t count{0};
  uint16_t dropped{0};

  constexpr void clear() {
    count = 0;
    dropped = 0;
  }
  constexpr void push(const CandidatePair& p) {
    if (count < kMaxCandidatePairs) {
      items[count++] = p;
    } else {
      ++dropped;
    }
  }
  [[nodiscard]] constexpr const CandidatePair* begin() const { return items.data(); }
  [[nodiscard]] constexpr const CandidatePair* end() const { return items.data() + count; }
};

}  // namespace kfm::sim
