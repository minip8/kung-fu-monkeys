// InteractionSystem (architecture §5.3 step 7, §5.6, §5.11): resolves contacts
// through the prioritised rules table (rules/interaction.hpp).
//
// A Player × Player contact is tried in both orientations (A dashing into B,
// and B into A), so every rule keys on (kindA, kindB) with the actor as A.
// Oriented contacts are resolved in a deterministic order: time of impact
// first; then the earlier dash press of A when A holds the tag its rows key
// on (PerfectDash against a banana or pickup, Dashing against a player; §5.11
// tie-break); then pool index order. For each the rows are scanned by priority
// and the first matching row whose handler returns true wins. Contacts whose
// entities an earlier contact despawned are skipped. Outside
// MatchPhase::Playing no contact resolves: the round is decided (or not yet
// started) and nothing may hit, catch or pick up.
#include "kfm/sim/rules/interaction.hpp"

#include "kfm/sim/systems.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <tuple>

namespace kfm::sim {
namespace {

struct SortKey {
  Fixed tHit{};
  uint64_t pressTime{0};  // (activatedTick << 8) | activatedSubTick; max when not perfect-dashing
  uint8_t kindA{0};
  uint16_t indexA{0};
  uint8_t kindB{0};
  uint16_t indexB{0};
  uint16_t contact{0};   // index into Contacts, the payload
  bool reversed{false};  // a Player × Player contact seen from B

  [[nodiscard]] auto tied() const {
    return std::tie(tHit, pressTime, kindA, indexA, kindB, indexB, contact, reversed);
  }
  friend bool operator<(const SortKey& l, const SortKey& r) { return l.tied() < r.tied(); }
};

[[nodiscard]] uint64_t pressTimeOf(const WorldState& s, const Contact& c) {
  if (c.kindA != EntityKind::Player) return std::numeric_limits<uint64_t>::max();
  const Player* p = s.players.get(c.a);
  const Tag keyed = c.kindB == EntityKind::Player ? Tag::Dashing : Tag::PerfectDash;
  if (p == nullptr || !p->tags.has(keyed)) return std::numeric_limits<uint64_t>::max();
  const AbilityState& dash = p->abilities[kAbilitySlotDash];
  return (static_cast<uint64_t>(dash.activatedTick.value) << 8) | dash.activatedSubTick;
}

[[nodiscard]] bool isAlive(const WorldState& s, EntityKind kind, EntityId id) {
  switch (kind) {
    case EntityKind::Player:
      return s.players.isAlive(id);
    case EntityKind::Boomerang:
      return s.boomerangs.isAlive(id);
    case EntityKind::Pickup:
      return s.pickups.isAlive(id);
    case EntityKind::Hazard:
      return true;  // arena statics never despawn
  }
  return false;
}

[[nodiscard]] bool playerTagsMatch(const WorldState& s, EntityKind kind, EntityId id, Tags require,
                                   Tags forbid) {
  if (kind != EntityKind::Player) return true;
  const Player* p = s.players.get(id);
  return p != nullptr && p->tags.hasAll(require) && p->tags.hasNone(forbid);
}

[[nodiscard]] bool ruleMatches(const WorldState& s, const InteractionRule& rule, const Contact& c) {
  if (rule.kindA != c.kindA || rule.kindB != c.kindB) return false;
  if (!playerTagsMatch(s, c.kindA, c.a, rule.requireA, rule.forbidA)) return false;
  if (!playerTagsMatch(s, c.kindB, c.b, rule.requireB, rule.forbidB)) return false;
  if (c.kindB != EntityKind::Boomerang) return true;
  const Boomerang* b = s.boomerangs.get(c.b);
  if (b == nullptr || !phaseMaskMatches(rule.phaseMaskB, b->phase)) return false;
  if (rule.ownership == Ownership::Any) return true;
  if (c.kindA != EntityKind::Player) return false;
  const Player* p = s.players.get(c.a);
  if (p == nullptr) return false;
  const bool own = b->owner == p->id;
  return rule.ownership == Ownership::Own ? own : !own;
}

}  // namespace

void interactionResolveContact(WorldState& s, const MatchContent& content, const Contact& c,
                               EventSink& sink) {
  for (const InteractionRule& rule : interactionRules()) {
    if (!isAlive(s, c.kindA, c.a) || !isAlive(s, c.kindB, c.b)) return;
    if (!ruleMatches(s, rule, c)) continue;
    if (rule.resolve(s, content, c.a, c.b, c, sink)) return;
  }
}

void interactionSystem(WorldState& s, const MatchContent& content, const Contacts& contacts,
                       EventSink& sink) {
  if (s.rules.phase != MatchPhase::Playing) return;
  // Each Player × Player contact appears twice (both orientations).
  std::array<SortKey, kMaxContacts + kMaxPlayerPairs> order{};
  uint16_t n = 0;
  const auto add = [&](const Contact& c, uint16_t index, bool reversed) {
    order[n++] = SortKey{c.tHit,
                         pressTimeOf(s, c),
                         static_cast<uint8_t>(c.kindA),
                         c.a.index,
                         static_cast<uint8_t>(c.kindB),
                         c.b.index,
                         index,
                         reversed};
  };
  for (uint16_t i = 0; i < contacts.count; ++i) {
    const Contact& c = contacts.items[i];
    add(c, i, false);
    if (c.kindA == EntityKind::Player && c.kindB == EntityKind::Player && n < order.size()) {
      add(reversedContact(c), i, true);
    }
  }
  std::sort(order.begin(), order.begin() + n);

  for (uint16_t k = 0; k < n; ++k) {
    const Contact& c = contacts.items[order[k].contact];
    interactionResolveContact(s, content, order[k].reversed ? reversedContact(c) : c, sink);
  }
}

}  // namespace kfm::sim
