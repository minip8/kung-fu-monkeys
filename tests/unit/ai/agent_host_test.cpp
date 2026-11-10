// AgentHost (architecture §8): the per-slot delay line, the repeat-last rule
// with its counter, deterministic slot order and present-mask handling.
#include "kfm/ai/agent_host.hpp"

#include "../sim/sim_test_util.hpp"
#include "kfm/ai/agent.hpp"
#include "kfm/ai/observation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using namespace kfm;
using namespace kfm::ai;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

// Answers moveDir = the observed tick's low byte; nullopt on ticks where
// tick % failEvery == failAt. Logs (slot, tick, delay seen) of every act.
struct Call {
  uint8_t slot;
  uint32_t tick;
  uint8_t delay;
  bool operator==(const Call&) const = default;
};

class EchoAgent final : public IAgent {
 public:
  explicit EchoAgent(std::vector<Call>& log, uint32_t failEvery = 0, uint32_t failAt = 0)
      : log_(&log), failEvery_(failEvery), failAt_(failAt) {}

  void init(uint64_t seed, PlayerId slot) override {
    seed_ = seed;
    slot_ = slot;
  }
  std::optional<PlayerInput> act(const Observation& obs, Tick tick) override {
    log_->push_back(Call{obs.self.slot, tick.value, obs.match.inputDelayTicks});
    if (obs.match.tick != tick.value) return std::nullopt;  // the host passes the observed tick
    if (failEvery_ != 0 && tick.value % failEvery_ == failAt_) return std::nullopt;
    PlayerInput in;
    in.moveDir = static_cast<uint8_t>(tick.value & 0xFFu);
    in.moveMag = 255;
    in.buttons = static_cast<uint8_t>(slot_.value);
    return in;
  }
  [[nodiscard]] uint64_t seed() const { return seed_; }
  [[nodiscard]] PlayerId slot() const { return slot_; }

 private:
  std::vector<Call>* log_;
  uint32_t failEvery_;
  uint32_t failAt_;
  uint64_t seed_{0};
  PlayerId slot_{PlayerId::invalid()};
};

// Fills and steps `ticks` ticks, returning the input written for `slot` on each.
std::vector<PlayerInput> run(AgentHost& host, WorldState& s, PlayerId slot, int ticks) {
  std::vector<PlayerInput> out;
  EventSink sink;
  for (int k = 0; k < ticks; ++k) {
    InputFrame f = frameFor(s);
    host.fill(f, s, defaultContent());
    out.push_back(f.inputs[slot.value]);
    sink.clear();
    step(s, f, defaultContent(), sink);
  }
  return out;
}

}  // namespace

TEST_CASE("agentSeed is the documented formula every host seeds its agents with", "[ai][host]") {
  // kfm_arena, kfm_server and the client all seed slot s of a match with
  // seed S as S ^ (0x9E3779B97F4A7C15 * (s + 1)) (architecture §8).
  STATIC_REQUIRE(agentSeed(0, PlayerId{0}) == 0x9E3779B97F4A7C15ULL);
  STATIC_REQUIRE(agentSeed(7, PlayerId{1}) == (7ULL ^ (0x9E3779B97F4A7C15ULL * 2)));
  STATIC_REQUIRE(agentSeed(0xB07, PlayerId{9}) == (0xB07ULL ^ (0x9E3779B97F4A7C15ULL * 10)));
  STATIC_REQUIRE(agentSeed(7, PlayerId{0}) != agentSeed(7, PlayerId{1}));
  STATIC_REQUIRE(agentSeed(7, PlayerId{0}) != 7);
}

TEST_CASE("attach initialises the agent with its seed and slot", "[ai][host]") {
  std::vector<Call> log;
  AgentHost host;
  auto agent = std::make_unique<EchoAgent>(log);
  const EchoAgent* raw = agent.get();
  host.attach(PlayerId{3}, std::move(agent), 0xBEEF, 4);
  CHECK(raw->seed() == 0xBEEF);
  CHECK(raw->slot() == PlayerId{3});
  CHECK(host.has(PlayerId{3}));
  CHECK_FALSE(host.has(PlayerId{2}));
  CHECK(host.slots() == (1u << 3));
  CHECK(host.delayTicks(PlayerId{3}) == 4);
  host.attach(PlayerId::invalid(), std::make_unique<EchoAgent>(log), 1);
  host.attach(PlayerId{2}, nullptr, 1);
  CHECK(host.slots() == (1u << 3));
  host.attach(PlayerId{5}, std::make_unique<EchoAgent>(log), 1, 255);
  CHECK(host.delayTicks(PlayerId{5}) == kMaxAgentDelayTicks);

  const std::unique_ptr<IAgent> back = host.detach(PlayerId{3});
  CHECK(back.get() == raw);
  CHECK_FALSE(host.has(PlayerId{3}));
  CHECK(host.detach(PlayerId{3}) == nullptr);
}

