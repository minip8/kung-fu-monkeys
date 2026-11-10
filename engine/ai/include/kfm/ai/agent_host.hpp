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
// Per-instance lifecycle (v0.35, §8): a present slot whose monkey was in the
// world since attach() or reinit() and is gone (eliminated, waiting for a
// respawn or for the next round) is paused: act() is not called, the slot's
// input is idle and only its `paused` counter moves. A slot that never had a
// monkey yet (the lobby tick, a joiner waiting for the next round) acts as
// before, so a host's first frames do not change. When its
// monkey is back (a respawn, a new round) the slot resumes with an empty
// delay line, as after reinit(), and with RespawnMemory::Wipe its agent is
// init()ed again on the slot's seed first (a module gets a fresh instance:
// memory_on_respawn = wipe); Keep (the default) resumes the same memory.
//
// Fog and calls (v0.35, §8, docs/bot-abi.md §3): under a fog vision the host
// computes every monkey's sight once per tick (sightOf) and ORs each slot's
// (visionOf: its own, or its team's) into the slot's turn window, which the
// observation carries; the window closes after the observation of a tick
// that ends a turn (turnWindowEnds: a latch, or any tick outside a grid
// turn), so a latch observation shows everything seen since the previous
// one. observe() takes each step's events: a MonkeyCalled is heard by every
// other slot whose monkey stands within the call's range (Chebyshev between
// the centres, in units, after the step) and enters that slot's window, so
// the observation of the next tick and of every tick up to the next latch
// lists it.
//
// The observe check: a host that steps the world without passing the step
// to observe() delivers no calls, silently. step() does all three (fill,
// sim::step, observe) for a host that steps the world itself; a host whose
// world is stepped elsewhere (kfm_server: NetServer steps, its botInputs
// callback is the fill) calls fill() and observe() itself, and fill()
// checks it did: when the world has moved past the previous fill()'s tick,
// the last observe() must have been handed the world at this tick (the one
// the step left). A miss is counted (missedObserves()) and, in a build with
// asserts, aborts. Filling the same tick again, a world that went back (a
// new match, a restart, a rollback) and ticks stepped and observed without
// a fill (no agent present) are all fine, as are paused slots and ticks the
// host does not run at all (the room's Waiting and Over); clear() forgets
// both ticks. Each slot's `heard` counter counts the calls that entered its
// window, so a host test can see its bots hear.
//
// The content hash in the observation (Observation::match.contentHash) is
// sim::contentHash(content.db), which serialises the whole ContentDB: far too
// slow for every tick. The host caches it: computed at the first fill() after
// attach/detach/reinit/clear or invalidateContentHash(), and again whenever
// fill() is handed a different ContentDB object (keyed on its address). A
// host whose ContentDB is edited in place between ticks (the client's live
// feel tuning) must call invalidateContentHash() after the edit.
//
// Ownership: the host owns its agents (std::unique_ptr); attach() hands one
// over and detach() hands it back. Allocation happens only in attach/detach;
// fill() allocates nothing, never writes the world and costs O(agents) apart
// from the observations themselves (plus one contentHash() after a cache
// invalidation).
#pragma once

#include "kfm/ai/agent.hpp"
#include "kfm/ai/observation.hpp"
#include "kfm/ai/observation_builder.hpp"
#include "kfm/ai/vision.hpp"
#include "kfm/core/types.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
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

// The seed every host init()s the agent in `slot` with, from the match's bot
// seed (kfm_arena: the world seed; kfm_server: its per-match bot seed; the
// client: --players-seed): the seed mixed with the golden-ratio constant
// times (slot + 1), so slots differ and slot 0 does not get the seed itself.
// One formula for every host, so one seed means the same bots everywhere
// and a module's seed is reproducible from an arena result line (§8).
[[nodiscard]] constexpr uint64_t agentSeed(uint64_t seed, PlayerId slot) {
  return seed ^ (0x9E3779B97F4A7C15ULL * (uint64_t{slot.value} + 1));
}

// What an agent remembers across its monkey's elimination (memory_on_respawn).
enum class RespawnMemory : uint8_t { Keep = 0, Wipe = 1 };

