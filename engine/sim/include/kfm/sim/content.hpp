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
inline constexpr uint8_t kMaxArenas = 16;  // v0.33 (8 before: the objective arenas filled it)
inline constexpr uint8_t kMaxModes = 32;   // v0.31 (8 before)

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

// AbilityDef::rootPhases bit of a phase (v0.34).
[[nodiscard]] constexpr uint8_t phaseBit(AbilityPhase phase) {
  return static_cast<uint8_t>(1u << static_cast<uint8_t>(phase));
}

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
  // v0.34, rooting (§5.10): the phases (phaseBit) during which the monkey's
  // movement intent is ignored (continuous) or it takes no new cell
  // commitment at a latch (grid). 0 = never rooted.
  uint8_t rootPhases{0};
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

  // Whether a match may pair this arena with this mode (v0.32, v0.33): a
  // team mode needs a spawn group per team, hill a zone (zone 0), capture a
  // zone per team (team t's is zone t). Step 4 (§5.12) adds a grid mode's
  // `cellSize > 0`. An incompatible pair is a configuration error of the
  // host (the lobby skips it, a command line refuses it), never a load
  // error: the sim plays any pair (a missing zone holds nobody).
  [[nodiscard]] constexpr bool compatible(uint8_t arena, uint8_t mode) const {
    const ModeDef& m = modeAt(mode);
    const ArenaDef& a = arenaAt(arena);
    if (m.teams != 0 && a.spawnGroups < m.teams) return false;
    if (m.kind == ModeKind::Hill && a.zoneCount < 1) return false;
    if (m.kind == ModeKind::Capture && a.zoneCount < (m.teams > 0 ? m.teams : 1)) return false;
    return true;
  }
  // The first arena compatible with the mode, if any.
  [[nodiscard]] constexpr std::optional<uint8_t> firstCompatibleArena(uint8_t mode) const {
    for (uint8_t i = 0; i < arenaCount; ++i) {
      if (compatible(i, mode)) return i;
    }
    return std::nullopt;
  }
};

// Teams (v0.32, architecture §5.3): a team index per slot, constant for a
// match like the arena (MatchConfig::teams, MatchContent::teams). A
// free-for-all gives slot i team i, so every other slot is an enemy.
using TeamSet = std::array<uint8_t, kMaxPlayers>;
[[nodiscard]] constexpr TeamSet freeForAllTeams() {
  TeamSet t{};
  for (uint8_t i = 0; i < kMaxPlayers; ++i)
    t[i] = i;
  return t;
}
inline constexpr TeamSet kFreeForAllTeams = freeForAllTeams();

// How one monkey stands to another (or to a banana's owner): the rules
// table's InteractionRule::relationB is a mask over these bits.
enum class Relation : uint8_t { Self = 0, Ally, Enemy };
[[nodiscard]] constexpr uint8_t relationBit(Relation r) {
  return static_cast<uint8_t>(1u << static_cast<uint8_t>(r));
}
inline constexpr uint8_t kRelationAny =
    relationBit(Relation::Self) | relationBit(Relation::Ally) | relationBit(Relation::Enemy);

// What one match simulates with (architecture §5.10): the content DB plus the
// arena, mode, power-up set and loadout its MatchConfig selected. Every
// system, ability behaviour and interaction rule takes one; arena- and
// mode-dependent code reads `content.arena` / `content.mode`, the kit
// `content.loadout()`, everything else `content.db`. A bare ContentDB
// converts implicitly to its default match (arena 0, mode 0, loadout 0),
// which is what the headless tests use; hosts build it from their
// MatchConfig (matchContent() in world_state.hpp). It holds references:
// never keep one past the DB.
struct ModeBehavior;  // modes/registry.hpp

