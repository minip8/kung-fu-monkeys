// xoshiro128** deterministic PRNG. Lives inside WorldState (architecture
// §5.1): 16 bytes of state, trivially copyable, no OS entropy anywhere.
#pragma once

#include "kfm/core/fixed.hpp"

#include <array>
#include <bit>
#include <cstdint>

namespace kfm {

struct Rng {
  std::array<uint32_t, 4> s{1, 2, 3, 4};

  // Seeds the state through splitmix32 so nearby seeds give unrelated streams.
  [[nodiscard]] static constexpr Rng seeded(uint64_t seed) {
    Rng r;
    uint32_t x = static_cast<uint32_t>(seed) ^ static_cast<uint32_t>(seed >> 32) * 0x9E3779B9u;
    for (uint32_t& word : r.s) {
      x += 0x9E3779B9u;
      uint32_t z = x;
      z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
      z = (z ^ (z >> 13)) * 0xC2B2AE35u;
      word = z ^ (z >> 16);
    }
    if (r.s[0] == 0 && r.s[1] == 0 && r.s[2] == 0 && r.s[3] == 0) r.s[0] = 1;
    return r;
  }

  constexpr uint32_t next() {
    const uint32_t result = std::rotl(s[1] * 5u, 7) * 9u;
    const uint32_t t = s[1] << 9;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = std::rotl(s[3], 11);
    return result;
  }

  // Uniform in [0, n). n == 0 yields 0.
  constexpr uint32_t below(uint32_t n) {
    return static_cast<uint32_t>((static_cast<uint64_t>(next()) * n) >> 32);
  }

  // Uniform in [lo, hi], inclusive. Requires lo <= hi.
  constexpr int32_t range(int32_t lo, int32_t hi) {
    const auto span = static_cast<uint32_t>(static_cast<int64_t>(hi) - lo + 1);
    return static_cast<int32_t>(lo + static_cast<int64_t>(below(span)));
  }

  // Uniform Fixed in [0, 1).
  constexpr Fixed unit() { return Fixed::fromRaw(static_cast<int32_t>(next() >> 16)); }

  // Uniform Fixed in [lo, hi).
  constexpr Fixed between(Fixed lo, Fixed hi) { return lo + (hi - lo) * unit(); }

  // True with probability p, p in [0, 1].
  constexpr bool chance(Fixed p) { return unit() < p; }

  constexpr Angle angle() { return Angle::fromBam(static_cast<uint16_t>(next() >> 16)); }

  constexpr auto operator<=>(const Rng&) const = default;
};

}  // namespace kfm
