#include "kfm/presentation/render_state.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("presentation placeholder", "[presentation]") {
  REQUIRE(kfm::presentation::placeholder() == 1);
}
