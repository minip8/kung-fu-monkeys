#include "kfm/ai/agent.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Observation carries its schema version", "[ai]") {
  const kfm::ai::Observation obs{};
  CHECK(obs.version == kfm::ai::kObservationVersion);
}
