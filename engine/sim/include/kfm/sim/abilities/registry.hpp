// AbilityBehavior table indexed by AbilityKind (architecture §5.5,
// Appendix A). The table is constexpr: no startup registration and no
// mutable static state. New ability = behaviour file + content def + one
// line in src/abilities/registry.cpp + a scenario test.
#pragma once

#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/world_state.hpp"

namespace kfm::sim {

// A holdsPhase for a kind whose phases always run their def's durations.
[[nodiscard]] constexpr bool neverHolds(const AbilityState& /*st*/, const AbilityDef& /*def*/,
                                        bool /*buttonHeld*/) {
  return false;
}

// An onIdleHold for a kind that does nothing while its button is held idle.
// NOLINTNEXTLINE(misc-const-correctness): parameter types fixed by AbilityBehavior
constexpr void noIdleHold(WorldState& /*s*/, Player& /*p*/, AbilityState& /*st*/,
                          const AbilityDef& /*def*/, const MatchContent& /*content*/,
                          EventSink& /*sink*/) {}

// Hooks called by AbilitySystem for one ability slot. `p` is the owning
// player (an element of s.players) and `st` one of p.abilities. Every
// pointer is non-null in the registry; behaviours that do not need a hook
// use a no-op (the nullptr defaults only keep the aggregate initialised).
struct AbilityBehavior {
  // Extra activation condition on top of "idle, cooldown 0, button pressed".
  bool (*canActivate)(const WorldState& s, const Player& p, const AbilityDef& def) = nullptr;
  // Called once on activation, after AbilitySystem recorded activatedTick,
  // activatedSubTick and reset the phase.
  void (*onActivate)(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                     const MatchContent& content, EventSink& sink) = nullptr;
  // Called every tick the slot is not Idle, after the phase advanced.
  void (*onTick)(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                 const MatchContent& content, EventSink& sink) = nullptr;
  // Called on every phase transition, including skipped zero-length phases.
  void (*onPhaseChange)(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                        AbilityPhase from, AbilityPhase to, const MatchContent& content,
                        EventSink& sink) = nullptr;
  // Called when the slot returns to Idle.
  void (*onEnd)(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                const MatchContent& content, EventSink& sink) = nullptr;
  // Tags the kind itself grants in the slot's current state, on top of the
  // def's phase tags (Dash: PerfectDash while the perfect window may be open).
  // Pure: AbilitySystem re-derives every tick from the state alone.
  Tags (*kindTags)(const AbilityState& st, const AbilityDef& def) = nullptr;
  // v0.48: true keeps the current phase going past its def's duration
  // (AbilitySystem's settle does not advance it this tick). `buttonHeld` is
  // the slot's button in the player's intent (Throw: a charging wind-up runs
  // while the button is held). Pure, like kindTags.
  bool (*holdsPhase)(const AbilityState& st, const AbilityDef& def, bool buttonHeld) = &neverHolds;
  // v0.89: called on every Playing tick the slot ends Idle with its button
  // held (after the activation check, so also on a press canActivate
  // refused). Throw: the recall of a banana lying on the floor
  // (abilities/throw.hpp). Every other kind: noIdleHold.
  void (*onIdleHold)(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                     const MatchContent& content, EventSink& sink) = &noIdleHold;
};

// kindTags for a kind that grants nothing beyond its def's phase tags.
[[nodiscard]] constexpr Tags noKindTags(const AbilityState& /*st*/, const AbilityDef& /*def*/) {
  return Tags{};
}

// Every tag an AbilityBehavior::kindTags may return. AbilitySystem owns these
// bits (it clears and re-derives them each tick) like the defs' phase tags.
inline constexpr Tags kAbilityKindTags =
    Tags::of(Tag::Dashing) | Tag::PerfectDash | Tag::Slamming | Tag::Parrying;

// Behaviour for a kind; out-of-range kinds get the inert None behaviour.
[[nodiscard]] const AbilityBehavior& behaviorFor(AbilityKind kind);

}  // namespace kfm::sim
