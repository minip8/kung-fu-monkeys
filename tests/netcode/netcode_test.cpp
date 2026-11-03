// End-to-end netcode over the loopback emulator (architecture §6, §9, §11 M3
// exit criterion): a NetServer and NetClients in one deterministic loop.
#include "kfm/net/netsim.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

using namespace kfm;
using namespace kfm::net;
using namespace kfm::net::netsim;

namespace {

[[nodiscard]] bool allRunning(const NetSim& s) {
  return s.allRunning();
}

using SoakReport = Report;

// What a soak plays: the content and the mode index (default: the endless
// default content, mode 0).
struct SoakMatch {
  const sim::ContentDB* content{nullptr};
  uint8_t mode{0};
  // Optional per-server-tick hook (event counting).
  std::function<void(const NetServer&)> onServerTick;
};

// Runs a soak of `clients` clients over symmetric links for `seconds`, then
// checks the §11 M3 exit criterion.
SoakReport soak(const char* name, uint32_t clients, uint64_t oneWayNs, uint64_t jitterNs,
                uint32_t lossPpm, uint32_t seconds, const SoakMatch& match = {}) {
  const auto wallStart = std::chrono::steady_clock::now();
  ServerConfig sc;
  sc.match.seed = 0x50A4;
  sc.match.mode = match.mode;
  NetSim s(sc, 0xBEEF + clients, match.content != nullptr ? *match.content : endlessContent());
  if (match.onServerTick) s.afterServerTick = [&] { match.onServerTick(s.server()); };
  const LinkConditions l = link(oneWayNs, jitterNs, lossPpm);
  for (uint32_t i = 0; i < clients; ++i)
    s.addClient(l, l, tokenConfig("player-" + std::to_string(i)));
  REQUIRE(s.runUntil([&] { return allRunning(s); }, 5 * kSec));

  // Let the lead controller converge, then measure.
  constexpr uint64_t kWarmup = 10 * kSec;
  LeadSampler lead;
  s.afterClientFrame = [&](ClientRig& r) {
    if (s.now() >= kWarmup) lead.sample(s, r, s.server().nextTick());
  };
  s.runFor(kWarmup - s.now());
  const uint64_t substAfter = s.server().stats().substitutedInputs;
  const uint64_t measured = static_cast<uint64_t>(seconds) * kSec - kWarmup;
  s.runFor(measured);
  s.afterClientFrame = nullptr;

  const double wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
  const SoakReport r = summarise(s, static_cast<double>(s.now()) / 1e9, lead, substAfter,
                                 static_cast<double>(measured) / 1e9);
  std::printf("%s\n", format(name, r, wall).c_str());
  CHECK(allRunning(s));
  CHECK(r.mismatches == 0);
  CHECK(r.resyncs == 0);
  CHECK(r.overflows == 0);
  CHECK(r.compared > 0);
  CHECK(r.maxDepth < kRollbackWindow);
  return r;
}

}  // namespace

TEST_CASE("zero latency: confirmed states match the server tick for tick", "[netcode]") {
  ServerConfig sc;
  sc.match.seed = 0xA;
  sc.checksumEveryTicks = 1;
  NetSim s(sc, 7);
  const LinkConditions l = link(0);
  ClientRig& a = s.addClient(l, l, tokenConfig("a"), Human::Scripted);
  ClientRig& b = s.addClient(l, l, tokenConfig("b"), Human::Idle);
  REQUIRE(s.runUntil([&] { return allRunning(s); }, 2 * kSec));
  s.runFor(2 * kSec);  // both joined; the present-mask changes are behind us
  const ClientNetStats a0 = a.client->netStats();
  const ClientNetStats b0 = b.client->netStats();
  const uint64_t subst0 = s.server().stats().substitutedInputs;
  s.runFor(10 * kSec);

  const ClientNetStats& sa = a.client->netStats();
  const ClientNetStats& sb = b.client->netStats();
  INFO("a: rollbacks " << sa.rollbacks - a0.rollbacks << ", b: rollbacks "
                       << sb.rollbacks - b0.rollbacks << ", b mispredicted remote "
                       << sb.mispredictedRemote - b0.mispredictedRemote);
  // Every tick's checksum was compared and matched.
  CHECK(sa.checksumsCompared - a0.checksumsCompared >= 590);
  CHECK(sb.checksumsCompared - b0.checksumsCompared >= 590);
  CHECK(sa.checksumMismatches == 0);
  CHECK(sb.checksumMismatches == 0);
  CHECK(s.server().stats().substitutedInputs == subst0);
  // A's only remote player is idle: nothing to mispredict, no rollback.
  CHECK(sa.rollbacks == a0.rollbacks);
  CHECK(sa.mispredictedLocal == 0);
  // B rolls back only because A's input changed.
  CHECK(sb.rollbacks > b0.rollbacks);
  CHECK(sb.mispredictedLocal == 0);
  CHECK(sb.mispredictedPresent == b0.mispredictedPresent);
  CHECK(sb.mispredictedRemote - b0.mispredictedRemote ==
        sb.mispredictedTicks - b0.mispredictedTicks);
  CHECK(sb.rollbacks - b0.rollbacks <= sb.mispredictedTicks - b0.mispredictedTicks);
}

TEST_CASE("soak: 10 clients at 100 ms RTT with ±10 ms jitter and 2% loss for 60 s",
          "[netcode][soak]") {
  const SoakReport r = soak("100ms-2%", 10, 50 * kMs, 10 * kMs, 20'000, 60);
  // The lead settles at the §6.2 target.
  CHECK(std::abs(r.meanLead - r.meanTarget) <= 2.0);
  CHECK(r.p99Depth <= kRollbackWindow / 2);
  // Substitutions stay rare after convergence: under 1% of a client's ticks.
  CHECK(r.serverSubstPerSec < 0.6);
}

TEST_CASE("deathmatch: 4 clients at 100 ms RTT and 2% loss stay in sync through respawns",
          "[netcode][modes]") {
  // An endless deathmatch (no score limit, no clock) so it keeps respawning.
  static const sim::ContentDB content = [] {
    sim::ContentDB c = sim::defaultContent();
    sim::ModeDef& dm = c.modes[1];
    dm.name = sim::ContentName::of("deathmatch");
    dm.kind = sim::ModeKind::Deathmatch;
    dm.respawnTicks = 60;
    dm.respawnInvulnTicks = 30;
    c.modeCount = 2;
    return endless(c);
  }();
  int respawns = 0;
  int scores = 0;
  SoakMatch match{&content, 1, [&](const NetServer& server) {
                    for (const sim::SimEvent& e : server.tickEvents()) {
                      if (e.kind == sim::EventKind::PlayerRespawned) ++respawns;
                      if (e.kind == sim::EventKind::ScoreChanged) ++scores;
                    }
                  }};
  const SoakReport r = soak("deathmatch-100ms-2%", 4, 50 * kMs, 10 * kMs, 20'000, 30, match);
  CHECK(r.mismatches == 0);
  CHECK(respawns > 5);
  CHECK(scores > 5);
}

TEST_CASE("soak: 10 clients at 300 ms RTT with ±10 ms jitter and 10% loss for 60 s",
          "[netcode][soak]") {
  const SoakReport r = soak("300ms-10%", 10, 150 * kMs, 10 * kMs, 100'000, 60);
  CHECK(std::abs(r.meanLead - r.meanTarget) <= 3.0);
}
