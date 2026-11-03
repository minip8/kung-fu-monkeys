// In-process netsim for the end-to-end netcode tests (architecture §6.4,
// §9): one NetServer and N NetClients over a LoopbackNetwork, stepped by a
// deterministic scheduler. Time advances in 1 ms steps; the server ticks
// exactly every 1/60 s (tick k at k·1e9/60 ns) and each client runs its own
// rate-controlled TickClock through ticksDue(). Fake humans are
// sim::scriptedInput on the client's own predicted world with a per-client
// Rng, so a run is a pure function of its seeds.
#pragma once

#include "kfm/core/rng.hpp"
#include "kfm/net/loopback.hpp"
#include "kfm/net/net_client.hpp"
#include "kfm/net/net_server.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/scripted_inputs.hpp"
#include "kfm/sim/step.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace kfm::net::netsim {

inline constexpr uint64_t kMs = 1'000'000;
inline constexpr uint64_t kSec = 1'000 * kMs;

// Default content with a match that never ends, so a long soak keeps
// playing rounds (both sides use it, so the content hashes agree).
[[nodiscard]] inline const sim::ContentDB& endlessContent() {
  static const sim::ContentDB c = [] {
    sim::ContentDB x = sim::defaultContent();
    x.mode.roundsToWin = 255;
    return x;
  }();
  return c;
}

// One direction of a link: latency ± jitter (so latency − jitter .. latency +
// jitter) and loss on both lanes (reliable loss costs a retransmission).
[[nodiscard]] inline LinkConditions link(uint64_t latencyNs, uint64_t jitterNs = 0,
                                         uint32_t lossPpm = 0) {
  LinkConditions c = LinkConditions::uniform(latencyNs - jitterNs, 2 * jitterNs, lossPpm);
  c.reliable.retransmitNs = std::max<uint64_t>(2 * latencyNs, 20 * kMs);
  return c;
}

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
                  const sim::ContentDB& content = endlessContent())
      : content_(&content), net_(seed) {
    server_ = std::make_unique<NetServer>(net_.server(), content, std::move(config));
    sums_.push_back(sim::checksum(server_->world()));
  }

  // Opens a link and starts a client on it.
  ClientRig& addClient(const LinkConditions& up, const LinkConditions& down,
                       ClientConfig config = {}, Human human = Human::Scripted,
                       uint64_t humanSeed = 0) {
    ClientRig& r = rigs_.emplace_back();
    r.transport = &net_.connectClient(now_, up, down);
    r.client = std::make_unique<NetClient>(*content_, config);
    r.rng = Rng::seeded(humanSeed != 0 ? humanSeed : 0xC11E47 + rigs_.size());
    r.human = human;
    r.up = up;
    r.down = down;
    r.client->connect(*r.transport, now_);
    return r;
  }

  // Opens a fresh link for an existing client (reconnect).
  void reconnect(ClientRig& r) {
    r.transport = &net_.connectClient(now_, r.up, r.down);
    r.client->connect(*r.transport, now_);
  }

  void step() {
    now_ += kMs;
    server_->pump(now_);
    while (now_ >= serverTickAt(serverTicks_)) {
      server_->tick(now_);
      ++serverTicks_;
      sums_.push_back(sim::checksum(server_->world()));
    }
    for (ClientRig& r : rigs_) {
      if (r.active) stepClient(r);
    }
    server_->clearEvents();
  }

  void runFor(uint64_t ns) {
    const uint64_t end = now_ + ns;
    while (now_ < end)
      step();
  }

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
  [[nodiscard]] LoopbackNetwork& network() { return net_; }
  [[nodiscard]] std::deque<ClientRig>& rigs() { return rigs_; }
  [[nodiscard]] uint64_t now() const { return now_; }
  // Server checksum of the state whose world.tick == t.
  [[nodiscard]] uint32_t serverSum(Tick t) const { return sums_.at(t.value); }
  [[nodiscard]] const sim::ContentDB& content() const { return *content_; }

  // Called for every client frame after its ticks (sampling hooks).
  std::function<void(ClientRig&)> afterClientFrame;

 private:
  [[nodiscard]] static uint64_t serverTickAt(uint64_t k) { return k * kSec / sim::kTickHz; }

  void stepClient(ClientRig& r) {
    NetClient& c = *r.client;
    c.beginFrame();
    c.pump(now_);
    c.reconcile();
    const uint32_t n = c.ticksDue(now_);
    for (uint32_t i = 0; i < n; ++i) {
      std::array<sim::PlayerInput, sim::kMaxPlayers> inputs{};
      size_t k = 0;
      for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
        const PlayerId id{static_cast<PlayerId::rep>(s)};
        if (!sim::maskHas(c.localSlots(), id)) continue;
        inputs[k++] = r.human == Human::Scripted
                          ? sim::scriptedInput(r.rng, c.world(), *content_, id)
                          : sim::PlayerInput{};
      }
      c.tick(std::span<const sim::PlayerInput>(inputs.data(), k));
    }
    static_cast<void>(c.drainEvents());
    if (afterClientFrame) afterClientFrame(r);
  }

  const sim::ContentDB* content_;
  LoopbackNetwork net_;
  std::unique_ptr<NetServer> server_;
  std::deque<ClientRig> rigs_;
  std::vector<uint32_t> sums_;  // sums_[t] = checksum of the server state at tick t
  uint64_t now_{0};
  uint64_t serverTicks_{0};
};

// p-th percentile (0..100) of a rollback depth histogram.
[[nodiscard]] inline uint32_t percentile(std::span<const uint64_t> hist, uint32_t p) {
  uint64_t total = 0;
  for (const uint64_t n : hist)
    total += n;
  if (total == 0) return 0;
  const uint64_t want = (total * p + 99) / 100;
  uint64_t seen = 0;
  for (size_t d = 0; d < hist.size(); ++d) {
    seen += hist[d];
    if (seen >= want) return static_cast<uint32_t>(d);
  }
  return static_cast<uint32_t>(hist.size() - 1);
}

}  // namespace kfm::net::netsim
