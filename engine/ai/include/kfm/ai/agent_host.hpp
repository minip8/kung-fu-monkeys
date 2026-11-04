// AgentHost (architecture §8): the host-side slot runner shared by the
// server (bots write straight into the authoritative frame), the client
// (bots as local players) and kfm_arena (the recorded frame). It owns up to
// kMaxPlayers agents, one per slot, and each tick builds every agent's
// observation, calls act() and writes the input due this tick into the frame.
//
// Per slot:
//   - an input delay in ticks, the "human-likeness" host setting: 0 bot
//     versus bot, the humans' lead in a mixed match. The answer to the
//     observation of tick T is applied on tick T + delay (idle until the first
//     answer is due); the delay is in the observation, so the bot can predict
//     ahead;
//   - the repeat-last rule: act() returning nullopt (a wasm trap or an
//     over-budget tick) applies the slot's last applied input again, exactly
//     the server's rule for a late network input (§6.1);
//   - counters for kfm_arena's result line: acts and substitutions.
//
// A new match: reinit() calls the agent's init() again with a fresh seed and
// empties the delay line (idle until the first answer is due), keeping the
// delay and the counters, which are per attach.
//
// Ownership: the host owns its agents (std::unique_ptr); attach() hands one
// over and detach() hands it back. Allocation happens only in attach/detach;
// fill() allocates nothing and never writes the world.
#pragma once

#include "kfm/ai/agent.hpp"
#include "kfm/ai/observation.hpp"
#include "kfm/core/types.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/world_state.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace kfm::ai {

// The largest per-slot input delay; attach() clamps to it. Two seconds is far
// beyond any lead the client runs at (§6.2).
inline constexpr uint8_t kMaxAgentDelayTicks = 120;

struct AgentSlotStats {
  uint32_t acts{0};           // act() calls (one per present tick)
  uint32_t substitutions{0};  // of them, the ones that returned nullopt (repeat last input)
};

class AgentHost {
 public:
  AgentHost() = default;

  // Puts `agent` in `slot` (replacing and destroying an agent already there),
  // calls agent->init(seed, slot) and resets the slot's delay line, last
  // input and counters. A null agent or an invalid slot is ignored.
  void attach(PlayerId slot, std::unique_ptr<IAgent> agent, uint64_t seed, uint8_t delayTicks = 0);
  // Removes the agent from `slot` and returns it (null when there was none).
  std::unique_ptr<IAgent> detach(PlayerId slot);
  // Destroys every agent.
  void clear();
  // Calls init(seed, slot) on the agent in `slot` again and empties its delay
  // line and last input; the delay and the counters stay. No-op without an
  // agent.
  void reinit(PlayerId slot, uint64_t seed);
  // Changes the slot's input delay (clamped to kMaxAgentDelayTicks) from the
  // next fill() on; see the header comment. No-op without an agent.
  void setDelay(PlayerId slot, uint8_t delayTicks);

  [[nodiscard]] bool has(PlayerId slot) const { return agent(slot) != nullptr; }
  [[nodiscard]] IAgent* agent(PlayerId slot) const;
  [[nodiscard]] uint8_t delayTicks(PlayerId slot) const;
  // Bit i: an agent sits in slot i.
  [[nodiscard]] sim::PlayerMask slots() const;
  // Zero counters for a slot without an agent.
  [[nodiscard]] AgentSlotStats stats(PlayerId slot) const;

  // For every slot holding an agent and present in frame.present, in slot
  // order: builds the observation of `world` for that slot (with its delay),
  // calls act(obs, world.tick), queues the answer and writes the input due
  // this tick into frame.inputs[slot]. Other slots of the frame are left
  // untouched, so humans' and network inputs can be filled before or after.
  // A slot not present this tick is skipped entirely (no act, its delay line
  // does not advance). Precondition: frame.tick == world.tick.
  void fill(sim::InputFrame& frame, const sim::WorldState& world, const sim::MatchContent& content);

  // The observation fill() built last (for tools and tests).
  [[nodiscard]] const Observation& lastObservation() const { return obs_; }

 private:
  struct Slot {
    std::unique_ptr<IAgent> agent;
    uint8_t delay{0};
    uint32_t fills{0};  // act() calls since attach or reinit: the delay line's clock
    uint32_t next{0};   // the index of the first answer not applied (or skipped) yet
    sim::PlayerInput last{};
    AgentSlotStats stats{};
    // ring[n % ring.size()] holds the answer of the n-th act(): the last
    // kMaxAgentDelayTicks + 1 answers, whatever the delay, so a delay change
    // never loses one that is still due.
    std::array<std::optional<sim::PlayerInput>, kMaxAgentDelayTicks + 1> ring{};
  };
  [[nodiscard]] const Slot* slotAt(PlayerId slot) const;
  [[nodiscard]] Slot* slotAt(PlayerId slot);

  std::array<Slot, sim::kMaxPlayers> slots_{};
  Observation obs_{};
};

}  // namespace kfm::ai
