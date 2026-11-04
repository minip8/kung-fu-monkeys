// AgentHost (see agent_host.hpp).
#include "kfm/ai/agent_host.hpp"

#include "kfm/ai/observation_builder.hpp"
#include "kfm/core/assert.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace kfm::ai {

const AgentHost::Slot* AgentHost::slotAt(PlayerId slot) const {
  if (!slot.isValid() || slot.value >= sim::kMaxPlayers) return nullptr;
  return &slots_[slot.value];
}

void AgentHost::attach(PlayerId slot, std::unique_ptr<IAgent> agent, uint64_t seed,
                       uint8_t delayTicks) {
  if (agent == nullptr || slotAt(slot) == nullptr) return;
  Slot& s = slots_[slot.value];
  s = Slot{};
  s.agent = std::move(agent);
  s.delay = std::min(delayTicks, kMaxAgentDelayTicks);
  s.agent->init(seed, slot);
}

std::unique_ptr<IAgent> AgentHost::detach(PlayerId slot) {
  if (slotAt(slot) == nullptr) return nullptr;
  Slot& s = slots_[slot.value];
  std::unique_ptr<IAgent> agent = std::move(s.agent);
  s = Slot{};
  return agent;
}

IAgent* AgentHost::agent(PlayerId slot) const {
  const Slot* s = slotAt(slot);
  return s != nullptr ? s->agent.get() : nullptr;
}

uint8_t AgentHost::delayTicks(PlayerId slot) const {
  const Slot* s = slotAt(slot);
  return s != nullptr ? s->delay : 0;
}

sim::PlayerMask AgentHost::slots() const {
  sim::PlayerMask mask = 0;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if (slots_[i].agent != nullptr) mask = static_cast<sim::PlayerMask>(mask | (1u << i));
  }
  return mask;
}

AgentSlotStats AgentHost::stats(PlayerId slot) const {
  const Slot* s = slotAt(slot);
  return s != nullptr ? s->stats : AgentSlotStats{};
}

void AgentHost::fill(sim::InputFrame& frame, const sim::WorldState& world,
                     const sim::MatchContent& content) {
  KFM_ASSERT(frame.tick == world.tick);
  bool hashed = false;
  uint32_t hash = 0;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    Slot& s = slots_[i];
    const PlayerId id{i};
    if (s.agent == nullptr || !sim::maskHas(frame.present, id)) continue;
    if (!hashed) {  // once per tick, and only when some agent plays
      hash = sim::contentHash(content.db);
      hashed = true;
    }
    buildObservation(world, content, hash, id, s.delay, obs_);
    const std::optional<sim::PlayerInput> answer = s.agent->act(obs_, world.tick);
    ++s.stats.acts;
    if (!answer.has_value()) ++s.stats.substitutions;

    const uint32_t lineLength = uint32_t{s.delay} + 1;
    s.ring[s.fills % lineLength] = answer;
    if (s.fills >= s.delay) {
      // The answer to the observation `delay` acts ago is due; a missing one
      // (nullopt) repeats the last applied input.
      const std::optional<sim::PlayerInput>& due = s.ring[(s.fills - s.delay) % lineLength];
      if (due.has_value()) s.last = *due;
    }
    ++s.fills;
    frame.inputs[i] = s.last;
  }
}

}  // namespace kfm::ai