TEST_CASE("a delay shifts an agent's inputs by exactly N ticks", "[ai][host]") {
  constexpr int kTicks = 40;
  std::vector<Call> log;
  for (const uint8_t delay : {uint8_t{0}, uint8_t{1}, uint8_t{5}, uint8_t{17}}) {
    WorldState s = makePlayingWorld(5, 0b11);
    const uint32_t start = s.tick.value;
    AgentHost host;
    host.attach(PlayerId{1}, std::make_unique<EchoAgent>(log), 9, delay);
    const std::vector<PlayerInput> in = run(host, s, PlayerId{1}, kTicks);
    INFO("delay " << int{delay});
    for (int k = 0; k < kTicks; ++k) {
      const PlayerInput& got = in[static_cast<size_t>(k)];
      if (k < int{delay}) {
        CHECK(got == PlayerInput{});  // nothing due yet: idle
      } else {
        CHECK(got.moveDir ==
              static_cast<uint8_t>((start + static_cast<uint32_t>(k - delay)) & 0xFF));
        CHECK(got.moveMag == 255);
      }
    }
    CHECK(host.stats(PlayerId{1}).acts == kTicks);
    CHECK(host.stats(PlayerId{1}).substitutions == 0);
    CHECK(log.back().delay == delay);  // the observation carries the slot's delay
  }
}

TEST_CASE("no input repeats the last applied input and is counted", "[ai][host]") {
  std::vector<Call> log;
  for (const uint8_t delay : {uint8_t{0}, uint8_t{3}}) {
    WorldState s = makePlayingWorld(5, 0b11);
    const uint32_t start = s.tick.value;
    AgentHost host;
    // Fails on every tick whose number is 2 mod 4.
    host.attach(PlayerId{0}, std::make_unique<EchoAgent>(log, 4, 2), 1, delay);
    const std::vector<PlayerInput> in = run(host, s, PlayerId{0}, 24);
    INFO("delay " << int{delay});
    uint32_t failures = 0;
    for (int k = 0; k < 24; ++k) {
      const uint32_t tick = start + static_cast<uint32_t>(k);
      if (tick % 4 == 2) ++failures;
      if (k < int{delay}) continue;
      const uint32_t answered = tick - delay;  // the observation this input answers
      const uint32_t expected = answered % 4 == 2 ? answered - 1 : answered;
      CHECK(in[static_cast<size_t>(k)].moveDir == static_cast<uint8_t>(expected & 0xFF));
    }
    CHECK(host.stats(PlayerId{0}).acts == 24);
    CHECK(host.stats(PlayerId{0}).substitutions == failures);
  }
}

TEST_CASE("agents act in slot order on present slots only", "[ai][host]") {
  std::vector<Call> log;
  const WorldState s = makePlayingWorld(5, 0b1111);
  AgentHost host;
  // Attached out of order; slot 2 is not in the frame's roster.
  for (const uint8_t slot : {uint8_t{3}, uint8_t{0}, uint8_t{2}, uint8_t{1}}) {
    host.attach(PlayerId{slot}, std::make_unique<EchoAgent>(log), slot);
  }
  InputFrame f = frameFor(s);
  f.present = 0b1011;
  f.inputs[2].moveDir = 99;  // a human's input in a slot the host skips stays as it was
  host.fill(f, s, defaultContent());
  REQUIRE(log.size() == 3);
  CHECK(log[0] == Call{0, s.tick.value, 0});
  CHECK(log[1] == Call{1, s.tick.value, 0});
  CHECK(log[2] == Call{3, s.tick.value, 0});
  CHECK(f.inputs[2].moveDir == 99);
  CHECK(f.inputs[3].buttons == 3);
  CHECK(host.stats(PlayerId{2}).acts == 0);
  CHECK(host.lastObservation().self.slot == 3);
}

