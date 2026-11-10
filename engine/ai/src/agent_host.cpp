// AgentHost (see agent_host.hpp).
#include "kfm/ai/agent_host.hpp"

#include "kfm/ai/observation_builder.hpp"
#include "kfm/ai/vision.hpp"
#include "kfm/core/assert.hpp"
#include "kfm/sim/spaces/grid_cells.hpp"
#include "kfm/sim/step.hpp"

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

void AgentHost::restartLine(Slot& s) {
  s.fills = 0;
  s.next = 0;
  s.last = {};
  s.ring = {};
  s.seen.clear();
  s.heardCount = 0;
}

void AgentHost::attach(PlayerId slot, std::unique_ptr<IAgent> agent, uint64_t seed,
                       uint8_t delayTicks) {
  if (agent == nullptr || slotAt(slot) == nullptr) return;
  Slot& s = slots_[slot.value];
  s = Slot{};
  s.agent = std::move(agent);
  s.delay = std::min(delayTicks, kMaxAgentDelayTicks);
  s.seed = seed;
  s.agent->init(seed, slot);
  invalidateContentHash();
}

std::unique_ptr<IAgent> AgentHost::detach(PlayerId slot) {
  if (slotAt(slot) == nullptr) return nullptr;
  Slot& s = slots_[slot.value];
  std::unique_ptr<IAgent> agent = std::move(s.agent);
  s = Slot{};
  invalidateContentHash();
  return agent;
}

void AgentHost::clear() {
  for (Slot& s : slots_)
    s = Slot{};
  invalidateContentHash();
  filledAt_.reset();
  observedAt_.reset();
}

void AgentHost::reinit(PlayerId slot, uint64_t seed) {
  Slot* s = slotAt(slot);
  if (s == nullptr || s->agent == nullptr) return;
  restartLine(*s);
  s->paused = false;
  s->hadMonkey = false;
  s->seed = seed;
  s->agent->init(seed, slot);
  invalidateContentHash();
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

bool AgentHost::paused(PlayerId slot) const {
  const Slot* s = slotAt(slot);
  return s != nullptr && s->agent != nullptr && s->paused;
}

uint32_t AgentHost::wipes(PlayerId slot) const {
  const Slot* s = slotAt(slot);
  return s != nullptr ? s->wipes : 0;
}

void AgentHost::fill(sim::InputFrame& frame, const sim::WorldState& world,
                     const sim::MatchContent& content) {
  KFM_ASSERT(frame.tick == world.tick);
  // The observe check (see the header): the world moved on since the last
  // fill(), so the step that left it must have been observed.
  if (filledAt_.has_value() && world.tick > *filledAt_ && observedAt_ != world.tick) {
    ++missedObserves_;
    KFM_ASSERT_MSG(!assertOnMissedObserve_,
                   "AgentHost: the world was stepped since the last fill() without observe() "
                   "(its bots hear no calls); call observe() after every step, or use step()");
  }
  filledAt_ = world.tick;
  const bool fog = content.mode.vision.fog();
  const bool windowEnds = turnWindowEnds(world, content);
  bool sightsReady = false;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    Slot& s = slots_[i];
    const PlayerId id{i};
    if (s.agent == nullptr || !sim::maskHas(frame.present, id)) continue;

    // The lifecycle: a slot whose monkey is gone (eliminated) is paused.
    const bool monkey = world.playerBySlot(id) != nullptr;
    if (monkey) s.hadMonkey = true;
    if (!monkey && s.hadMonkey) {
      if (!s.paused) {
        s.paused = true;
        restartLine(s);
      }
      ++s.stats.paused;
      frame.inputs[i] = sim::PlayerInput{};
      continue;
    }
    if (s.paused) {
      s.paused = false;
      restartLine(s);
      if (memory_ == RespawnMemory::Wipe) {
        s.agent->init(s.seed, id);
        ++s.wipes;
      }
    }

    if (hashFor_ != &content.db) {  // only when some agent plays (see the header)
      hash_ = sim::contentHash(content.db);
      hashFor_ = &content.db;
    }
    if (fog) {
      if (!sightsReady) {
        for (uint8_t j = 0; j < sim::kMaxPlayers; ++j)
          sights_[j] = sightOf(world, content, PlayerId{j});
        sightsReady = true;
      }
      s.seen.add(visionOf(world, content, id, &sights_), world);
    }
    const ObservationContext context{fog ? &s.seen : nullptr, s.heard.data(), s.heardCount};
    buildObservation(world, content, hash_, id, s.delay, context, obs_);
    if (windowEnds) {
      s.seen.clear();
      s.heardCount = 0;
    }
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

void AgentHost::observe(const sim::EventSink& events, const sim::WorldState& after,
                        const sim::MatchContent& content) {
  observedAt_ = after.tick;
  const sim::ArenaDef& arena = content.arena;
  for (const sim::SimEvent& e : events) {
    if (e.kind != sim::EventKind::MonkeyCalled) continue;
    const sim::Player* caller = after.playerBySlot(e.player);
    if (caller == nullptr) continue;  // gone with the step that called: nobody heard where
    const Fixed range = Fixed::fromRaw(static_cast<int32_t>(e.b));
    const Vec2 from = caller->transform.pos;
    HeardCall call;
    call.slot = e.player.value;
    call.symbol = static_cast<uint8_t>(e.a);
    call.tick = e.tick.value;
    call.hasCell = sim::grid::hasCells(arena);
    if (call.hasCell) call.cell = sim::grid::cellOf(arena, from);
    for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
      Slot& s = slots_[i];
      if (s.agent == nullptr || s.paused || i == e.player.value) continue;
      const sim::Player* listener = after.playerBySlot(PlayerId{i});
      if (listener == nullptr) continue;
      const Vec2 d = listener->transform.pos - from;
      if (abs(d.x) > range || abs(d.y) > range) continue;
      if (s.heardCount < s.heard.size()) {
        s.heard[s.heardCount++] = call;
        ++s.stats.heard;
      }
    }
  }
}

void AgentHost::step(sim::WorldState& world, sim::InputFrame& frame,
                     const sim::MatchContent& content, sim::EventSink& sink) {
  fill(frame, world, content);
  sink.clear();
  sim::step(world, frame, content, sink);
  observe(sink, world, content);
}

}  // namespace kfm::ai
