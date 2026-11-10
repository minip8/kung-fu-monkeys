// The native client's platform hooks (client_platform.hpp): SDL.
#include "client_platform.hpp"
#include "core/client_options.hpp"
#include "core/replay_session.hpp"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kfm::client {

namespace {

// Files in SDL's preference directory: "settings.txt", "guest_id.txt".
[[nodiscard]] std::filesystem::path prefPath(std::string_view name) {
  char* dir = SDL_GetPrefPath("kfm", "kung-fu-monkeys");  // created if missing
  if (dir == nullptr) return {};
  std::filesystem::path p = std::filesystem::path(dir) / (std::string(name) + ".txt");
  SDL_free(dir);
  return p;
}

[[nodiscard]] std::string loadPrefText(std::string_view name) {
  const std::filesystem::path path = prefPath(name);
  if (path.empty()) return {};
  const std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

void savePrefText(std::string_view name, std::string_view text) {
  const std::filesystem::path path = prefPath(name);
  if (path.empty()) return;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  if (!out) std::fprintf(stderr, "warning: cannot save %s\n", path.string().c_str());
}

}  // namespace

platform::PlatformHooks makePlatformHooks(bool persist) {
  platform::PlatformHooks hooks;
  if (persist) {
    hooks.prefs.load = [](std::string_view name) { return loadPrefText(name); };
    hooks.prefs.save = [](std::string_view name, std::string_view text) {
      savePrefText(name, text);
    };
  }
  hooks.share.copy = [](std::string_view text) {
    return SDL_SetClipboardText(std::string(text).c_str());
  };
  // A download is a local file, read at once: the first poll has it.
  const auto result = std::make_shared<platform::Download>();
  hooks.download.start = [result](std::string_view source, std::size_t maxBytes) {
    platform::Download d;
    if (readFileBytes(std::string(source), d.bytes, d.error, maxBytes)) {
      d.state = platform::Download::State::Done;
    } else {
      d.state = platform::Download::State::Failed;
      d.bytes.clear();
    }
    *result = std::move(d);
  };
  hooks.download.poll = [result] { return std::exchange(*result, platform::Download{}); };
  hooks.download.cancel = [result] { *result = platform::Download{}; };
  hooks.pageUrl = defaultPageUrl();  // usually empty: Copy link copies the code
  hooks.canQuit = true;
  hooks.localFiles = true;
  return hooks;
}

}  // namespace kfm::client