struct MatchContent {
  const ContentDB& db;
  const ArenaDef& arena;
  const ModeDef& mode;
  uint16_t powerUps{kAllPowerUps};  // MatchConfig::powerUps: the enabled power-ups
  uint8_t loadoutIndex{0};          // MatchConfig::loadout (v0.31): into ContentDB::loadouts
  TeamSet teams{kFreeForAllTeams};  // MatchConfig::teams (v0.32)
  // Tests only (v0.31): replaces the mode's ModeBehavior row when set, so a
  // scenario can run a probe mode without a ModeKind of its own
  // (modeBehaviorOf() in modes/registry.hpp). Hosts never set it.
  const ModeBehavior* modeBehavior{nullptr};

  constexpr MatchContent(const ContentDB& content, uint8_t arenaIndex, uint8_t modeIndex,
                         uint16_t powerUpSet = kAllPowerUps, uint8_t selectedLoadout = 0,
                         const TeamSet& teamSet = kFreeForAllTeams)
      : db(content),
        arena(content.arenaAt(arenaIndex)),
        mode(content.modeAt(modeIndex)),
        powerUps(powerUpSet),
        loadoutIndex(selectedLoadout),
        teams(teamSet) {}
  // NOLINTNEXTLINE(misc-explicit-constructor): implicit on purpose, see above
  constexpr MatchContent(const ContentDB& content) : MatchContent(content, 0, 0) {}

  // The match's loadout as the ability def per slot (v0.31).
  [[nodiscard]] constexpr const std::array<uint8_t, kAbilitySlots>& loadout() const {
    return db.loadoutAbilities(loadoutIndex);
  }
  // The loadout index slot `id` plays (v0.32): in a team mode with roles,
  // the role at the slot's position within its team (slots in slot order,
  // round-robin over the roles); otherwise the match's.
  [[nodiscard]] constexpr uint8_t loadoutIndexFor(PlayerId id) const {
    if (mode.teams == 0 || mode.roleCount == 0 || !id.isValid() || id.value >= kMaxPlayers) {
      return loadoutIndex;
    }
    uint8_t position = 0;
    for (uint8_t j = 0; j < id.value; ++j) {
      if (teams[j] == teams[id.value]) ++position;
    }
    const uint8_t count = mode.roleCount < kMaxPlayers ? mode.roleCount : kMaxPlayers;
    return mode.roles[position % count];
  }
  // The kit slot `id` spawns with and ModifierSystem restores after an
  // override, as the ability def per slot.
  [[nodiscard]] constexpr const std::array<uint8_t, kAbilitySlots>& loadout(PlayerId id) const {
    return db.loadoutAbilities(loadoutIndexFor(id));
  }

  // The team of a slot (an invalid slot has none: kMaxPlayers).
  [[nodiscard]] constexpr uint8_t teamOf(PlayerId id) const {
    return id.isValid() && id.value < kMaxPlayers ? teams[id.value] : uint8_t{kMaxPlayers};
  }
  // How `b` stands to `a`: Self, Ally (same team) or Enemy. A slot without a
  // team (invalid: a banana whose thrower is unknown) is an enemy, as every
  // other slot is in a free-for-all.
  [[nodiscard]] constexpr Relation relation(PlayerId a, PlayerId b) const {
    if (a == b) return Relation::Self;
    if (!a.isValid() || !b.isValid() || a.value >= kMaxPlayers || b.value >= kMaxPlayers) {
      return Relation::Enemy;
    }
    return teams[a.value] == teams[b.value] ? Relation::Ally : Relation::Enemy;
  }
  // A team's score (v0.32): the sum of its slots' per-slot scores
  // (MatchRules::scores: round wins or eliminations).
  [[nodiscard]] constexpr uint16_t teamScore(const std::array<uint8_t, kMaxPlayers>& scores,
                                             uint8_t team) const {
    uint16_t sum = 0;
    for (uint8_t i = 0; i < kMaxPlayers; ++i) {
      if (teams[i] == team) sum = static_cast<uint16_t>(sum + scores[i]);
    }
    return sum;
  }
};

// The M1 constexpr tables (default tuning numbers from the M1 contract).
[[nodiscard]] const ContentDB& defaultContent();

// XXH32 of the canonical serialized form (visit(V&, ContentDB&)). Peers and
// replays must match it.
[[nodiscard]] uint32_t contentHash(const ContentDB& content);

}  // namespace kfm::sim
