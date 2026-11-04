#include "kfm/content/app_content.hpp"

#include "kfm/content/loader.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <system_error>
#include <utility>

namespace kfm::content {
namespace {

[[nodiscard]] std::string hex32(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%08x", v);
  return buf;
}

#ifndef __EMSCRIPTEN__
[[nodiscard]] bool hasTomlFile(const std::filesystem::path& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return false;
  for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    std::error_code fileEc;
    if (it->is_regular_file(fileEc) && it->path().extension() == ".toml") return true;
  }
  return false;
}
#endif

template <class Def, size_t N>
[[nodiscard]] std::string nameList(const std::array<Def, N>& defs, uint8_t count) {
  std::string s;
  for (uint8_t i = 0; i < count && i < N; ++i) {
    if (!s.empty()) s += ", ";
    s += defs[i].name.view();
  }
  return s;
}

}  // namespace

std::string LoadedContent::describe() const {
  std::string s = "content: ";
  s += embedded() ? std::string("embedded") : dir.generic_string();
  s += " (hash " + hex32(hash) + ", " + std::to_string(db.arenaCount) +
       (db.arenaCount == 1 ? " arena, " : " arenas, ") + std::to_string(db.modeCount) +
       (db.modeCount == 1 ? " mode)" : " modes)");
  return s;
}

std::filesystem::path findContentDir(std::string_view cliDir, const std::filesystem::path& exeDir) {
  if (!cliDir.empty()) return {cliDir};
#ifdef __EMSCRIPTEN__
  (void)exeDir;
  return {};
#else
  if (exeDir.empty()) return {};
  const std::filesystem::path dir = exeDir / "content";
  return hasTomlFile(dir) ? dir : std::filesystem::path{};
#endif
}

bool loadAppContent(std::string_view cliDir, const std::filesystem::path& exeDir,
                    LoadedContent& out, Diagnostics& diag) {
  LoadedContent loaded;
  loaded.dir = findContentDir(cliDir, exeDir);
  const bool ok = loaded.embedded() ? loadEmbeddedContent(loaded.db, diag)
                                    : loadContentDir(loaded.dir, loaded.db, diag);
  if (!ok) return false;
  loaded.hash = sim::contentHash(loaded.db);
  out = std::move(loaded);
  return true;
}

std::filesystem::path executableDir(const char* argv0) {
  if (argv0 == nullptr || *argv0 == '\0') return {};
  std::error_code ec;
  const std::filesystem::path p = std::filesystem::absolute(std::filesystem::path(argv0), ec);
  return ec ? std::filesystem::path{} : p.parent_path();
}

bool resolveMatchNames(const sim::ContentDB& content, std::string_view arena, std::string_view mode,
                       sim::MatchConfig& config, std::string& error) {
  sim::MatchConfig c = config;
  c.arena = 0;
  c.mode = 0;
  if (!arena.empty()) {
    const std::optional<uint8_t> i = content.findArena(arena);
    if (!i) {
      error = "unknown arena '" + std::string(arena) +
              "' (content has: " + nameList(content.arenas, content.arenaCount) + ")";
      return false;
    }
    c.arena = *i;
  }
  if (!mode.empty()) {
    const std::optional<uint8_t> i = content.findMode(mode);
    if (!i) {
      error = "unknown mode '" + std::string(mode) +
              "' (content has: " + nameList(content.modes, content.modeCount) + ")";
      return false;
    }
    c.mode = *i;
  }
  // v0.32: the arena must suit the mode (ContentDB::compatible). Without an
  // --arena a team mode takes the first arena that does; a named arena that
  // does not is a configuration error.
  if (!content.compatible(c.arena, c.mode)) {
    const sim::ModeDef& m = content.modeAt(c.mode);
    const std::optional<uint8_t> first = content.firstCompatibleArena(c.mode);
    if (!arena.empty() || !first) {
      std::string fits;
      for (uint8_t a = 0; a < content.arenaCount; ++a) {
        if (!content.compatible(a, c.mode)) continue;
        if (!fits.empty()) fits += ", ";
        fits += content.arenas[a].name.view();
      }
      error = "arena '" + std::string(content.arenaAt(c.arena).name.view()) +
              "' does not suit mode '" + std::string(m.name.view()) +
              "' (a spawn group per team, the mode's zones, cells for a grid mode; arenas that "
              "do: " +
              (fits.empty() ? std::string("none") : fits) + ")";
      return false;
    }
    c.arena = *first;
  }
  c.teams = sim::teamsFor(content.modeAt(c.mode), c.players);
  config = c;
  return true;
}

bool resolveLoadoutName(const sim::ContentDB& content, std::string_view loadout,
                        sim::MatchConfig& config, std::string& error) {
  if (loadout.empty()) {
    config.loadout = 0;
    return true;
  }
  const std::optional<uint8_t> i = content.findLoadout(loadout);
  if (!i) {
    error = "unknown loadout '" + std::string(loadout) +
            "' (content has: " + nameList(content.loadouts, content.loadoutCount) + ")";
    return false;
  }
  config.loadout = *i;
  return true;
}

bool resolvePowerUpList(const sim::ContentDB& content, std::string_view list, uint16_t& powerUps,
                        std::string& error) {
  if (list.empty() || list == "all") {
    powerUps = sim::kAllPowerUps;
    return true;
  }
  if (list == "none") {
    powerUps = 0;
    return true;
  }
  uint16_t set = 0;
  while (true) {
    const size_t comma = list.find(',');
    const std::string_view name = list.substr(0, comma);
    const std::optional<uint8_t> i = content.findPowerUp(name);
    if (!i) {
      error = "unknown power-up '" + std::string(name) +
              "' (content has: " + nameList(content.powerUps, content.powerUpCount) +
              "; or all, none)";
      return false;
    }
    set = static_cast<uint16_t>(set | (1u << *i));
    if (comma == std::string_view::npos) break;
    list.remove_prefix(comma + 1);
  }
  powerUps = set;
  return true;
}

std::string powerUpListText(const sim::ContentDB& content, uint16_t powerUps) {
  const uint8_t count = std::min<uint8_t>(content.powerUpCount, sim::kMaxPowerUps);
  const uint32_t all = (uint32_t{1} << count) - 1u;
  const uint32_t set = powerUps & all;
  if (set == all) return "all";
  if (set == 0) return "none";
  std::string s;
  for (uint8_t i = 0; i < count; ++i) {
    if ((set & (1u << i)) == 0) continue;
    if (!s.empty()) s += ',';
    s += content.powerUps[i].name.view();
  }
  return s;
}

ContentWatcher::ContentWatcher(std::filesystem::path dir, double intervalSeconds)
    : dir_(std::move(dir)), intervalSeconds_(intervalSeconds), stamp_(contentDirStamp(dir_)) {}

bool ContentWatcher::poll(double nowSeconds) {
  if (dir_.empty() || nowSeconds < nextCheck_) return false;
  nextCheck_ = nowSeconds + intervalSeconds_;
  const uint64_t stamp = contentDirStamp(dir_);
  if (stamp == stamp_) return false;
  stamp_ = stamp;
  return true;
}

}  // namespace kfm::content
