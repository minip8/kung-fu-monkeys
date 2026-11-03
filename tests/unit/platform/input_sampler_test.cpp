#include "kfm/platform/input_sampler.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("platform placeholder", "[platform]") {
  REQUIRE(kfm::platform::placeholder() == 1);
}
