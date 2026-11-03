// AbilitySystem (architecture §5.3 step 3, §5.5): activation, phase state machines,
// cooldowns and phase tags, dispatching to the AbilityBehavior table.
//
// Per player, per slot in slot order, each tick:
//   1. cooldownTicks > 0  -> cooldownTicks--.
//   2. phase != Idle      -> phaseTicks++, then settle (below).
//   3. phase == Idle, the slot's button edge is in intent.pressed, cooldown 0,
//      match phase Playing and behavior.canActivate -> activate: cooldownTicks =
//      def.cooldownTicks, activatedTick/SubTick recorded, phase Startup,
//      phaseTicks 0, onActivate, then settle.
//   4. phase != Idle      -> onTick (after the phase advanced, so a Startup of 0
//      makes the activation tick the first Active tick).
// settle: while the current phase has run for its full duration, move to the
// next phase (Startup -> Active -> Recovery -> Idle) with phaseTicks 0, calling
// onPhaseChange for each transition and onEnd on reaching Idle. Zero-length
// phases are thereby skipped within the same tick. Activation itself (Idle ->
// Startup) is reported through onActivate, not onPhaseChange.
//
// Timeline consequences (phase durations S/A/R, press on tick T):
//   Startup on ticks [T, T+S), Active on [T+S, T+S+A), Recovery on
//   [T+S+A, T+S+A+R), Idle again from T+S+A+R. A cooldown C allows the next
//   activation on tick T+C at the earliest.
//
// Tags: after all slots ran, Dashing/PerfectDash and every phase tag granted by
// the player's slot defs are cleared and re-derived from the current phases
// (def.tagsStartup/Active/Recovery). PerfectDash marks the ticks on which the
// perfect window [press, press + perfectWindowTicks) is open: the Dash slot is
// Active and phaseTicks < perfectWindowTicks, plus tick press + window itself
// when the press had a non-zero sub-tick (the window closes inside that tick;
// the catch rule judges the exact instant, §5.11). HitStun and Eliminated are
// never touched.
#include "kfm/sim/abilities/registry.hpp"
#include "kfm/sim/systems.hpp"

namespace kfm::sim {
namespace {

constexpr Tags kSystemOwnedTags = Tags::of(Tag::HitStun) | Tag::Eliminated;

[[nodiscard]] constexpr uint16_t phaseDuration(const AbilityDef& def, AbilityPhase phase) {
  switch (phase) {
    case AbilityPhase::Startup:
      return def.startupTicks;
    case AbilityPhase::Active:
      return def.activeTicks;
    case AbilityPhase::Recovery:
      return def.recoveryTicks;
    case AbilityPhase::Idle:
      break;
  }
  return 0;
}

[[nodiscard]] constexpr AbilityPhase nextPhase(AbilityPhase phase) {
  switch (phase) {
    case AbilityPhase::Startup:
      return AbilityPhase::Active;
    case AbilityPhase::Active:
      return AbilityPhase::Recovery;
    case AbilityPhase::Recovery:
    case AbilityPhase::Idle:
      break;
  }
  return AbilityPhase::Idle;
}

[[nodiscard]] constexpr Tags phaseTags(const AbilityDef& def, AbilityPhase phase) {
  switch (phase) {
    case AbilityPhase::Startup:
      return def.tagsStartup;
    case AbilityPhase::Active:
      return def.tagsActive;
    case AbilityPhase::Recovery:
      return def.tagsRecovery;
    case AbilityPhase::Idle:
      break;
  }
  return Tags{};
}

// The button whose press edge activates an ability of this kind.
[[nodiscard]] constexpr uint8_t activationButton(AbilityKind kind) {
  switch (kind) {
    case AbilityKind::Dash:
      return buttonBit(Button::Dash);
    case AbilityKind::Throw:
      return buttonBit(Button::Throw);
    case AbilityKind::None:
    case AbilityKind::kCount:
      break;
  }
  return 0;
}

[[nodiscard]] const AbilityDef& defFor(const ContentDB& content, const AbilityState& st) {
  // An out-of-range index falls back to the inert None def (index 0).
  return st.defIndex < content.abilities.size() ? content.abilities[st.defIndex]
                                                : content.abilities[0];
}

// Advances through every phase whose duration has elapsed (a loop, so several
// zero-length phases are skipped in one tick).
void settle(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
            const AbilityBehavior& behavior, const ContentDB& content, EventSink& sink) {
  while (st.phase != AbilityPhase::Idle && st.phaseTicks >= phaseDuration(def, st.phase)) {
    const AbilityPhase from = st.phase;
    const AbilityPhase to = nextPhase(from);
    st.phase = to;
    st.phaseTicks = 0;
    behavior.onPhaseChange(s, p, st, def, from, to, content, sink);
    if (to == AbilityPhase::Idle) behavior.onEnd(s, p, st, def, content, sink);
  }
}

void tickSlot(WorldState& s, Player& p, AbilityState& st, const ContentDB& content,
              EventSink& sink) {
  const AbilityDef& def = defFor(content, st);
  const AbilityBehavior& behavior = behaviorFor(def.kind);

  if (st.cooldownTicks > 0) --st.cooldownTicks;

  if (st.phase != AbilityPhase::Idle) {
    ++st.phaseTicks;
    settle(s, p, st, def, behavior, content, sink);
  }

  const uint8_t button = activationButton(def.kind);
  if (st.phase == AbilityPhase::Idle && button != 0 && (p.intent.pressed & button) != 0 &&
      st.cooldownTicks == 0 && s.rules.phase == MatchPhase::Playing &&
      behavior.canActivate(s, p, def)) {
    st.cooldownTicks = def.cooldownTicks;
    st.activatedTick = s.tick;
    st.activatedSubTick = p.intent.subTick;
    st.phase = AbilityPhase::Startup;
    st.phaseTicks = 0;
    behavior.onActivate(s, p, st, def, content, sink);
    settle(s, p, st, def, behavior, content, sink);
  }

  if (st.phase != AbilityPhase::Idle) behavior.onTick(s, p, st, def, content, sink);
}

void rebuildTags(Player& p, const ContentDB& content) {
  Tags owned = Tags::of(Tag::Dashing) | Tag::PerfectDash;
  Tags granted{};
  for (const AbilityState& st : p.abilities) {
    const AbilityDef& def = defFor(content, st);
    owned = owned | def.tagsStartup | def.tagsActive | def.tagsRecovery;
    granted = granted | phaseTags(def, st.phase);
    const bool windowOpen = st.phaseTicks < def.perfectWindowTicks ||
                            (st.phaseTicks == def.perfectWindowTicks && st.activatedSubTick != 0);
    if (def.kind == AbilityKind::Dash && st.phase == AbilityPhase::Active && windowOpen) {
      granted.set(Tag::PerfectDash);
    }
  }
  owned.bits &= ~kSystemOwnedTags.bits;
  granted.bits &= ~kSystemOwnedTags.bits;
  p.tags.bits = (p.tags.bits & ~owned.bits) | granted.bits;
}

}  // namespace

void abilitySystem(WorldState& s, const ContentDB& content, EventSink& sink) {
  s.players.forEachAlive([&](EntityId /*id*/, Player& p) {
    for (AbilityState& st : p.abilities) {
      tickSlot(s, p, st, content, sink);
    }
    rebuildTags(p, content);
  });
}

}  // namespace kfm::sim
