// C++23 toolchain probe (architecture §9). Compiled with CXX_STANDARD 23 on
// the full CI matrix. If it builds and runs everywhere, the baseline moves
// to C++23. It exercises exactly the features the design would use:
// std::expected for fallible decoders and deducing `this` for CRTP-free
// visitors and pools.
#include <cstdint>
#include <cstdio>
#include <expected>
#include <string_view>
#include <utility>

namespace {

enum class DecodeError : uint8_t { Empty, BadMagic };

std::expected<uint32_t, DecodeError> decode(std::string_view s) {
  if (s.empty()) return std::unexpected(DecodeError::Empty);
  if (s.front() != 'K') return std::unexpected(DecodeError::BadMagic);
  return static_cast<uint32_t>(s.size());
}

struct Counter {
  uint32_t value{0};

  // Deducing this: one definition serves const and non-const callers.
  template <class Self>
  auto&& get(this Self&& self) {
    return std::forward<Self>(self).value;
  }

  // Recursive lambda without std::function, via deducing this.
  static uint32_t sum(uint32_t n) {
    auto rec = [](this auto&& self, uint32_t k) -> uint32_t {
      return k == 0 ? 0 : k + self(k - 1);
    };
    return rec(n);
  }
};

}  // namespace

int main() {
  const auto ok = decode("KFMR");
  const auto bad = decode("XXXX");
  const auto empty = decode("");
  if (!ok || *ok != 4) return 1;
  if (bad || bad.error() != DecodeError::BadMagic) return 2;
  if (empty || empty.error() != DecodeError::Empty) return 3;

  Counter c;
  c.get() = 7;
  const Counter& cc = c;
  if (cc.get() != 7) return 4;
  if (Counter::sum(10) != 55) return 5;
  if (std::to_underlying(DecodeError::BadMagic) != 1) return 6;

  std::puts("cxx23 probe: std::expected, deducing this, std::to_underlying OK");
  return 0;
}
