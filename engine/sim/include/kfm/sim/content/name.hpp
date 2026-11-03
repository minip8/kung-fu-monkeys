// ContentName: a def's name as TOML spells it (architecture §5.10). Its own
// header so the per-def headers under content/ can use it without including
// content.hpp.
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace kfm::sim {

// Up to kMaxChars characters, NUL padded, so it is fixed-size, trivially
// copyable and hashed byte for byte. Names are how content files, command
// lines and the lobby refer to arenas and modes; the sim only ever sees
// indices.
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

}  // namespace kfm::sim
