#include "kfm/platform/platform_services.hpp"

#include <algorithm>
#include <utility>

namespace kfm::platform {

namespace {

constexpr std::size_t kGuestIdLength = 32;

constexpr std::string_view kGuestIdPref = "guest_id";

[[nodiscard]] Download refusedDownload() {
  return Download{.state = Download::State::Failed,
                  .bytes = {},
                  .error = "this platform cannot download anything"};
}

[[nodiscard]] uint64_t splitmix64(uint64_t& state) {
  uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

void appendHex(std::string& out, uint64_t v) {
  constexpr std::string_view kHex = "0123456789abcdef";
  for (int shift = 60; shift >= 0; shift -= 4)
    out.push_back(kHex[(v >> shift) & 0xF]);
}

[[nodiscard]] std::string_view trimmed(std::string_view s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
    s.remove_suffix(1);
  }
  return s;
}

}  // namespace

bool isGuestId(std::string_view id) {
  return id.size() == kGuestIdLength && std::ranges::all_of(id, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::string makeGuestId(uint64_t entropyA, uint64_t entropyB) {
  uint64_t state = entropyA ^ (entropyB * 0xD1B54A32D192ED03ULL);
  std::string id;
  id.reserve(kGuestIdLength);
  appendHex(id, splitmix64(state));
  appendHex(id, splitmix64(state));
  return id;
}

void NullPlatformServices::startDownload(std::string_view /*source*/, std::size_t /*maxBytes*/) {
  download_ = refusedDownload();
}

Download NullPlatformServices::pollDownload() {
  return std::exchange(download_, Download{});
}

LocalPlatformServices::LocalPlatformServices(PlatformHooks hooks, std::string lobbyUrl,
                                             uint64_t entropyA, uint64_t entropyB)
    : hooks_(std::move(hooks)),
      lobbyUrl_(std::move(lobbyUrl)),
      entropyA_(entropyA),
      entropyB_(entropyB) {}

std::string LocalPlatformServices::guestId() {
  if (!id_.empty()) return id_;
  const std::string stored = loadPref(kGuestIdPref);
  if (isGuestId(trimmed(stored))) {
    id_ = trimmed(stored);
    return id_;
  }
  id_ = makeGuestId(entropyA_, entropyB_);
  savePref(kGuestIdPref, id_);
  return id_;
}

bool LocalPlatformServices::copyText(std::string_view text) {
  return hooks_.share.copy && hooks_.share.copy(text);
}

bool LocalPlatformServices::canShare() {
  return hooks_.share.canShare && hooks_.share.share && hooks_.share.canShare();
}

bool LocalPlatformServices::share(std::string_view title, std::string_view text,
                                  std::string_view url) {
  return canShare() && hooks_.share.share(title, text, url);
}

std::string LocalPlatformServices::loadPref(std::string_view name) {
  return hooks_.prefs.load ? hooks_.prefs.load(name) : std::string{};
}

void LocalPlatformServices::savePref(std::string_view name, std::string_view text) {
  if (hooks_.prefs.save) hooks_.prefs.save(name, text);
}

void LocalPlatformServices::startDownload(std::string_view source, std::size_t maxBytes) {
  const DownloadHooks& d = hooks_.download;
  if (d.start && d.poll) {
    refused_ = {};
    d.start(source, maxBytes);
  } else {
    refused_ = refusedDownload();
  }
}

Download LocalPlatformServices::pollDownload() {
  if (refused_.state != Download::State::None) return std::exchange(refused_, Download{});
  return hooks_.download.poll ? hooks_.download.poll() : Download{};
}

void LocalPlatformServices::cancelDownload() {
  refused_ = {};
  if (hooks_.download.cancel) hooks_.download.cancel();
}

}  // namespace kfm::platform
