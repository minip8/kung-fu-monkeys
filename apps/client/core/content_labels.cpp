#include "content_labels.hpp"

#include "kfm/sim/constants.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <utility>

namespace kfm::client {

namespace {

struct ModeEntry {
  std::string_view name;
  std::string_view title;
  std::string_view blurb;
};

// The shipped modes (content/modes.toml).
constexpr std::array<ModeEntry, 13> kModes{{
    {"rounds", "Last Monkey Standing", "Knock the others out: the last monkey up wins the round."},
    {"deathmatch", "Brawl",
     "Every knockout scores and the knocked-out come back: most knockouts wins."},
    {"survival", "Survival",
     "Rounds on a clock: be the last one up, or the healthiest when time runs out."},
    {"team_rounds", "Team Rounds", "Two teams: the last team with a monkey up wins the round."},
    {"team_deathmatch", "Team Brawl",
     "Two teams: every knockout scores for your team, and the knocked-out come back."},
    {"hill", "King of the Hill",
     "Stand on the hill with no rival on it to score points for your team."},
    {"capture", "Capture the Flag",
     "Carry the other team's flag home while your own flag is safe at home."},
    {"grid_rounds", "Grid Rounds",
     "Teams hop from tile to tile on the beat: the last team standing wins the round."},
    {"kill_rounds", "Knockout Rounds",
     "Free-for-all rounds where every knockout is a point: first to the target wins."},
    {"grid_rounds_1t", "Grid Rounds (bots)", "One-tick turns on the grid, for bot matches."},
    {"contest", "Harvest (bot contest)",
     "The bot ladder: carry bananas from the trees to your team's home."},
    {"control_1t", "Control (bot contest)",
     "The bot ladder: take the zones and hold them for points."},
    {"harvest", "Harvest", "Carry bananas from the trees to your team's home and bank them."},
}};

constexpr std::array<std::pair<std::string_view, std::string_view>, 1> kArenas{{
    {"dojo_powerups", "Dojo with Power-ups"},
}};

constexpr std::array<std::pair<std::string_view, std::string_view>, 1> kLoadouts{{
    {"base", "Standard"},
}};

[[nodiscard]] std::string fromTable(
    std::string_view name, std::span<const std::pair<std::string_view, std::string_view>> t) {
  for (const auto& [n, title] : t) {
    if (n == name) return std::string(title);
  }
  return prettyContentName(name);
}

}  // namespace

std::string prettyContentName(std::string_view name) {
  constexpr std::string_view kTick = "_1t";
  bool oneTick = false;
  if (name.size() > kTick.size() && name.ends_with(kTick)) {
    oneTick = true;
    name.remove_suffix(kTick.size());
  }
  std::string out;
  out.reserve(name.size() + 10);
  bool wordStart = true;
  for (const char c : name) {
    if (c == '_' || c == '-' || c == ' ') {
      if (!out.empty() && out.back() != ' ') out.push_back(' ');
      wordStart = true;
      continue;
    }
    if (wordStart && c >= 'a' && c <= 'z') {
      out.push_back(static_cast<char>(c - 'a' + 'A'));
    } else {
      out.push_back(c);
    }
    wordStart = false;
  }
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  if (oneTick) out += " (1-tick)";
  return out;
}

ModeLabel modeLabel(std::string_view name) {
  for (const ModeEntry& e : kModes) {
    if (e.name == name) return {.title = std::string(e.title), .blurb = std::string(e.blurb)};
  }
  return {.title = prettyContentName(name), .blurb = {}};
}

std::string arenaTitle(std::string_view name) {
  return fromTable(name, kArenas);
}

std::string loadoutTitle(std::string_view name) {
  return fromTable(name, kLoadouts);
}

std::string powerUpTitle(std::string_view name) {
  return prettyContentName(name);
}

bool isBotMode(const sim::ModeDef& mode) {
  return mode.space == sim::Space::Grid && mode.turnTicks <= 1;
}

bool isBotLoadout(const sim::ContentDB& db, uint8_t loadout) {
  bool botRole = false;
  for (uint8_t m = 0; m < db.modeCount && m < sim::kMaxModes; ++m) {
    const sim::ModeDef& mode = db.modes[m];
    const uint8_t roles = std::min<uint8_t>(mode.roleCount, sim::kMaxPlayers);
    for (uint8_t r = 0; r < roles; ++r) {
      if (mode.roles[r] != loadout) continue;
      if (!isBotMode(mode)) return false;  // a human mode hands it out
      botRole = true;
    }
  }
  return botRole;
}

uint16_t arenaPowerUps(const sim::ContentDB& db, const sim::ArenaDef& arena) {
  const uint8_t count = std::min<uint8_t>(db.powerUpCount, sim::kMaxPowerUps);
  const auto all = static_cast<uint16_t>((uint32_t{1} << count) - 1u);
  uint16_t set = 0;
  for (uint8_t i = 0; i < arena.pickupSpawnerCount && i < sim::kMaxPickupSpawners; ++i) {
    const sim::PickupSpawner& sp = arena.pickupSpawners[i];
    if (sp.isTree()) continue;
    set = static_cast<uint16_t>(set | (sp.powerUpSet == 0 ? all : (sp.powerUpSet & all)));
  }
  return set;
}

}  // namespace kfm::client
