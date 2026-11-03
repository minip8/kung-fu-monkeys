// Placeholder: the parallel M1 work for this system replaces this file's
// contents (tags [sim][interaction]).
#include "kfm/sim/rules/interaction.hpp"

#include "sim_test_util.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

TEST_CASE("interaction test file is registered", "[sim][interaction]") {
  CHECK(interactionRules().size() == 4);
}
