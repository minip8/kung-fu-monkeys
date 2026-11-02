#include "kfm/core/ring_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

using kfm::RingBuffer;

TEST_CASE("RingBuffer push, index, overwrite", "[core][ring]") {
  STATIC_REQUIRE(std::is_trivially_copyable_v<RingBuffer<int, 4>>);
  RingBuffer<int, 4> r{};
  CHECK(r.empty());
  r.push(1);
  r.push(2);
  r.push(3);
  CHECK(r.size() == 3);
  CHECK(r.front() == 1);
  CHECK(r.back() == 3);
  r.push(4);
  CHECK(r.full());
  r.push(5);  // overwrites 1
  CHECK(r.size() == 4);
  CHECK(r[0] == 2);
  CHECK(r[1] == 3);
  CHECK(r[2] == 4);
  CHECK(r[3] == 5);
  CHECK(r.popFront() == 2);
  CHECK(r.size() == 3);
  CHECK(r.front() == 3);
  r.clear();
  CHECK(r.empty());
}
