// How the apps find, load and watch content (architecture §5.10), shared by
// kfm_client, kfm_server, kfm_replay, kfm_netsim and kfm_render_probe so
// they all resolve `--content`, `--arena` and `--mode` the same way.
#pragma once

#include "kfm/content/diagnostics.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/world_state.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace kfm::content {

struct LoadedContent {
  sim::ContentDB db{};
  std::filesystem::path dir;  // the directory it came from; empty = embedded
  uint32_t hash{0};           // sim::contentHash(db)

  [[nodiscard]] bool embedded() const { return dir.empty(); }
  // "content: <dir> (hash 0x1234abcd, 1 arena, 1 mode)" or "content: embedded (...)".
  [[nodiscard]] std::string describe() const;
};

// The directory a command line names, or else <exeDir>/content when it
// holds a *.toml (CMake copies content/ there, kfm_use_content), or else
// none (empty path): use the embedded copy. Under Emscripten always none.
[[nodiscard]] std::filesystem::path findContentDir(std::string_view cliDir,
                                                   const std::filesystem::path& exeDir);

// Loads from findContentDir(), or the embedded copy when that is empty. An
// explicit --content that fails to load is an error (no silent fallback).
[[nodiscard]] bool loadAppContent(std::string_view cliDir, const std::filesystem::path& exeDir,
                                  LoadedContent& out, Diagnostics& diag);

// The directory of the running executable from argv[0] (empty when unknown).
[[nodiscard]] std::filesystem::path executableDir(const char* argv0);

// Resolves --arena / --mode names into config.arena / config.mode. An empty
// name keeps index 0 (the default). On an unknown name returns false with a
// message that lists the names the content defines.
[[nodiscard]] bool resolveMatchNames(const sim::ContentDB& content, std::string_view arena,
                                     std::string_view mode, sim::MatchConfig& config,
                                     std::string& error);

// Hot reload polling (dev builds, local play only): call poll() every
// frame; it stats the directory at most every intervalSeconds and reports a
// change of contentDirStamp() since the previous check.
class ContentWatcher {
 public:
  ContentWatcher() = default;
  explicit ContentWatcher(std::filesystem::path dir, double intervalSeconds = 1.0);

  [[nodiscard]] bool active() const { return !dir_.empty(); }
  [[nodiscard]] const std::filesystem::path& dir() const { return dir_; }
  [[nodiscard]] bool poll(double nowSeconds);

 private:
  std::filesystem::path dir_;
  double intervalSeconds_{1.0};
  double nextCheck_{0.0};
  uint64_t stamp_{0};
};

}  // namespace kfm::content
