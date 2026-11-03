// Placeholder: the parallel M1 work replaces this file's contents with
// end-to-end catch scenarios (tags [scenario][catch]).
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

TEST_CASE("catch scenario file is registered", "[scenario][catch]") {
  const WorldState s = makePlayingWorld(1, 0b11);
  CHECK(s.players.size() == 2);
}
