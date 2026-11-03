// Exact decimal -> Q16.16 conversion for content files (architecture §5.10).
// A decimal literal is an exact rational n / 10^k; the Fixed value is
// n * 65536 / (10^k * divisor) truncated toward zero, the same rounding as
// Fixed::fromRatio, which built the M1 constexpr content: "0.35" is exactly
// Fixed::fromRatio(35, 100) and speed 22 with divisor 60 is
// Fixed::fromRatio(22, 60). Integer arithmetic only, so every platform gets
// the same bits; a double never enters the computation.
#pragma once

#include "kfm/core/fixed.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace kfm::content {

// Most fraction digits accepted (Q16.16 resolves ~1.5e-5; 9 digits keep
// n * 65536 inside int64 for the whole Fixed range).
inline constexpr int kMaxDecimalPlaces = 9;

// Accepts [+-]digits[.digits] with TOML's '_' digit separators between
// digits; no exponent, inf or nan. divisor >= 1 (60 converts a per-second
// value to per-tick). nullopt on a malformed literal, more than
// kMaxDecimalPlaces fraction digits, or a result outside the Fixed range.
[[nodiscard]] std::optional<Fixed> parseFixedDecimal(std::string_view text, int32_t divisor = 1);

}  // namespace kfm::content
