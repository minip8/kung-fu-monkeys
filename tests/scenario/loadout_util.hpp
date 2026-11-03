// Helpers for the v0.21 ability scenarios: content with an extra ability in
// the third slot (or a mode loadout), and a Playing world for any
// MatchContent. The defs mirror content/abilities.toml.
#pragma once

#include "scenario_util.hpp"

#include <string_view>

namespace kfm::sim::test {

// Units per second -> units per tick, as the loader converts speeds.
[[nodiscard]] inline Fixed perSecond(int32_t unitsPerSecond) {
  return Fixed::fromRatio(unitsPerSecond, static_cast<int32_t>(kTickHz));
}

[[nodiscard]] inline AbilityDef blinkDef() {
  AbilityDef d;
  d.name = ContentName::of("blink");
  d.kind = AbilityKind::Blink;
  d.cooldownTicks = 90;
  d.startupTicks = 6;
  d.activeTicks = 10;
  d.recoveryTicks = 6;
  d.speed = perSecond(30);  // 0.5 units per active tick: a 5-unit jump
  return d;
}

[[nodiscard]] inline AbilityDef slamDef() {
  AbilityDef d;
  d.name = ContentName::of("slam");
  d.kind = AbilityKind::Slam;
  d.cooldownTicks = 120;
  d.startupTicks = 12;
  d.activeTicks = 12;
  d.recoveryTicks = 12;
  d.param0 = Fixed::fromInt(3);  // reach
  d.param1 = Fixed::fromInt(4);  // push
  return d;
}

[[nodiscard]] inline AbilityDef parryDef() {
  AbilityDef d;
  d.name = ContentName::of("parry");
  d.kind = AbilityKind::Parry;
  d.cooldownTicks = 60;
  d.startupTicks = 2;
  d.activeTicks = 8;
  d.recoveryTicks = 14;
  return d;
}

// Appends `def` to a copy of `base`; returns its index through `index`.
[[nodiscard]] inline ContentDB withAbility(const AbilityDef& def, uint8_t& index,
                                           const ContentDB& base = defaultContent()) {
  ContentDB c = base;
  REQUIRE(c.abilityCount < kMaxAbilityDefs);
  index = c.abilityCount;
  c.abilities[c.abilityCount++] = def;
  return c;
}

// The default content with `def` in the third slot (the ability button).
[[nodiscard]] inline ContentDB withExtraSlot(const AbilityDef& def) {
  uint8_t index = 0;
  ContentDB c = withAbility(def, index);
  c.player.abilities[kAbilitySlotExtra] = index;
  return c;
}

// Steps idle frames until the match is Playing (makePlayingWorld for any
// MatchContent, e.g. a mode with its own loadout).
[[nodiscard]] inline WorldState playingWorld(uint64_t seed, PlayerMask players,
                                             const MatchContent& content) {
  WorldState s = initialWorld(MatchConfig{seed, players});
  EventLog log;
  for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i) {
    runTicks(s, frameFor(s), 1, content, log);
  }
  REQUIRE(s.rules.phase == MatchPhase::Playing);
  return s;
}

// One tick with `buttons` pressed by `who` (others idle) and aim `aim`.
inline void pressOnce(WorldState& s, PlayerId who, uint8_t buttons, const MatchContent& content,
                      EventLog& log, uint8_t aim = 0, uint8_t subTick = 0) {
  InputFrame f = frameFor(s);
  f.inputs[who.value].aimDir = aim;
  f.inputs[who.value].buttons = buttons;
  f.inputs[who.value].subTick = subTick;
  runTicks(s, f, 1, content, log);
}

// n idle ticks (everyone keeps aim `aim`).
inline void idle(WorldState& s, int n, const MatchContent& content, EventLog& log,
                 uint8_t aim = 0) {
  InputFrame f = frameFor(s);
  for (PlayerInput& in : f.inputs)
    in.aimDir = aim;
  runTicks(s, f, n, content, log);
}

}  // namespace kfm::sim::test
