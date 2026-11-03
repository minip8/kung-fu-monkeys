// Placeholder: the parallel M1 work for this system replaces this file's
// contents (tags [sim][ability]).
#include "kfm/sim/abilities/registry.hpp"
#include "sim_test_util.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

TEST_CASE("ability test file is registered", "[sim][ability]") {
  STATIC_REQUIRE(kAbilitySlots == 2);
  CHECK(behaviorFor(AbilityKind::Dash).canActivate != nullptr);
}
