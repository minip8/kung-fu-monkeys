// Immutable content definitions (architecture §5.5, §5.7, §5.10). The
// source of truth is the TOML under content/, parsed and validated by
// engine/content into one ContentDB and hashed through the visitor;
// defaultContent() is the same data as constexpr tables, the headless
// fixture of the sim tests. Systems read every tuning number from here,
// never from literals of their own, and receive it as a MatchContent: the
// DB plus the arena and mode the match selected.
//
// Units: lengths in arena units, speeds in units per tick, durations in ticks.
#pragma once

#include "kfm/core/fixed.hpp"
#include "kfm/core/vec2.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/tags.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace kfm::sim {

inline constexpr uint8_t kAbilitySlots = 2;
inline constexpr uint8_t kAbilitySlotDash = 0;
inline constexpr uint8_t kAbilitySlotThrow = 1;
inline constexpr uint8_t kMaxAbilityDefs = 4;
inline constexpr uint8_t kMaxArenas = 8;
inline constexpr uint8_t kMaxModes = 8;

// A def's name as TOML spells it (`name = "dojo"`): up to kMaxChars
// characters, NUL padded, so it is fixed-size, trivially copyable and hashed
// byte for byte. Names are how content files, command lines and the lobby
// refer to arenas and modes; the sim only ever sees indices.
struct ContentName {
  static constexpr size_t kBytes = 16;
  static constexpr size_t kMaxChars = kBytes - 1;  // always NUL terminated
  std::array<char, kBytes> chars{};

  // Truncates past kMaxChars (the loader rejects such names first).
  [[nodiscard]] static constexpr ContentName of(std::string_view s) {
    ContentName n;
    for (size_t i = 0; i < s.size() && i < kMaxChars; ++i)
      n.chars[i] = s[i];
    return n;
  }
  [[nodiscard]] constexpr std::string_view view() const {
    size_t len = 0;
    while (len < kMaxChars && chars[len] != '\0')
      ++len;
    return {chars.data(), len};
  }
  [[nodiscard]] constexpr const char* cStr() const { return chars.data(); }
  constexpr bool operator==(const ContentName&) const = default;
};

enum class AbilityKind : uint8_t {
  None = 0,
  Dash,
  Throw,
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
  Fixed speed{};                   // Dash: dash speed (units/tick). Throw: unused
  Fixed param0{};                  // kind-specific spare numeric params
  Fixed param1{};
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
  uint8_t damage{1};
  uint8_t pierce{0};  // 0/1: keeps flying after a hit
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
  // Base loadout: slot kAbilitySlotDash, slot kAbilitySlotThrow (indices into
  // ContentDB::abilities).
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

struct ArenaDef {
  ContentName name{};
  Vec2 halfExtents{};  // box arena, centred at the origin
  std::array<Vec2, kMaxPlayers> spawnPoints{};
  uint8_t spawnPointCount{0};
};

struct ModeDef {  // FFA rules
  ContentName name{};
  uint8_t roundsToWin{3};
  uint16_t roundEndTicks{120};   // pause after a round ends before the next starts
  uint16_t roundStartTicks{60};  // countdown at round start (players spawned, inputs ignored)
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
};

// What one match simulates with (architecture §5.10): the content DB plus the
// arena and mode its MatchConfig selected. Every system, ability behaviour
// and interaction rule takes one; arena- and mode-dependent code reads
// `content.arena` / `content.mode`, everything else `content.db`. A bare
// ContentDB converts implicitly to its default match (arena 0, mode 0), which
// is what the headless tests use; hosts build it from their MatchConfig
// (matchContent() in world_state.hpp). It holds references: never keep one past the DB.
struct MatchContent {
  const ContentDB& db;
  const ArenaDef& arena;
  const ModeDef& mode;

  constexpr MatchContent(const ContentDB& content, uint8_t arenaIndex, uint8_t modeIndex)
      : db(content), arena(content.arenaAt(arenaIndex)), mode(content.modeAt(modeIndex)) {}
  // NOLINTNEXTLINE(misc-explicit-constructor): implicit on purpose, see above
  constexpr MatchContent(const ContentDB& content) : MatchContent(content, 0, 0) {}
};

// The M1 constexpr tables (default tuning numbers from the M1 contract).
[[nodiscard]] const ContentDB& defaultContent();

// XXH32 of the canonical serialized form (visit(V&, ContentDB&)). Peers and
// replays must match it.
[[nodiscard]] uint32_t contentHash(const ContentDB& content);

}  // namespace kfm::sim
