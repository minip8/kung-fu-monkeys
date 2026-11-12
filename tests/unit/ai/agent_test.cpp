#include "kfm/ai/agent.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Observation carries its schema version", "[ai]") {
  STATIC_CHECK(kfm::ai::kObservationVersion == 2);
  const kfm::ai::Observation obs{};
  CHECK(obs.header.version == 0);  // value-initialised: the builder stamps it
}