TEST_CASE("setDelay grows by holding and shrinks by skipping, never replaying", "[ai][host]") {
  std::vector<Call> log;
  WorldState s = makePlayingWorld(5, 0b11);
  const uint32_t start = s.tick.value;
  AgentHost host;
  host.attach(PlayerId{1}, std::make_unique<EchoAgent>(log), 9, 2);
  const auto answered = [&](const PlayerInput& in) {
    return static_cast<uint32_t>(in.moveDir);  // the low byte of the observed tick
  };
  const auto tickByte = [&](uint32_t k) { return (start + k) & 0xFFu; };

  std::vector<PlayerInput> in = run(host, s, PlayerId{1}, 10);  // k = 0..9, delay 2
  CHECK(answered(in[9]) == tickByte(7));

  host.setDelay(PlayerId{1}, 5);  // k = 10..19
  CHECK(host.delayTicks(PlayerId{1}) == 5);
  in = run(host, s, PlayerId{1}, 10);
  // k = 10..12 would be due answers 5..7, all applied already: hold answer 7.
  for (int k = 0; k < 3; ++k)
    CHECK(answered(in[static_cast<size_t>(k)]) == tickByte(7));
  for (uint32_t k = 3; k < 10; ++k)
    CHECK(answered(in[k]) == tickByte(10 + k - 5));
  CHECK(log.back().delay == 5);

  host.setDelay(PlayerId{1}, 1);  // k = 20..29: answers 15..18 are skipped
  in = run(host, s, PlayerId{1}, 10);
  for (uint32_t k = 0; k < 10; ++k)
    CHECK(answered(in[k]) == tickByte(20 + k - 1));
  CHECK(host.stats(PlayerId{1}).acts == 30);

  host.setDelay(PlayerId{1}, 200);
  CHECK(host.delayTicks(PlayerId{1}) == kMaxAgentDelayTicks);
  host.setDelay(PlayerId{4}, 3);  // no agent there: nothing happens
  CHECK(host.delayTicks(PlayerId{4}) == 0);
}

TEST_CASE("reinit re-seeds the agent and empties its delay line; clear empties the host",
          "[ai][host]") {
  std::vector<Call> log;
  WorldState s = makePlayingWorld(5, 0b11);
  AgentHost host;
  auto agent = std::make_unique<EchoAgent>(log);
  const EchoAgent* raw = agent.get();
  host.attach(PlayerId{0}, std::move(agent), 1, 3);
  std::vector<PlayerInput> in = run(host, s, PlayerId{0}, 8);
  CHECK(in.back().moveMag == 255);

  host.reinit(PlayerId{0}, 0xC0FFEE);
  CHECK(raw->seed() == 0xC0FFEE);
  CHECK(raw->slot() == PlayerId{0});
  CHECK(host.delayTicks(PlayerId{0}) == 3);
  const uint32_t start = s.tick.value;
  in = run(host, s, PlayerId{0}, 6);
  for (uint32_t k = 0; k < 3; ++k)
    CHECK(in[k] == PlayerInput{});  // idle until the first answer of the new match is due
  for (uint32_t k = 3; k < 6; ++k)
    CHECK(in[k].moveDir == static_cast<uint8_t>((start + k - 3) & 0xFFu));
  CHECK(host.stats(PlayerId{0}).acts == 14);  // counters are per attach

  host.reinit(PlayerId{5}, 7);  // no agent: no-op
  host.attach(PlayerId{4}, std::make_unique<EchoAgent>(log), 2);
  CHECK(host.slots() == 0b10001);
  host.clear();
  CHECK(host.slots() == 0);
  CHECK(host.stats(PlayerId{0}).acts == 0);
}

TEST_CASE("the content hash is cached and recomputed after an invalidation", "[ai][host]") {
  // fill() hashes the ContentDB once, not every tick; an in-place edit needs
  // invalidateContentHash() (the client's live tuning), while a different
  // ContentDB object, attach and reinit each recompute on their own.
  std::vector<Call> log;
  WorldState s = makePlayingWorld(5, 0b11);
  ContentDB c = defaultContent();
  const uint32_t original = contentHash(c);
  AgentHost host;
  host.attach(PlayerId{0}, std::make_unique<EchoAgent>(log), 1);
  const auto fillOnce = [&](const ContentDB& db) {
    InputFrame f = frameFor(s);
    host.fill(f, s, db);
    return host.lastObservation().match.contentHash;
  };
  CHECK(fillOnce(c) == original);

  const uint16_t stun = c.player.hitStunTicks;
  c.player.hitStunTicks = static_cast<uint16_t>(stun + 1);
  const uint32_t edited = contentHash(c);
  REQUIRE(edited != original);
  CHECK(fillOnce(c) == original);  // an in-place edit is not seen by itself
  host.invalidateContentHash();
  CHECK(fillOnce(c) == edited);

  const ContentDB other = defaultContent();
  CHECK(fillOnce(other) == original);  // another object: recomputed
  CHECK(fillOnce(c) == edited);

  c.player.hitStunTicks = stun;
  host.reinit(PlayerId{0}, 2);
  CHECK(fillOnce(c) == original);
  c.player.hitStunTicks = static_cast<uint16_t>(stun + 1);
  host.attach(PlayerId{1}, std::make_unique<EchoAgent>(log), 3);
  CHECK(fillOnce(c) == edited);
}
