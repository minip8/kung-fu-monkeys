#include "kfm/content/app_content.hpp"

#include "kfm/content/loader.hpp"

#include <cstdio>
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
  config = c;
  return true;
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
