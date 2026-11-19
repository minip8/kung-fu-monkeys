// Game presets (architecture §7, v0.97): the game-setup panel's first row.
// A preset is a whole match in one choice (mode, arena, kit, power-up set)
// with a name a player reads ("Classic", "King of the Hill") and how many
// monkeys it wants. SDL- and ImGui-free; tested in
// tests/unit/client/presets_test.cpp.
//
// The table is client-side and names content by its content names, resolved
// against the loaded ContentDB (resolvePresets()): presets are a front-end
// choice, not content, so content/*.toml and its hash never change for them.
// A preset whose names the content does not define (a --content directory
// without them) or whose arena does not suit its mode is left out.
//
// Which preset is active is never stored: it is the first one whose match
// the settings equal (presetMatches()), else "Custom". The comparison skips
// what cannot matter: the kit under a mode that hands kits out by role, and
// the power-ups the arena's spawners cannot draw (all of them on an arena
// without spawners). Bots and their difficulty are never compared: adding a
// bot does not make a preset Custom.
//
// Online a preset is the RoomSettings it implies (arena, mode, loadout,
// power-ups, bots): nothing new goes on the wire.
#pragma once

#include "kfm/sim/content.hpp"
#include "lobby_settings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace kfm::client {

struct GamePreset {
  std::string_view id;     // stable, lower case ("classic")
  std::string_view title;  // what the Game row shows ("Classic")
  std::string_view blurb;  // one line under it
  std::string_view mode;   // content names
  std::string_view arena;
  std::string_view loadout;
  std::string_view powerUps;  // "all", "none" or comma-separated names
  // The monkeys the preset is meant for: picking it raises the bots so the
  // humans and the bots make at least this many (never lowers them).
  uint8_t players{2};
};

// Every preset, in the Game row's order. The first is the default of a
// first run (kDefaultPreset).
[[nodiscard]] std::span<const GamePreset> gamePresets();
inline constexpr std::string_view kDefaultPreset = "classic";
inline constexpr std::size_t kMaxPresets = 16;

// A preset resolved against a content: indices, ready to compare and apply.
struct ResolvedPreset {
  const GamePreset* def{nullptr};
  uint8_t mode{0};
  uint8_t arena{0};
  uint8_t loadout{0};
  uint16_t powerUps{0};
};

struct PresetList {
  std::array<ResolvedPreset, kMaxPresets> items{};
  std::size_t count{0};
  [[nodiscard]] std::span<const ResolvedPreset> view() const { return {items.data(), count}; }
};

// The presets this content can play, in table order (see the header).
[[nodiscard]] PresetList resolvePresets(const sim::ContentDB& content);
// One preset, or nullopt with the reason (an unknown name, an arena that
// does not suit the mode): what the unit test checks for the shipped content.
[[nodiscard]] std::optional<ResolvedPreset> resolvePreset(const sim::ContentDB& content,
                                                          const GamePreset& preset,
                                                          std::string& error);

// Whether the settings play the preset's match (see the header).
[[nodiscard]] bool presetMatches(const sim::ContentDB& content, const ResolvedPreset& preset,
                                 const LobbySettings& settings);
// The index into `presets` of the first preset the settings match.
[[nodiscard]] std::optional<std::size_t> findPreset(const sim::ContentDB& content,
                                                    std::span<const ResolvedPreset> presets,
                                                    const LobbySettings& settings);

// Applies the preset's match to `settings`, and raises the bots so that
// `humans` and the bots make the preset's players (at most `maxBots`).
// True when the bots were raised.
bool applyPreset(const ResolvedPreset& preset, uint8_t humans, uint8_t maxBots,
                 LobbySettings& settings);

}  // namespace kfm::client
