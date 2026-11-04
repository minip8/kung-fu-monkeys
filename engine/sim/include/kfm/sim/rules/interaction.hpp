// Prioritised interaction rules table (architecture §5.6, Appendix A).
// InteractionSystem resolves each contact by scanning the rows in priority
// order; the first row that matches and whose handler returns true wins. A
// handler returning false means "did not apply, keep scanning" (how a
// mistimed perfect catch falls through to damage).
#pragma once

#include "kfm/sim/contacts.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/tags.hpp"
#include "kfm/sim/world_state.hpp"

#include <cstdint>
#include <span>

namespace kfm::sim {

// Bits over BoomerangPhase for InteractionRule::phaseMaskB.
[[nodiscard]] constexpr uint8_t boomerangPhaseBit(BoomerangPhase p) {
  return static_cast<uint8_t>(1u << static_cast<uint8_t>(p));
}
inline constexpr uint8_t kPhaseAny = 0;  // no phase constraint
inline constexpr uint8_t kPhaseInFlight =
    boomerangPhaseBit(BoomerangPhase::Outbound) | boomerangPhaseBit(BoomerangPhase::Returning);
inline constexpr uint8_t kPhaseReturning = boomerangPhaseBit(BoomerangPhase::Returning);

[[nodiscard]] constexpr bool phaseMaskMatches(uint8_t mask, BoomerangPhase p) {
  return mask == kPhaseAny || (mask & boomerangPhaseBit(p)) != 0;
}

// Bits over PickupKind for InteractionRule::pickupMaskB.
[[nodiscard]] constexpr uint8_t pickupKindBit(PickupKind k) {
  return static_cast<uint8_t>(1u << static_cast<uint8_t>(k));
}
inline constexpr uint8_t kPickupAny = 0;  // no pickup-kind constraint

[[nodiscard]] constexpr bool pickupMaskMatches(uint8_t mask, PickupKind k) {
  return mask == kPickupAny || (mask & pickupKindBit(k)) != 0;
}

// Relation between the player A and the owner of boomerang B.
enum class Ownership : uint8_t { Any = 0, Own, NotOwn };

// a/b are the contact's entities in the rule's (kindA, kindB) order.
using InteractionResolveFn = bool (*)(WorldState& s, const MatchContent& content, EntityId a,
                                      EntityId b, const Contact& contact, EventSink& sink);

struct InteractionRule {
  uint8_t priority{0};
  EntityKind kindA{EntityKind::Player};
  Tags requireA{};  // player tags (when A is a player)
  Tags forbidA{};
  EntityKind kindB{EntityKind::Player};
  Tags requireB{};  // player tags (when B is a player)
  Tags forbidB{};
  uint8_t phaseMaskB{kPhaseAny};  // when B is a boomerang: allowed BoomerangPhase bits
  Ownership ownership{Ownership::Any};
  InteractionResolveFn resolve{nullptr};
  uint8_t pickupMaskB{kPickupAny};  // when B is a pickup: allowed PickupKind bits
  // v0.21: 1 = also matches a contact made only through A's slam reach
  // (Contact::reach has kReachA); every other row needs the bodies to touch.
  uint8_t reachA{0};
  // v0.32: allowed relationBit(Relation) bits of B to A, judged through
  // MatchContent::relation between A and B (a player) or B's owner (a
  // banana); pickups and hazards have no relation and match any mask. Every
  // bit by default. A free-for-all makes every other slot an enemy.
  uint8_t relationB{kRelationAny};
};

// The M1 rows, sorted by ascending priority.
[[nodiscard]] std::span<const InteractionRule> interactionRules();

}  // namespace kfm::sim
