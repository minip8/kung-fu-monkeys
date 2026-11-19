#include "presets.hpp"

#include "content_labels.hpp"
#include "kfm/content/app_content.hpp"
#include "kfm/sim/constants.hpp"

#include <algorithm>
#include <string>

namespace kfm::client {

namespace {

// One per shipped player-facing mode (content/modes.toml; the bot contests
// are left out as the lobby leaves them out), each on an arena made for it.
constexpr std::array<GamePreset, 10> kPresets{{
    {.id = "classic",
     .title = "Classic",
     .blurb = "Last monkey standing wins the round, with power-ups to grab. First to 3 rounds.",
     .mode = "rounds",
     .arena = "dojo_powerups",
     .loadout = "base",
     .powerUps = "all",
     .players = 4},
    {.id = "brawl",
     .title = "Brawl",
     .blurb = "Knocked-out monkeys come back: the most knockouts in 3 minutes wins.",
     .mode = "deathmatch",
     .arena = "pillars",
     .loadout = "base",
     .powerUps = "all",
     .players = 4},
    {.id = "frenzy",
     .title = "Power-up Frenzy",
     .blurb = "A big brawl across the marsh, power-ups popping up all over it.",
     .mode = "deathmatch",
     .arena = "marsh",
     .loadout = "base",
     .powerUps = "all",
     .players = 6},
    {.id = "survival",
     .title = "Survival",
     .blurb = "One-minute rounds on an island ringed by a drop: stay up, stay dry.",
     .mode = "survival",
     .arena = "island",
     .loadout = "base",
     .powerUps = "all",
     .players = 4},
    {.id = "knockout",
     .title = "Knockout",
     .blurb = "Rounds in the corridors where every knockout is a point: first to 10.",
     .mode = "kill_rounds",
     .arena = "corridors",
     .loadout = "base",
     .powerUps = "all",
     .players = 4},
    {.id = "team_brawl",
     .title = "Team Brawl",
     .blurb = "Two teams in a walled compound: every knockout scores for your team.",
     .mode = "team_deathmatch",
     .arena = "compound",
     .loadout = "base",
     .powerUps = "all",
     .players = 4},
    {.id = "team_rounds",
     .title = "Team Rounds",
     .blurb = "Two teams face off: the last team with a monkey up wins the round.",
     .mode = "team_rounds",
     .arena = "standoff",
     .loadout = "base",
     .powerUps = "all",
     .players = 4},
    {.id = "hill",
     .title = "King of the Hill",
     .blurb = "Hold the hill with no rival on it to score for your team.",
     .mode = "hill",
     .arena = "hilltop",
     .loadout = "base",
     .powerUps = "all",
     .players = 4},
    {.id = "capture",
     .title = "Capture the Flag",
     .blurb = "Bring the other team's flag home while yours is safe at home.",
     .mode = "capture",
     .arena = "outposts",
     .loadout = "base",
     .powerUps = "all",
     .players = 4},
    {.id = "grid",
     .title = "Grid Rounds",
     .blurb = "Two teams hop tile to tile on the beat: the last team standing wins.",
     .mode = "grid_rounds",
     .arena = "courtyard",
     .loadout = "base",
     .powerUps = "all",
     .players = 4},
}};
static_assert(kPresets.size() <= kMaxPresets);

// The power-ups that count on this arena: what its spawners can draw.
[[nodiscard]] uint16_t drawn(const sim::ContentDB& content, uint8_t arena, uint16_t set) {
  return static_cast<uint16_t>(arenaPowerUps(content, content.arenaAt(arena)) & set);
}

}  // namespace

std::span<const GamePreset> gamePresets() {
  return kPresets;
}

std::optional<ResolvedPreset> resolvePreset(const sim::ContentDB& content, const GamePreset& preset,
                                            std::string& error) {
  const std::optional<uint8_t> mode = content.findMode(preset.mode);
  const std::optional<uint8_t> arena = content.findArena(preset.arena);
  const std::optional<uint8_t> loadout = content.findLoadout(preset.loadout);
  if (!mode || !arena || !loadout) {
    std::string what = "loadout '" + std::string(preset.loadout);
    if (!mode) {
      what = "mode '" + std::string(preset.mode);
    } else if (!arena) {
      what = "arena '" + std::string(preset.arena);
    }
    error = "preset '" + std::string(preset.id) + "': this content has no " + what + "'";
    return std::nullopt;
  }
  if (!content.compatible(*arena, *mode)) {
    error = "preset '" + std::string(preset.id) + "': arena '" + std::string(preset.arena) +
            "' does not suit mode '" + std::string(preset.mode) + "'";
    return std::nullopt;
  }
  uint16_t powerUps = 0;
  if (!content::resolvePowerUpList(content, preset.powerUps, powerUps, error)) {
    error = "preset '" + std::string(preset.id) + "': " + error;
    return std::nullopt;
  }
  return ResolvedPreset{
      .def = &preset, .mode = *mode, .arena = *arena, .loadout = *loadout, .powerUps = powerUps};
}

PresetList resolvePresets(const sim::ContentDB& content) {
  PresetList out;
  for (const GamePreset& p : kPresets) {
    std::string error;
    if (const std::optional<ResolvedPreset> r = resolvePreset(content, p, error)) {
      out.items[out.count++] = *r;
    }
  }
  return out;
}

bool presetMatches(const sim::ContentDB& content, const ResolvedPreset& preset,
                   const LobbySettings& settings) {
  if (settings.mode != preset.mode || settings.arena != preset.arena) return false;
  // A mode that hands kits out by role plays no match kit.
  if (content.modeAt(preset.mode).roleCount == 0 && settings.loadout != preset.loadout) {
    return false;
  }
  return drawn(content, preset.arena, settings.powerUps) ==
         drawn(content, preset.arena, preset.powerUps);
}

std::optional<std::size_t> findPreset(const sim::ContentDB& content,
                                      std::span<const ResolvedPreset> presets,
                                      const LobbySettings& settings) {
  for (std::size_t i = 0; i < presets.size(); ++i) {
    if (presetMatches(content, presets[i], settings)) return i;
  }
  return std::nullopt;
}

bool applyPreset(const ResolvedPreset& preset, uint8_t humans, uint8_t maxBots,
                 LobbySettings& settings) {
  settings.mode = preset.mode;
  settings.arena = preset.arena;
  settings.loadout = preset.loadout;
  settings.powerUps = preset.powerUps;
  const uint8_t players = preset.def != nullptr ? preset.def->players : 0;
  const uint8_t needed = players > humans ? static_cast<uint8_t>(players - humans) : uint8_t{0};
  const uint8_t wanted = std::min(needed, maxBots);
  if (wanted > settings.bots) {
    settings.bots = wanted;
    return true;
  }
  return false;
}

}  // namespace kfm::client
