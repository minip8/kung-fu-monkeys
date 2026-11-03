// A scripted 10-player match through step() (architecture §5.1, §9): the
// same scripted inputs give identical per-tick checksums, the match produces
// every kind of gameplay event, and a memcpy rollback re-simulates to the
// same checksums. Inputs come from the shared generator in
// kfm/sim/scripted_inputs.hpp, the same one `kfm_replay record-scripted` uses.
#include "scenario_util.hpp"

#include "kfm/core/rng.hpp"
#include "kfm/sim/scripted_inputs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint64_t kWorldSeed = 0x4B464D10;
constexpr uint64_t kInputSeed = 0x5C121D;
constexpr PlayerMask kTenPlayers = (1u << kMaxPlayers) - 1u;
constexpr int kTicks = 1200;

struct Run {
  std::vector<uint32_t> checksums;  // after each step
  std::vector<InputFrame> frames;
  std::map<EventKind, int> events;
};

Run runScripted(uint64_t worldSeed, uint64_t inputSeed) {
  const ContentDB& content = defaultContent();
  WorldState s = initialWorld(MatchConfig{worldSeed, kTenPlayers});
  Rng inputs = Rng::seeded(inputSeed);
  Run run;
  EventSink sink;
  for (int k = 0; k < kTicks; ++k) {
    const InputFrame f = scriptedFrame(inputs, s, content);
    run.frames.push_back(f);
    sink.clear();
    step(s, f, content, sink);
    REQUIRE(sink.dropped == 0);
    for (const SimEvent& e : sink)
      ++run.events[e.kind];
    run.checksums.push_back(checksum(s));
  }
  return run;
}

}  // namespace

TEST_CASE("a scripted 10-player match is deterministic and eventful", "[scenario][match]") {
  const Run a = runScripted(kWorldSeed, kInputSeed);
  const Run b = runScripted(kWorldSeed, kInputSeed);
  REQUIRE(a.checksums.size() == static_cast<size_t>(kTicks));
  CHECK(a.checksums == b.checksums);
  CHECK(a.frames == b.frames);

  for (const auto& [kind, n] : a.events)
    UNSCOPED_INFO("event " << static_cast<int>(kind) << " x" << n);
  for (const EventKind kind :
       {EventKind::BoomerangThrown, EventKind::PlayerHit, EventKind::PlayerEliminated,
        EventKind::BoomerangCaught, EventKind::RoundEnded, EventKind::PickupTaken,
        EventKind::BoomerangBounced, EventKind::PlayerDashed, EventKind::PickupSpawned}) {
    INFO("event kind " << static_cast<int>(kind));
    CHECK(a.events.contains(kind));
  }

  // A different input seed plays a different match.
  const Run c = runScripted(kWorldSeed, kInputSeed + 1);
  CHECK(c.checksums != a.checksums);
}

TEST_CASE("memcpy rollback mid-match re-simulates to identical checksums", "[scenario][match]") {
  const ContentDB& content = defaultContent();
  const Run reference = runScripted(kWorldSeed, kInputSeed);

  // Replay the recorded frames, snapshotting by memcpy at several points, and
  // re-simulate from each snapshot (rollback, §5.2).
  for (const int snapAt : {1, 61, 400, 777}) {
    WorldState s = initialWorld(MatchConfig{kWorldSeed, kTenPlayers});
    EventSink sink;
    WorldState saved{};
    for (int k = 0; k < kTicks; ++k) {
      if (k == snapAt) std::memcpy(&saved, &s, sizeof(WorldState));
      sink.clear();
      step(s, reference.frames[static_cast<size_t>(k)], content, sink);
      REQUIRE(checksum(s) == reference.checksums[static_cast<size_t>(k)]);
    }
    // Roll back and re-simulate the rest of the match.
    std::memcpy(&s, &saved, sizeof(WorldState));
    REQUIRE(s.tick == Tick{static_cast<uint32_t>(snapAt)});
    for (int k = snapAt; k < kTicks; ++k) {
      sink.clear();
      step(s, reference.frames[static_cast<size_t>(k)], content, sink);
      INFO("snapshot at " << snapAt << ", tick " << k);
      REQUIRE(checksum(s) == reference.checksums[static_cast<size_t>(k)]);
    }
  }
}
