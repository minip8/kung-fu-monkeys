#include "kfm/core/field_count.hpp"

#include "kfm/core/fixed.hpp"
#include "kfm/core/pool.hpp"
#include "kfm/core/vec2.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace {

struct Empty {};
struct One {
  int32_t a{0};
};
struct Scalars {
  uint8_t a{0};
  uint16_t b{0};
  int32_t c{0};
  uint64_t d{0};
  bool e{false};
};
enum class Kind : uint8_t { A, B };
// Arrays and nested aggregates are one field each: a placeholder initialises
// the member directly, so braces are never elided into it.
struct Nested {
  One one{};
  std::array<uint8_t, 7> bytes{};
  std::array<One, 3> ones{};
  kfm::Vec2 pos{};
  Kind kind{Kind::A};
  kfm::Fixed f{};
};
struct NoInitialisers {
  int32_t a;
  int32_t b;
  int32_t c;
};
struct StaticsDoNotCount {
  static constexpr int32_t kLimit = 4;
  int32_t a{kLimit};
  [[nodiscard]] constexpr int32_t twice() const { return 2 * a; }
};

}  // namespace

static_assert(kfm::fieldCount<Empty>() == 0);
static_assert(kfm::fieldCount<One>() == 1);
static_assert(kfm::fieldCount<Scalars>() == 5);
static_assert(kfm::fieldCount<Nested>() == 6);
static_assert(kfm::fieldCount<NoInitialisers>() == 3);
static_assert(kfm::fieldCount<StaticsDoNotCount>() == 1);
static_assert(kfm::fieldCount<kfm::Vec2>() == 2);
static_assert(kfm::fieldCount<kfm::Pool<One, 70>>() == 4);

TEST_CASE("fieldCount counts the direct members of an aggregate", "[core][field_count]") {
  // The asserts above are the test; this keeps the file a visible test case.
  CHECK(kfm::fieldCount<Nested>() == 6);
}
