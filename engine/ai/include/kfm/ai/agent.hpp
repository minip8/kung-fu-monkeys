// IAgent (architecture §8): the seam every bot sits behind, the built-in
// reference bot and WasmAgent alike. An agent is fed a versioned POD
// Observation each tick and answers with a PlayerInput, the same action
// space a human has (§5.4). Events reach a bot folded into the observation,
// never through a callback. Agents live outside the sim: the host builds the
// observation from the WorldState, calls act(), and writes the input into
// the frame it steps (the authoritative frame on the server, the local frame
// in the client, the recorded frame in kfm_arena).
#pragma once

#include "kfm/ai/observation.hpp"
#include "kfm/core/types.hpp"
#include "kfm/sim/input.hpp"

#include <cstdint>
#include <optional>

namespace kfm::ai {

struct IAgent {
  IAgent() = default;
  IAgent(const IAgent&) = default;
  IAgent& operator=(const IAgent&) = default;
  IAgent(IAgent&&) = default;
  IAgent& operator=(IAgent&&) = default;
  virtual ~IAgent() = default;

  // Called once when the agent takes a slot in a match (before the first act()).
  virtual void init(uint64_t seed, PlayerId slot) = 0;
  // The input for `tick`, given the observation the host built for it.
  // nullopt = the agent produced no input this tick (a wasm module that
  // trapped or ran over its fuel budget): the host substitutes "repeat last
  // input", exactly the server's rule for a late network input (§6.1), and
  // counts the substitution.
  virtual std::optional<sim::PlayerInput> act(const Observation& obs, Tick tick) = 0;
};

}  // namespace kfm::ai
