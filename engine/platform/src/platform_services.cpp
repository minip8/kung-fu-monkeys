#include "kfm/platform/platform_services.hpp"

#include <algorithm>
#include <utility>

namespace kfm::platform {

namespace {

constexpr std::size_t kGuestIdLength = 32;

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

LocalPlatformServices::LocalPlatformServices(GuestIdStore store, std::string lobbyUrl,
                                             uint64_t entropyA, uint64_t entropyB)
    : store_(std::move(store)),
      lobbyUrl_(std::move(lobbyUrl)),
      entropyA_(entropyA),
      entropyB_(entropyB) {}

std::string LocalPlatformServices::guestId() {
  if (!id_.empty()) return id_;
  const std::string stored = store_.load ? store_.load() : std::string{};
  if (isGuestId(trimmed(stored))) {
    id_ = trimmed(stored);
    return id_;
  }
  id_ = makeGuestId(entropyA_, entropyB_);
  if (store_.save) store_.save(id_);
  return id_;
}

}  // namespace kfm::platform
