// Immutable content definitions (architecture §5.5, §5.7, §5.10). The
// source of truth is the TOML under content/, parsed and validated by
// engine/content into one ContentDB and hashed through the visitor;
// defaultContent() is the same data as constexpr tables, the headless
// fixture of the sim tests. Systems read every tuning number from here,
// never from literals of their own, and receive it as a MatchContent: the
// DB plus the arena, mode and loadout the match selected.
//
// Units: lengths in arena units, speeds in units per tick, durations in ticks.
#pragma once

#include "kfm/core/fixed.hpp"
#include "kfm/core/vec2.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/content/arena.hpp"  // ArenaDef, Wall, Hazard
#include "kfm/sim/content/loadout.hpp"
#include "kfm/sim/content/mode.hpp"
#include "kfm/sim/content/name.hpp"
#include "kfm/sim/content/powerup.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/tags.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace kfm::sim {

inline constexpr uint8_t kAbilitySlotDash = 0;
inline constexpr uint8_t kAbilitySlotThrow = 1;
inline constexpr uint8_t kAbilitySlotExtra = 2;  // v0.21: Button::Ability1, None in the base kit
static_assert(kAbilitySlots == 3);

// The button whose press edge fires an ability slot (v0.21: buttons belong to
// slots, whatever kind the slot runs).
[[nodiscard]] constexpr uint8_t slotButton(uint8_t slot) {
  switch (slot) {
    case kAbilitySlotDash:
      return buttonBit(Button::Dash);
    case kAbilitySlotThrow:
      return buttonBit(Button::Throw);
    case kAbilitySlotExtra:
      return buttonBit(Button::Ability1);
    default:
      break;
  }
  return 0;
}
inline constexpr uint8_t kMaxAbilityDefs = 16;
inline constexpr uint8_t kMaxArenas = 8;
inline constexpr uint8_t kMaxModes = 32;  // v0.31 (8 before)

enum class AbilityKind : uint8_t {
  None = 0,
  Dash,
  Throw,
  Blink,   // v0.21: teleport dash (abilities/blink.cpp)
  Slam,    // v0.21: radial impulse (abilities/slam.cpp, rules/slam.cpp)
  Parry,   // v0.21: timed banana reflection (abilities/parry.cpp, rules/parry.cpp)
  kCount,  // NOLINT(readability-identifier-naming): table-size sentinel, not a kind
};
enum class AbilityPhase : uint8_t { Idle = 0, Startup, Active, Recovery };

struct AbilityDef {
  ContentName name{};  // the loadout and other defs refer to it by name in TOML
  AbilityKind kind{AbilityKind::None};
  uint16_t cooldownTicks{0};  // counted from activation
  uint16_t startupTicks{0};
  uint16_t activeTicks{0};
  uint16_t recoveryTicks{0};
  uint16_t perfectWindowTicks{0};  // Dash: first N active ticks grant Tag::PerfectDash
  Fixed speed{};   // Dash: dash speed; Blink: distance per active tick (units/tick). Throw: unused
  Fixed param0{};  // kind-specific numeric params. Slam: reach (radius from its centre)
  Fixed param1{};  // Slam: push distance, spread over activeTicks
  Tags tagsStartup{};  // tags granted during each phase
  Tags tagsActive{};
  Tags tagsRecovery{};
};

struct BoomerangDef {
  Fixed radius{};
  Fixed speed{};               // units/tick, constant magnitude in flight
  uint16_t outboundTicks{0};   // then Returning
  uint16_t maxReturnTicks{0};  // Returning longer than this -> Grounded (no infinite orbit)
  uint16_t turnRateBam{0};     // max heading change per tick while returning (BAM units)
  Fixed restitution{};         // wall bounce velocity scale (1 = elastic)
  Fixed spawnOffset{};         // distance ahead of the thrower where it spawns
  uint8_t damage{1};           // per hit; the banana flies on through its victim
};

// Pickups lying in the arena (M1: grounded bananas; M4 adds power-ups).
struct PickupDef {
  Fixed radius{};  // contact radius against a player
};

struct PlayerDef {
  Fixed radius{};
  Fixed moveSpeed{};  // units/tick
  uint8_t maxHp{1};
  uint8_t ammoMax{1};
  uint8_t ammoStart{1};
  uint16_t hitStunTicks{0};
  // Base loadout: slot kAbilitySlotDash, kAbilitySlotThrow, kAbilitySlotExtra
  // (indices into ContentDB::abilities; 0 = None). It is also loadout 0
  // (ContentDB::loadouts[0], "base"); a match may pick another (v0.31).
  std::array<uint8_t, kAbilitySlots> abilities{};
};

// Dash attack (rules/dash_attack.cpp, architecture §5.6): a player dashing
// into an enemy while holding at least minAmmo bananas deals `damage`. A
// content switch: minAmmo = 0 would make every dash an attack (the default
// content asserts minAmmo >= 1), and consumesAmmo spends one banana per hit.
struct DashAttackDef {
  uint8_t damage{1};
  uint8_t minAmmo{1};
  uint8_t consumesAmmo{0};  // 0/1
};

struct ContentDB {
  std::array<AbilityDef, kMaxAbilityDefs> abilities{};  // index 0 = None, then Dash, Throw
  uint8_t abilityCount{0};
  BoomerangDef boomerang{};
  PickupDef pickup{};
  PlayerDef player{};
  DashAttackDef dashAttack{};
  // Selected per match by MatchConfig::arena / ::mode (indices; names are
  // resolved by the loader and the apps). Index 0 is the default.
  std::array<ArenaDef, kMaxArenas> arenas{};
  uint8_t arenaCount{0};
  std::array<ModeDef, kMaxModes> modes{};
  uint8_t modeCount{0};
  // Power-ups (§5.8), referenced by pickups, spawners and ModifierList
  // entries by index.
  std::array<PowerUpDef, kMaxPowerUps> powerUps{};
  uint8_t powerUpCount{0};
  // Loadouts (v0.31), selected per match by MatchConfig::loadout. Index 0 is
  // the base kit: always present, its abilities equal player.abilities.
  std::array<LoadoutDef, kMaxLoadouts> loadouts{};
  uint8_t loadoutCount{0};

