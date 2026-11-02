#include "kfm/core/hash.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <span>
#include <string_view>
#include <vector>

using kfm::Hash32;
using kfm::xxh32;

namespace {
std::vector<std::byte> bytesOf(std::string_view s) {
  std::vector<std::byte> v(s.size());
  if (!s.empty()) std::memcpy(v.data(), s.data(), s.size());
  return v;
}
}  // namespace

TEST_CASE("xxh32 matches the reference test vectors", "[core][hash]") {
  CHECK(xxh32(bytesOf("")) == 0x02CC5D05u);
  CHECK(xxh32(bytesOf("a")) == 0x550D7456u);
  CHECK(xxh32(bytesOf("abc")) == 0x32D153FFu);
  CHECK(xxh32(bytesOf("Nobody inspects the spammish repetition")) == 0xE2293B2Fu);
  CHECK(xxh32(bytesOf("The quick brown fox jumps over the lazy dog")) == 0xE85EA4DEu);
  CHECK(xxh32(bytesOf("abc"), 1) == 0xAA3DA8FFu);
}

TEST_CASE("Hash32 is order-sensitive and stable", "[core][hash]") {
  Hash32 a;
  a.add(1u);
  a.add(2u);
  Hash32 b;
  b.add(2u);
  b.add(1u);
  CHECK(a.finish() != b.finish());
  Hash32 c;
  c.add(1u);
  c.add(2u);
  CHECK(a.finish() == c.finish());
  CHECK(Hash32{}.finish() != Hash32{1}.finish());
  Hash32 d;
  d.add(uint64_t{0x0000000200000001ULL});
  CHECK(d.finish() == a.finish());
}