struct AgentSlotStats {
  uint32_t acts{0};           // act() calls (one per present tick with a monkey, v0.35)
  uint32_t substitutions{0};  // of them, the ones that returned nullopt (repeat last input)
  uint32_t paused{0};         // v0.35: present ticks sat out without a monkey (the lifecycle)
  uint32_t heard{0};          // calls (MonkeyCalled) that entered the slot's turn window
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
  // Destroys every agent (and drops the cached content hash).
  void clear();
  // Calls init(seed, slot) on the agent in `slot` again and empties its delay
  // line and last input; the delay and the counters stay. No-op without an
  // agent.
  void reinit(PlayerId slot, uint64_t seed);
  // Changes the slot's input delay (clamped to kMaxAgentDelayTicks) from the
  // next fill() on; see the header comment. No-op without an agent.
  void setDelay(PlayerId slot, uint8_t delayTicks);
  // memory_on_respawn for every slot (see the header comment).
  void setRespawnMemory(RespawnMemory memory) { memory_ = memory; }
  [[nodiscard]] RespawnMemory respawnMemory() const { return memory_; }

  // The content hash cache (see the header comment): the next fill()
  // recomputes sim::contentHash(content.db). Call it after editing, in place,
  // the ContentDB a host passes to fill().
  void invalidateContentHash() { hashFor_ = nullptr; }

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
  // The events of the step that just ran and the world it left (v0.35): the
  // calls each slot hears (see the header comment). Hosts call it after every
  // step(); one that does not simply delivers no calls.
  void observe(const sim::EventSink& events, const sim::WorldState& after,
               const sim::MatchContent& content);

  // One tick of a host that steps the world itself (kfm_arena, the client's
  // MatchSession): fill(frame, world, content), then sink.clear() and
  // sim::step(world, frame, content, sink), then observe(sink, world,
  // content), so a host cannot step without its bots hearing the calls.
  // `frame` (tick, present and the non-agent slots set by the caller) is
  // left holding the inputs the step ran with, for the host to record.
  void step(sim::WorldState& world, sim::InputFrame& frame, const sim::MatchContent& content,
            sim::EventSink& sink);

  // The observe check (see the header comment): fills that found the step
  // before them not observed. With setAssertOnMissedObserve(false) a miss is
  // only counted, never an abort (tests of the check itself).
  [[nodiscard]] uint32_t missedObserves() const { return missedObserves_; }
  void setAssertOnMissedObserve(bool on) { assertOnMissedObserve_ = on; }

  // Whether the slot is paused (its monkey eliminated, see the header).
  [[nodiscard]] bool paused(PlayerId slot) const;
  // Re-inits on a resume under RespawnMemory::Wipe, since attach.
  [[nodiscard]] uint32_t wipes(PlayerId slot) const;

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
    uint64_t seed{0};  // the last init() seed: a wipe re-inits with it
    bool paused{false};
    bool hadMonkey{false};  // its monkey was in the world since attach/reinit
    uint32_t wipes{0};
    // v0.35: the turn window (see the header comment).
    ObsVisibility seen{};
    std::array<HeardCall, kObsMaxHeardCalls> heard{};
    uint8_t heardCount{0};
    // ring[n % ring.size()] holds the answer of the n-th act(): the last
    // kMaxAgentDelayTicks + 1 answers, whatever the delay, so a delay change
    // never loses one that is still due.
    std::array<std::optional<sim::PlayerInput>, kMaxAgentDelayTicks + 1> ring{};
  };
  [[nodiscard]] const Slot* slotAt(PlayerId slot) const;
  [[nodiscard]] Slot* slotAt(PlayerId slot);
  // Empties the delay line, the last input and the turn window.
  static void restartLine(Slot& s);

  std::array<Slot, sim::kMaxPlayers> slots_{};
  Observation obs_{};
  RespawnMemory memory_{RespawnMemory::Keep};
  std::array<Sight, sim::kMaxPlayers> sights_{};  // fill()'s per-tick scratch
  // The content hash cache: hash_ is contentHash(*hashFor_) while hashFor_ is set.
  const sim::ContentDB* hashFor_{nullptr};
  uint32_t hash_{0};
  // The observe check: the world tick of the last fill() and the tick of the
  // world the last observe() was handed.
  std::optional<Tick> filledAt_;
  std::optional<Tick> observedAt_;
  uint32_t missedObserves_{0};
  bool assertOnMissedObserve_{true};
};

}  // namespace kfm::ai
