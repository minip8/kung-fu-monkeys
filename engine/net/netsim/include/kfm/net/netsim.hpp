// In-process netsim (architecture §6.4, §9, §11): one NetServer and N
// NetClients over a LoopbackNetwork, stepped by a deterministic scheduler.
// Shared by the end-to-end netcode tests (tests/netcode) and the kfm_netsim
// app (apps/netsim); target kfm::netsim.
//
// Time advances in 1 ms steps; the server ticks exactly every 1/60 s (tick k
// at k·1e9/60 ns) and each client runs its own rate-controlled TickClock
// through ticksDue(). Fake humans are sim::scriptedInput on the client's own
// predicted world with a per-client Rng, so a run is a pure function of its
// seeds.
#pragma once

#include "kfm/core/rng.hpp"
#include "kfm/net/loopback.hpp"
#include "kfm/net/net_client.hpp"
#include "kfm/net/net_server.hpp"
#include "kfm/sim/content.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace kfm::net::netsim {

inline constexpr uint64_t kMs = 1'000'000;
inline constexpr uint64_t kSec = 1'000 * kMs;

// Default content with a match that never ends, so a long soak keeps
// playing rounds (both sides use it, so the content hashes agree).
[[nodiscard]] const sim::ContentDB& endlessContent();
// The same change to any content: every mode's roundsToWin = 255.
[[nodiscard]] sim::ContentDB endless(const sim::ContentDB& content);

// One direction of a link: latency ± jitter (so latency − jitter .. latency +
// jitter) and loss on both lanes (reliable loss costs a retransmission of
// max(2 × latency, 20 ms)).
[[nodiscard]] LinkConditions link(uint64_t latencyNs, uint64_t jitterNs = 0, uint32_t lossPpm = 0);

// A ClientConfig with a token (so a dropped link can reclaim its slots).
[[nodiscard]] ClientConfig tokenConfig(const std::string& token, uint8_t slots = 1);

enum class Human : uint8_t { Scripted = 0, Idle };

struct ClientRig {
  LoopbackTransport* transport{nullptr};
  std::unique_ptr<NetClient> client;
  Rng rng{};
  Human human{Human::Scripted};
  LinkConditions up{};
  LinkConditions down{};
  bool active{true};  // stepped by the scheduler
};

class NetSim {
 public:
  explicit NetSim(ServerConfig config = {}, uint64_t seed = 1,
                  const sim::ContentDB& content = endlessContent());

  // Opens a link and starts a client on it.
  ClientRig& addClient(const LinkConditions& up, const LinkConditions& down,
                       const ClientConfig& config = {}, Human human = Human::Scripted,
                       uint64_t humanSeed = 0);

  // Opens a fresh link for an existing client (reconnect).
  void reconnect(ClientRig& r);

  // One 1 ms step: server pump, due server ticks, every active client frame.
  void step();
  void runFor(uint64_t ns);

  template <class Pred>
  bool runUntil(Pred pred, uint64_t maxNs) {
    const uint64_t end = now_ + maxNs;
    while (now_ < end) {
      if (pred()) return true;
      step();
    }
    return pred();
  }

  [[nodiscard]] NetServer& server() { return *server_; }
  [[nodiscard]] const NetServer& server() const { return *server_; }
  [[nodiscard]] LoopbackNetwork& network() { return net_; }
  [[nodiscard]] std::deque<ClientRig>& rigs() { return rigs_; }
  [[nodiscard]] const std::deque<ClientRig>& rigs() const { return rigs_; }
  [[nodiscard]] uint64_t now() const { return now_; }
  // Server checksum of the state whose world.tick == t.
  [[nodiscard]] uint32_t serverSum(Tick t) const { return sums_.at(t.value); }
  [[nodiscard]] const sim::ContentDB& content() const { return *content_; }
  [[nodiscard]] bool allRunning() const;

  // Called after every server tick, before the server's events are cleared.
  std::function<void()> afterServerTick;
  // Called for every client frame after its ticks (sampling hooks).
  std::function<void(ClientRig&)> afterClientFrame;

 private:
  [[nodiscard]] static uint64_t serverTickAt(uint64_t k) { return k * kSec / sim::kTickHz; }
  void stepClient(ClientRig& r) const;

  const sim::ContentDB* content_;
  LoopbackNetwork net_;
  std::unique_ptr<NetServer> server_;
  std::deque<ClientRig> rigs_;
  std::vector<uint32_t> sums_;  // sums_[t] = checksum of the server state at tick t
  uint64_t now_{0};
  uint64_t serverTicks_{0};
};

// p-th percentile (0..100) of a rollback depth histogram.
[[nodiscard]] uint32_t percentile(std::span<const uint64_t> hist, uint32_t p);
// The deepest bucket with a non-zero count (0 when empty).
[[nodiscard]] uint32_t maxBucket(std::span<const uint64_t> hist);

// Lead against the §6.2 target, sampled every client frame after a warm-up.
struct LeadSampler {
  double leadSum{0};
  double targetSum{0};
  uint64_t samples{0};

  void sample(const NetSim& s, const ClientRig& r, Tick serverNextTick);
};

// Aggregated numbers of a run (the soak numbers of architecture §11).
struct Report {
  uint32_t clients{0};  // clients with slots (spectators excluded)
  uint32_t maxDepth{0};
  uint32_t p99Depth{0};
  uint32_t p50Depth{0};
  double mispredictionsPerSec{0};  // per client
  double rollbacksPerSec{0};       // per client
  double upBytesPerSec{0};         // per client
  double downBytesPerSec{0};       // per client
  uint64_t mismatches{0};
  uint64_t resyncs{0};
  uint64_t compared{0};
  uint64_t overflows{0};
  uint64_t hardJumps{0};
  uint64_t anchorRollbacks{0};
  uint64_t snapshotsLoaded{0};
  double meanLead{0};
  double meanTarget{0};
  double serverSubstPerSec{0};  // per client, after the warm-up
};

// Summarises every client of `s` (players and spectators alike for the
// consistency counters; rates are per playing client) over `seconds`.
// `substAfter` is the server's substitutedInputs at the end of the warm-up and
// `substSeconds` the time since.
[[nodiscard]] Report summarise(const NetSim& s, double seconds, const LeadSampler& lead,
                               uint64_t substAfter, double substSeconds);

// One line, the format the soak tests print.
[[nodiscard]] std::string format(const char* name, const Report& r, double wallSeconds);

}  // namespace kfm::net::netsim
