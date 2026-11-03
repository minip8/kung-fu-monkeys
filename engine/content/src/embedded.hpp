// The content/ directory compiled into the binary (architecture §5.10). The
// definition, embedded.cpp, is generated at build time by EmbedContent.cmake
// from content/**/*.toml in sorted order; it is never checked in.
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace kfm::content::detail {

struct EmbeddedFile {
  std::string_view path;  // relative to content/, '/' separators
  const unsigned char* bytes;
  size_t size;

  [[nodiscard]] std::string_view text() const {
    return {reinterpret_cast<const char*>(bytes), size};  // NOLINT(*-reinterpret-cast)
  }
};

[[nodiscard]] std::span<const EmbeddedFile> embeddedFiles();

}  // namespace kfm::content::detail
