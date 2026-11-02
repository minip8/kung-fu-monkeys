// Hashing: XXH32 for content hashing (matches the reference xxHash
// algorithm bit-for-bit, so content_hash can be recomputed by tools in any
// language) and a small streaming 32-bit mixer for per-tick state checksums.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace kfm {

namespace detail {
inline constexpr uint32_t kPrime32P1 = 0x9E3779B1u;
inline constexpr uint32_t kPrime32P2 = 0x85EBCA77u;
inline constexpr uint32_t kPrime32P3 = 0xC2B2AE3Du;
inline constexpr uint32_t kPrime32P4 = 0x27D4EB2Fu;
inline constexpr uint32_t kPrime32P5 = 0x165667B1u;

constexpr uint32_t xxh32Round(uint32_t acc, uint32_t input) {
  acc += input * kPrime32P2;
  acc = std::rotl(acc, 13);
  acc *= kPrime32P1;
  return acc;
}

constexpr uint32_t xxh32Avalanche(uint32_t h) {
  h ^= h >> 15;
  h *= kPrime32P2;
  h ^= h >> 13;
  h *= kPrime32P3;
  h ^= h >> 16;
  return h;
}

constexpr uint32_t readLE32(std::span<const std::byte> p, size_t i) {
  return static_cast<uint32_t>(p[i]) | (static_cast<uint32_t>(p[i + 1]) << 8) |
         (static_cast<uint32_t>(p[i + 2]) << 16) | (static_cast<uint32_t>(p[i + 3]) << 24);
}
}  // namespace detail

// Reference XXH32 over a byte span.
[[nodiscard]] constexpr uint32_t xxh32(std::span<const std::byte> data, uint32_t seed = 0) {
  using namespace detail;
  const size_t len = data.size();
  size_t i = 0;
  uint32_t h = 0;
  if (len >= 16) {
    uint32_t v1 = seed + kPrime32P1 + kPrime32P2;
    uint32_t v2 = seed + kPrime32P2;
    uint32_t v3 = seed;
    uint32_t v4 = seed - kPrime32P1;
    while (i + 16 <= len) {
      v1 = xxh32Round(v1, readLE32(data, i));
      v2 = xxh32Round(v2, readLE32(data, i + 4));
      v3 = xxh32Round(v3, readLE32(data, i + 8));
      v4 = xxh32Round(v4, readLE32(data, i + 12));
      i += 16;
    }
    h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
  } else {
    h = seed + kPrime32P5;
  }
  h += static_cast<uint32_t>(len);
  while (i + 4 <= len) {
    h += readLE32(data, i) * kPrime32P3;
    h = std::rotl(h, 17) * kPrime32P4;
    i += 4;
  }
  while (i < len) {
    h += static_cast<uint32_t>(data[i]) * kPrime32P5;
    h = std::rotl(h, 11) * kPrime32P1;
    ++i;
  }
  return xxh32Avalanche(h);
}

// Streaming mixer for state checksums. Feed 32-bit words in a fixed order;
// finish() gives a well-avalanched 32-bit digest. Not XXH32-compatible, but
// deterministic and cheap: one xxh32 round per word.
class Hash32 {
 public:
  constexpr Hash32() = default;
  constexpr explicit Hash32(uint32_t seed) : acc_(seed + detail::kPrime32P5) {}

  constexpr void add(uint32_t word) {
    acc_ = detail::xxh32Round(acc_, word);
    ++count_;
  }
  constexpr void add(int32_t word) { add(static_cast<uint32_t>(word)); }
  constexpr void add(uint64_t word) {
    add(static_cast<uint32_t>(word));
    add(static_cast<uint32_t>(word >> 32));
  }

  [[nodiscard]] constexpr uint32_t finish() const { return detail::xxh32Avalanche(acc_ + count_); }

 private:
  uint32_t acc_{detail::kPrime32P5};
  uint32_t count_{0};
};

}  // namespace kfm