  // Power-up at an index; out of range falls back to an inert empty def.
  [[nodiscard]] constexpr const PowerUpDef& powerUpAt(uint8_t index) const {
    return index < powerUpCount ? powerUps[index] : kNoPowerUp;
  }
  [[nodiscard]] constexpr std::optional<uint8_t> findPowerUp(std::string_view name) const {
    for (uint8_t i = 0; i < powerUpCount; ++i) {
      if (powerUps[i].name.view() == name) return i;
    }
    return std::nullopt;
  }
  static constexpr PowerUpDef kNoPowerUp{};

  // Def at a state's defIndex; an out-of-range index falls back to the inert
  // None def (index 0), so a bad index can never read past the table.
  [[nodiscard]] constexpr const AbilityDef& abilityAt(uint8_t index) const {
    return index < abilities.size() ? abilities[index] : abilities[0];
  }

  // Def for an ability slot of the base loadout.
  [[nodiscard]] constexpr const AbilityDef& abilityForSlot(uint8_t slot) const {
    return abilities[player.abilities[slot]];
  }

  // Arena / mode at an index; out of range falls back to index 0 (the
  // loader guarantees at least one of each).
  [[nodiscard]] constexpr const ArenaDef& arenaAt(uint8_t index) const {
    return index < arenaCount ? arenas[index] : arenas[0];
  }
  [[nodiscard]] constexpr const ModeDef& modeAt(uint8_t index) const {
    return index < modeCount ? modes[index] : modes[0];
  }
  // Index of the arena / mode with this name, if any.
  [[nodiscard]] constexpr std::optional<uint8_t> findArena(std::string_view name) const {
    for (uint8_t i = 0; i < arenaCount; ++i) {
      if (arenas[i].name.view() == name) return i;
    }
    return std::nullopt;
  }
  [[nodiscard]] constexpr std::optional<uint8_t> findMode(std::string_view name) const {
    for (uint8_t i = 0; i < modeCount; ++i) {
      if (modes[i].name.view() == name) return i;
    }
    return std::nullopt;
  }
  // Loadout at an index; out of range falls back to index 0 (the base kit).
  [[nodiscard]] constexpr const LoadoutDef& loadoutAt(uint8_t index) const {
    return index < loadoutCount ? loadouts[index] : loadouts[0];
  }
  // The ability def per slot of a loadout. Index 0 (and an index out of
  // range) is the base kit, read from player.abilities itself: loadouts[0]
  // mirrors it for the name and the hash (validated equal), so content that
  // edits the player's kit in code still plays what it says.
  [[nodiscard]] constexpr const std::array<uint8_t, kAbilitySlots>& loadoutAbilities(
      uint8_t index) const {
    return index != 0 && index < loadoutCount ? loadouts[index].abilities : player.abilities;
  }
  [[nodiscard]] constexpr std::optional<uint8_t> findLoadout(std::string_view name) const {
    for (uint8_t i = 0; i < loadoutCount; ++i) {
      if (loadouts[i].name.view() == name) return i;
    }
    return std::nullopt;
  }
};

// What one match simulates with (architecture §5.10): the content DB plus the
// arena, mode, power-up set and loadout its MatchConfig selected. Every
// system, ability behaviour and interaction rule takes one; arena- and
// mode-dependent code reads `content.arena` / `content.mode`, the kit
// `content.loadout()`, everything else `content.db`. A bare ContentDB
// converts implicitly to its default match (arena 0, mode 0, loadout 0),
// which is what the headless tests use; hosts build it from their
// MatchConfig (matchContent() in world_state.hpp). It holds references:
// never keep one past the DB.
struct MatchContent {
  const ContentDB& db;
  const ArenaDef& arena;
  const ModeDef& mode;
  uint16_t powerUps{kAllPowerUps};  // MatchConfig::powerUps: the enabled power-ups
  uint8_t loadoutIndex{0};          // MatchConfig::loadout (v0.31): into ContentDB::loadouts

  constexpr MatchContent(const ContentDB& content, uint8_t arenaIndex, uint8_t modeIndex,
                         uint16_t powerUpSet = kAllPowerUps, uint8_t selectedLoadout = 0)
      : db(content),
        arena(content.arenaAt(arenaIndex)),
        mode(content.modeAt(modeIndex)),
        powerUps(powerUpSet),
        loadoutIndex(selectedLoadout) {}
  // NOLINTNEXTLINE(misc-explicit-constructor): implicit on purpose, see above
  constexpr MatchContent(const ContentDB& content) : MatchContent(content, 0, 0) {}

  // The loadout the match's monkeys spawn with and ModifierSystem restores
  // after an override, as the ability def per slot (v0.31: the match's;
  // M5b step 2 adds the per-role lookup of team modes).
  [[nodiscard]] constexpr const std::array<uint8_t, kAbilitySlots>& loadout() const {
    return db.loadoutAbilities(loadoutIndex);
  }
};

// The M1 constexpr tables (default tuning numbers from the M1 contract).
[[nodiscard]] const ContentDB& defaultContent();

// XXH32 of the canonical serialized form (visit(V&, ContentDB&)). Peers and
// replays must match it.
[[nodiscard]] uint32_t contentHash(const ContentDB& content);

}  // namespace kfm::sim
