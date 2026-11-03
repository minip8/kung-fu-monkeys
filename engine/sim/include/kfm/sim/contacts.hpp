// Contacts emitted by CollisionSystem and consumed by InteractionSystem
// (architecture §5.3, §5.6). Derived per-tick data: lives in step()'s local
// scope, never in WorldState.
#pragma once

#include "kfm/core/fixed.hpp"
#include "kfm/core/types.hpp"
#include "kfm/core/vec2.hpp"
#include "kfm/sim/constants.hpp"

#include <array>
#include <compare>
#include <cstdint>

namespace kfm::sim {

inline constexpr uint16_t kMaxContacts = 256;
// Unordered player pairs: the Player × Player contacts (v0.16).
inline constexpr uint16_t kMaxPlayerPairs = kMaxPlayers * (kMaxPlayers - 1) / 2;
// Players × (boomerangs + pickups) plus the player pairs bound every
// candidate pair, so the broadphase can never drop one.
inline constexpr uint16_t kMaxCandidatePairs = 1024;
static_assert(uint32_t{kMaxPlayers} * (kMaxBoomerangs + kMaxPickups) + kMaxPlayerPairs <=
                  kMaxCandidatePairs,
              "every candidate pair fits");

// Hazard (v0.22) is not a pool entity: a Contact's EntityId for it carries the
// index into MatchContent::arena.hazards (generation 0). CollisionSystem
// emits Player × Hazard when the player's centre ends the tick inside one.
enum class EntityKind : uint8_t { Player = 0, Boomerang, Pickup, Hazard };

struct Contact {
  EntityKind kindA{EntityKind::Player};
  EntityId a{};
  EntityKind kindB{EntityKind::Player};
  EntityId b{};
  Fixed tHit{};   // time of impact as a fraction of the tick, in [0, 1]; 0 = overlapping at start
  Vec2 point{};   // B's centre at tHit; for Player × Player the midpoint of both centres
  Vec2 normal{};  // unit normal from A toward B at tHit
  // v0.21: kReachA / kReachB when the contact exists only through a slam's
  // widened radius on that side (0 = the bodies touch). Only rules with
  // reachA accept a contact carrying kReachA, and none accept kReachB.
  uint8_t reach{0};
};
inline constexpr uint8_t kReachA = 1;
inline constexpr uint8_t kReachB = 2;

// The same contact seen from B (Player × Player: InteractionSystem tries both
// orientations, §5.6). The midpoint is symmetric, so only the normal flips.
[[nodiscard]] constexpr Contact reversedContact(const Contact& c) {
  return Contact{.kindA = c.kindB,
                 .a = c.b,
                 .kindB = c.kindA,
                 .b = c.a,
                 .tHit = c.tHit,
                 .point = c.point,
                 .normal = -c.normal,
                 .reach = static_cast<uint8_t>(((c.reach & kReachA) != 0 ? kReachB : 0) |
                                               ((c.reach & kReachB) != 0 ? kReachA : 0))};
}

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
