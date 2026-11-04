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

AgentHost::Slot* AgentHost::slotAt(PlayerId slot) {
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

void AgentHost::clear() {
  for (Slot& s : slots_)
    s = Slot{};
}

void AgentHost::reinit(PlayerId slot, uint64_t seed) {
  Slot* s = slotAt(slot);
  if (s == nullptr || s->agent == nullptr) return;
  s->fills = 0;
  s->next = 0;
  s->last = {};
  s->ring = {};
  s->agent->init(seed, slot);
}

void AgentHost::setDelay(PlayerId slot, uint8_t delayTicks) {
  Slot* s = slotAt(slot);
  if (s == nullptr || s->agent == nullptr) return;
  s->delay = std::min(delayTicks, kMaxAgentDelayTicks);
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

    s.ring[s.fills % s.ring.size()] = answer;
    if (s.fills >= s.delay) {
      // The answer to the observation `delay` acts ago is due. After the
      // delay grew it was applied already (hold); after it shrank the ones
      // before it are skipped. A missing answer (nullopt) repeats the last
      // applied input.
      const uint32_t due = s.fills - s.delay;
      if (due >= s.next) {
        const std::optional<sim::PlayerInput>& in = s.ring[due % s.ring.size()];
        if (in.has_value()) s.last = *in;
        s.next = due + 1;
      }
    }
    ++s.fills;
    frame.inputs[i] = s.last;
  }
}

}  // namespace kfm::ai
