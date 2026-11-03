#include "kfm/content/decimal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using kfm::Fixed;
using kfm::content::parseFixedDecimal;

namespace {

int32_t raw(std::string_view text, int32_t divisor = 1) {
  const std::optional<Fixed> f = parseFixedDecimal(text, divisor);
  REQUIRE(f.has_value());
  return f.value_or(Fixed::min()).raw;
}

}  // namespace

TEST_CASE("decimals convert exactly to Q16.16, truncating toward zero", "[content][decimal]") {
  CHECK(raw("0") == 0);
  CHECK(raw("1") == 65536);
  CHECK(raw("16") == 16 * 65536);
  CHECK(raw("0.5") == 32768);
  CHECK(raw("-0.5") == -32768);
  CHECK(raw("+2.25") == 2 * 65536 + 16384);
  // 0.35 * 65536 = 22937.6: truncated, exactly Fixed::fromRatio(35, 100)
  // (the M1 constexpr boomerang radius). Round-half-up would give 22938.
  CHECK(raw("0.35") == 22937);
  CHECK(raw("0.35") == Fixed::fromRatio(35, 100).raw);
  CHECK(raw("-0.35") == -22937);
  CHECK(raw("-0.35") == Fixed::fromRatio(-35, 100).raw);
}

TEST_CASE("decimal digits beyond the resolution truncate", "[content][decimal]") {
  // 1/65536 = 0.0000152587890625; 9 places is the cap.
  CHECK(raw("0.000015258") == 0);  // 0.99997... raw steps
  CHECK(raw("0.000015259") == 1);  // 1.00003... raw steps
  CHECK(raw("-0.000015259") == -1);
  CHECK(raw("0.999999999") == 65535);
  CHECK(raw("1.000000000") == 65536);
}

TEST_CASE("per-second values divide by the tick rate in one rounding", "[content][decimal]") {
  CHECK(raw("22", 60) == Fixed::fromRatio(22, 60).raw);
  CHECK(raw("16", 60) == Fixed::fromRatio(16, 60).raw);
  CHECK(raw("7", 60) == Fixed::fromRatio(7, 60).raw);
  CHECK(raw("7.5", 60) == 8192);  // 0.125 exactly
  // n * 65536 / (10^k * 60) truncated once, not decimal-then-divide.
  CHECK(raw("0.000001", 60) == 0);
  CHECK(raw("22.0", 60) == raw("22", 60));
}

TEST_CASE("TOML digit separators and malformed literals", "[content][decimal]") {
  CHECK(raw("1_000") == 1000 * 65536);
  CHECK(raw("0.000_5") == raw("0.0005"));
  for (const char* bad :
       {"",     "-",  "+",  ".5",   "5.",   "1..2", "1e3",   "1E3", "inf", "nan",
        "+inf", "_1", "1_", "1__0", "1._5", "0x10", "1.2.3", " 1",  "1 ",  "--1"}) {
    INFO(bad);
    CHECK_FALSE(parseFixedDecimal(bad).has_value());
  }
}

TEST_CASE("decimal range and precision limits", "[content][decimal]") {
  CHECK(raw("32767.999984741") == 0x7FFF'FFFE);  // truncates
  CHECK(raw("32767.999984742") == 0x7FFF'FFFF);  // just under 32768
  CHECK_FALSE(parseFixedDecimal("32768").has_value());
  CHECK(raw("-32768") == INT32_MIN);
  CHECK(raw("-32768.00001") == INT32_MIN);  // truncates back onto the range
  CHECK_FALSE(parseFixedDecimal("-32768.0001").has_value());
  CHECK_FALSE(parseFixedDecimal("123456").has_value());
  CHECK_FALSE(parseFixedDecimal("0.1234567891").has_value());  // 10 places
  CHECK(parseFixedDecimal("0.123456789").has_value());
  CHECK(raw("00012.5") == raw("12.5"));
  CHECK_FALSE(parseFixedDecimal("1", 0).has_value());
}
