// AbilityBehavior table indexed by AbilityKind (architecture §5.5,
// Appendix A). The table is constexpr: no startup registration and no
// mutable static state. New ability = behaviour file + content def + one
// line in src/abilities/registry.cpp + a scenario test.
#pragma once

#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/world_state.hpp"

namespace kfm::sim {

// Hooks called by AbilitySystem for one ability slot. `p` is the owning
// player (an element of s.players) and `st` one of p.abilities. Every
// pointer is non-null; behaviours that do not need a hook use a no-op.
struct AbilityBehavior {
  // Extra activation condition on top of "idle, cooldown 0, button pressed".
  bool (*canActivate)(const WorldState& s, const Player& p, const AbilityDef& def);
  // Called once on activation, after AbilitySystem recorded activatedTick,
  // activatedSubTick and reset the phase.
  void (*onActivate)(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                     const MatchContent& content, EventSink& sink);
  // Called every tick the slot is not Idle, after the phase advanced.
  void (*onTick)(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                 const MatchContent& content, EventSink& sink);
  // Called on every phase transition, including skipped zero-length phases.
  void (*onPhaseChange)(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                        AbilityPhase from, AbilityPhase to, const MatchContent& content,
                        EventSink& sink);
  // Called when the slot returns to Idle.
  void (*onEnd)(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
                const MatchContent& content, EventSink& sink);
  // Tags the kind itself grants in the slot's current state, on top of the
  // def's phase tags (Dash: PerfectDash while the perfect window may be open).
  // Pure: AbilitySystem re-derives every tick from the state alone.
  Tags (*kindTags)(const AbilityState& st, const AbilityDef& def);
};

// kindTags for a kind that grants nothing beyond its def's phase tags.
[[nodiscard]] constexpr Tags noKindTags(const AbilityState& /*st*/, const AbilityDef& /*def*/) {
  return Tags{};
}

// Every tag an AbilityBehavior::kindTags may return. AbilitySystem owns these
// bits (it clears and re-derives them each tick) like the defs' phase tags.
inline constexpr Tags kAbilityKindTags = Tags::of(Tag::Dashing) | Tag::PerfectDash | Tag::Slamming;

// Behaviour for a kind; out-of-range kinds get the inert None behaviour.
[[nodiscard]] const AbilityBehavior& behaviorFor(AbilityKind kind);

}  // namespace kfm::sim
