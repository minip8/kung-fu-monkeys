#include "kfm/content/decimal.hpp"

#include <cstddef>
#include <cstdint>

namespace kfm::content {
namespace {

constexpr bool isDigit(char c) {
  return c >= '0' && c <= '9';
}

}  // namespace

std::optional<Fixed> parseFixedDecimal(std::string_view text, int32_t divisor) {
  if (divisor < 1) return std::nullopt;
  size_t i = 0;
  bool negative = false;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
    negative = text[i] == '-';
    ++i;
  }
  // Integer part: up to 5 digits (|value| < 2^15 = 32768 anyway), then an
  // optional fraction of up to kMaxDecimalPlaces digits. '_' only between
  // two digits, as in TOML.
  uint64_t mantissa = 0;
  int intDigits = 0;
  int fracDigits = 0;
  bool inFraction = false;
  bool lastWasDigit = false;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (isDigit(c)) {
      if (inFraction) {
        if (++fracDigits > kMaxDecimalPlaces) return std::nullopt;
      } else {
        // Leading zeros are harmless; the magnitude check is below.
        if (mantissa != 0 || c != '0') ++intDigits;
        if (intDigits > 5) return std::nullopt;
      }
      mantissa = mantissa * 10 + static_cast<uint64_t>(c - '0');
      lastWasDigit = true;
    } else if (c == '_') {
      if (!lastWasDigit || i + 1 >= text.size() || !isDigit(text[i + 1])) return std::nullopt;
      lastWasDigit = false;
    } else if (c == '.') {
      if (inFraction || !lastWasDigit || i + 1 >= text.size() || !isDigit(text[i + 1])) {
        return std::nullopt;
      }
      inFraction = true;
      lastWasDigit = false;
    } else {
      return std::nullopt;  // exponent, inf, nan, anything else
    }
  }
  if (!lastWasDigit) return std::nullopt;  // empty, sign only, or trailing '_' / '.'

  // mantissa < 10^14 < 2^47, so mantissa << 16 < 2^63. The denominator is
  // at most 10^9 * 2^31 < 2^62.
  uint64_t denominator = static_cast<uint64_t>(divisor);
  for (int d = 0; d < fracDigits; ++d)
    denominator *= 10;
  const uint64_t magnitude = (mantissa << Fixed::kShift) / denominator;  // truncates
  const uint64_t limit = negative ? uint64_t{0x8000'0000} : uint64_t{0x7FFF'FFFF};
  if (magnitude > limit) return std::nullopt;
  const int64_t raw = negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
  return Fixed::fromRaw(static_cast<int32_t>(raw));
}

}  // namespace kfm::content
