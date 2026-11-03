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
// Buttons belong to slots, not kinds (v0.21): slot kAbilitySlotDash fires on
// Button::Dash, kAbilitySlotThrow on Button::Throw and kAbilitySlotExtra on
// Button::Ability1 (slotButton() in content.hpp), whatever kind the slot runs.
//
// Tags: after all slots ran, the kind tags (kAbilityKindTags) and every phase
// tag granted by the player's slot defs are cleared and re-derived from the
// current phases: def.tagsStartup/Active/Recovery plus what the kind's
// AbilityBehavior::kindTags grants in a non-Idle slot (Dash: PerfectDash while
// its perfect window may be open, abilities/dash.cpp). HitStun and Eliminated
// are never touched.
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

[[nodiscard]] const AbilityDef& defFor(const MatchContent& content, const AbilityState& st) {
  return content.db.abilityAt(st.defIndex);
}

// Advances through every phase whose duration has elapsed (a loop, so several
// zero-length phases are skipped in one tick).
void settle(WorldState& s, Player& p, AbilityState& st, const AbilityDef& def,
            const AbilityBehavior& behavior, const MatchContent& content, EventSink& sink) {
  while (st.phase != AbilityPhase::Idle && st.phaseTicks >= phaseDuration(def, st.phase)) {
    const AbilityPhase from = st.phase;
    const AbilityPhase to = nextPhase(from);
    st.phase = to;
    st.phaseTicks = 0;
    behavior.onPhaseChange(s, p, st, def, from, to, content, sink);
    if (to == AbilityPhase::Idle) behavior.onEnd(s, p, st, def, content, sink);
  }
}

void tickSlot(WorldState& s, Player& p, uint8_t slot, const MatchContent& content,
              EventSink& sink) {
  AbilityState& st = p.abilities[slot];
  const AbilityDef& def = defFor(content, st);
  const AbilityBehavior& behavior = behaviorFor(def.kind);

  if (st.cooldownTicks > 0) --st.cooldownTicks;

  if (st.phase != AbilityPhase::Idle) {
    ++st.phaseTicks;
    settle(s, p, st, def, behavior, content, sink);
  }

  const uint8_t button = slotButton(slot);
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

// Power-up tags (Player::grantedTags, §5.8) belong to ModifierSystem: an
// ability phase that also grants one never clears it.
void rebuildTags(Player& p, const MatchContent& content) {
  Tags owned = kAbilityKindTags;
  for (const AbilityState& st : p.abilities) {
    const AbilityDef& def = defFor(content, st);
    owned = owned | def.tagsStartup | def.tagsActive | def.tagsRecovery;
  }
  owned.bits &= ~(kSystemOwnedTags.bits | p.grantedTags.bits);
  p.tags.bits = (p.tags.bits & ~owned.bits) | abilityPhaseTags(p, content).bits;
}

}  // namespace

Tags slotPhaseTags(const AbilityState& st, const MatchContent& content) {
  const AbilityDef& def = defFor(content, st);
  Tags granted = phaseTags(def, st.phase);
  if (st.phase != AbilityPhase::Idle) granted = granted | behaviorFor(def.kind).kindTags(st, def);
  granted.bits &= ~kSystemOwnedTags.bits;
  return granted;
}

Tags abilityPhaseTags(const Player& p, const MatchContent& content) {
  Tags granted{};
  for (const AbilityState& st : p.abilities) {
    granted = granted | slotPhaseTags(st, content);
  }
  return granted;
}

const AbilityState* slotGranting(const Player& p, const MatchContent& content, Tag tag) {
  for (const AbilityState& st : p.abilities) {
    if (slotPhaseTags(st, content).has(tag)) return &st;
  }
  return nullptr;
}

void abilitySystem(WorldState& s, const MatchContent& content, EventSink& sink) {
  s.players.forEachAlive([&](EntityId /*id*/, Player& p) {
    for (uint8_t slot = 0; slot < kAbilitySlots; ++slot) {
      tickSlot(s, p, slot, content, sink);
    }
    rebuildTags(p, content);
  });
}

}  // namespace kfm::sim
