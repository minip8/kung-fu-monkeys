// End-to-end session flows over the loopback emulator (architecture §6.6):
// mid-match join, spectating, reconnect within the grace window, checksum
// resync, input delay.
#include "kfm/net/netsim.hpp"
#include "kfm/sim/step.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>

using namespace kfm;
using namespace kfm::net;
using namespace kfm::net::netsim;

namespace {

[[nodiscard]] bool allRunning(const NetSim& s) {
  return s.allRunning();
}

}  // namespace

TEST_CASE("a client joining mid-match gets a snapshot and converges", "[netcode][join]") {
  ServerConfig sc;
  sc.match.seed = 0xD;
  NetSim s(sc, 11);
  const LinkConditions l = link(30 * kMs, 5 * kMs, 10'000);
  s.addClient(l, l, tokenConfig("first"));
  s.addClient(l, l, tokenConfig("second"));
  REQUIRE(s.server().setBot(PlayerId{9}, true));
  s.runFor(8 * kSec);
  REQUIRE(allRunning(s));
  const Tick before = s.server().nextTick();

  ClientRig& late = s.addClient(l, l, tokenConfig("late"));
  ClientRig& spec = s.addClient(l, l, tokenConfig("watcher", 0));
  REQUIRE(s.runUntil([&] { return late.client->running() && spec.client->running(); }, 3 * kSec));
  CHECK(late.client->localSlots() == 0b100);  // the lowest free slot
  CHECK(spec.client->spectator());
  CHECK(late.client->netStats().snapshotsLoaded == 1);
  CHECK(late.client->world().tick > before);
  CHECK(sim::maskHas(s.server().present(), PlayerId{2}));

  s.runFor(10 * kSec);
  const ClientNetStats& st = late.client->netStats();
  CHECK(st.checksumsCompared > 50);
  CHECK(st.checksumMismatches == 0);
  CHECK(st.resyncRequests == 0);
  CHECK(sim::maskHas(late.client->world().rules.present, PlayerId{2}));
  // The joiner's lead converged like everyone else's.
  CHECK(std::abs(late.client->localTick().minus(s.server().nextTick()) - st.targetLeadTicks) <= 2);

  // The spectator follows the authoritative stream a few ticks behind and
  // never predicts: its world is the server's at the same tick.
  const NetClient& w = *spec.client;
  const int32_t behind = s.server().nextTick().minus(w.localTick());
  INFO("spectator behind by " << behind << " ticks");
  CHECK(behind > 0);
  CHECK(behind < 30);
  CHECK(w.netStats().rollbacks == 0);
  CHECK(w.netStats().checksumMismatches == 0);
  CHECK(w.netStats().checksumsCompared > 50);
  CHECK(sim::checksum(w.world()) == s.serverSum(w.localTick()));
}

TEST_CASE("a cut link reconnects within the grace window and reclaims its slots",
          "[netcode][reconnect]") {
  ServerConfig sc;
  sc.match.seed = 0xE;
  sc.reconnectGraceTicks = 5 * sim::kTickHz;
  NetSim s(sc, 13);
  const LinkConditions l = link(40 * kMs, 5 * kMs, 20'000);
  ClientRig& a = s.addClient(l, l, tokenConfig("alice", 2));  // two local players
  ClientRig& b = s.addClient(l, l, tokenConfig("bob"));
  REQUIRE(s.runUntil([&] { return allRunning(s); }, 2 * kSec));
  s.runFor(4 * kSec);
  const sim::PlayerMask slots = a.client->localSlots();
  REQUIRE(slots == 0b011);
  const PeerId oldPeer = a.transport->self();

  s.network().cut(oldPeer);
  REQUIRE(s.runUntil([&] { return a.client->state() == ClientState::Disconnected; }, kSec));
  s.runFor(2 * kSec);
  // Reserved: still present, nobody owns them, and bob plays on.
  CHECK((s.server().present() & slots) == slots);
  CHECK(!s.server().ownerOf(PlayerId{0}).isValid());
  CHECK(b.client->running());

  s.reconnect(a);
  REQUIRE(s.runUntil([&] { return a.client->running(); }, 2 * kSec));
  CHECK(a.client->localSlots() == slots);
  CHECK(s.server().ownerOf(PlayerId{0}) == a.transport->self());
  CHECK(s.server().stats().reconnects == 1);
  s.runFor(5 * kSec);
  CHECK(a.client->netStats().checksumMismatches == 0);
  CHECK(b.client->netStats().checksumMismatches == 0);
  CHECK(a.client->netStats().resyncRequests == 0);
}

TEST_CASE("a reservation expires after the grace window", "[netcode][reconnect]") {
  ServerConfig sc;
  sc.reconnectGraceTicks = 2 * sim::kTickHz;
  NetSim s(sc, 17);
  const LinkConditions l = link(10 * kMs);
  ClientRig& a = s.addClient(l, l, tokenConfig("carol"));
  s.addClient(l, l, tokenConfig("dave"));
  REQUIRE(s.runUntil([&] { return allRunning(s); }, 2 * kSec));
  s.network().cut(a.transport->self());
  s.runFor(kSec);
  CHECK(sim::maskHas(s.server().present(), PlayerId{0}));
  s.runFor(2 * kSec);
  CHECK(!sim::maskHas(s.server().present(), PlayerId{0}));
  // Coming back after the window is a fresh join into a free slot.
  s.reconnect(a);
  REQUIRE(s.runUntil([&] { return a.client->running(); }, 2 * kSec));
  CHECK(s.server().stats().reconnects == 0);
  CHECK(a.client->localSlots() == 0b001);
}

TEST_CASE("a corrupted client state is caught by the checksum and resynced", "[netcode][resync]") {
  ServerConfig sc;
  sc.match.seed = 0xF;
  NetSim s(sc, 19);
  const LinkConditions l = link(40 * kMs, 5 * kMs, 20'000);
  ClientRig& a = s.addClient(l, l, tokenConfig("eve"));
  s.addClient(l, l, tokenConfig("frank"));
  REQUIRE(s.runUntil([&] { return allRunning(s); }, 2 * kSec));
  s.runFor(5 * kSec);
  REQUIRE(a.client->netStats().checksumMismatches == 0);

  int hooks = 0;
  a.client->setDesyncHook([&](const DesyncInfo& d) {
    ++hooks;
    CHECK(d.local != nullptr);
    CHECK(d.localChecksum != d.serverChecksum);
  });
  a.client->debugCorrupt([](sim::WorldState& w) { w.rng = Rng::seeded(w.rng.next()); });
  REQUIRE(s.runUntil([&] { return a.client->netStats().checksumMismatches > 0; }, kSec));
  REQUIRE(s.runUntil([&] { return a.client->netStats().snapshotsLoaded >= 2; }, 2 * kSec));
  CHECK(hooks == 1);
  CHECK(a.client->netStats().resyncRequests == 1);
  CHECK(s.server().stats().resyncsServed == 1);

  const uint64_t compared = a.client->netStats().checksumsCompared;
  s.runFor(5 * kSec);
  CHECK(a.client->netStats().checksumsCompared > compared + 20);
  CHECK(a.client->netStats().checksumMismatches == 1);
  CHECK(a.client->running());
}

TEST_CASE("input delay trades local latency for shallower rollbacks", "[netcode][delay]") {
  const auto run = [](uint8_t delay) {
    ServerConfig sc;
    sc.match.seed = 0x1D;
    NetSim s(sc, 23);
    const LinkConditions l = link(40 * kMs, 5 * kMs);
    for (int i = 0; i < 4; ++i) {
      ClientConfig c = tokenConfig("p" + std::to_string(i));
      c.inputDelayTicks = delay;
      s.addClient(l, l, c);
    }
    REQUIRE(s.runUntil([&] { return allRunning(s); }, 2 * kSec));
    s.runFor(15 * kSec);
    uint64_t depth = 0;
    uint64_t rollbacks = 0;
    for (const ClientRig& r : s.rigs()) {
      CHECK(r.client->netStats().checksumMismatches == 0);
      CHECK(r.client->netStats().substitutionsSeen < 20);
      depth += r.client->netStats().resimulatedTicks;
      rollbacks += r.client->netStats().rollbacks;
    }
    return rollbacks > 0 ? static_cast<double>(depth) / static_cast<double>(rollbacks) : 0.0;
  };
  const double none = run(0);
  const double two = run(2);
  INFO("mean rollback depth without delay " << none << ", with 2 ticks " << two);
  CHECK(two < none - 1.0);
}

TEST_CASE("a match restart reloads every client and spectator without a resync",
          "[netcode][restart]") {
  ServerConfig sc;
  sc.match.seed = 0x5E1;
  NetSim s(sc, 23);
  const LinkConditions l = link(50 * kMs, 10 * kMs, 20'000);
  s.addClient(l, l, tokenConfig("a"));
  s.addClient(l, l, tokenConfig("b"));
  s.addClient(l, l, tokenConfig("c"));
  s.addClient(l, l, tokenConfig("watcher", 0));
  REQUIRE(s.server().setBot(PlayerId{7}, true));
  REQUIRE(s.runUntil([&] { return allRunning(s); }, 3 * kSec));
  s.runFor(5 * kSec);

  // Announced 1.5 s ahead, as the dedicated server does after a match ends.
  const Tick start = s.server().nextTick().plus(90);
  s.server().restartMatch(0x5E2, start, 0xB2);
  std::vector<uint64_t> comparedAtRestart;
  REQUIRE(s.runUntil([&] { return s.server().matchIndex() == 1; }, 3 * kSec));
  for (const ClientRig& r : s.rigs()) {
    CHECK(r.client->pendingRestart() == start);
    comparedAtRestart.push_back(r.client->netStats().checksumsCompared);
  }
  const auto allRestarted = [&] {
    return std::ranges::all_of(
        s.rigs(), [](const ClientRig& r) { return r.client->netStats().matchRestarts == 1; });
  };
  REQUIRE(s.runUntil(allRestarted, 3 * kSec));
  s.runFor(5 * kSec);

  size_t i = 0;
  for (const ClientRig& r : s.rigs()) {
    const ClientNetStats& st = r.client->netStats();
    INFO("client " << i);
    CHECK(r.client->running());
    CHECK(!r.client->pendingRestart());
    CHECK(r.client->matchConfig().seed == 0x5E2);
    CHECK(st.checksumMismatches == 0);
    CHECK(st.resyncRequests == 0);
    CHECK(st.checksumsCompared > comparedAtRestart[i] + 20);  // the new match is verified
    ++i;
  }
}

TEST_CASE("an immediate restart on a lossless link needs no resync either", "[netcode][restart]") {
  ServerConfig sc;
  sc.match.seed = 0x5E3;
  sc.checksumEveryTicks = 1;
  NetSim s(sc, 29);
  const LinkConditions l = link(20 * kMs, 0, 0);
  s.addClient(l, l, tokenConfig("a"));
  s.addClient(l, l, tokenConfig("b"));
  REQUIRE(s.runUntil([&] { return allRunning(s); }, 3 * kSec));
  s.runFor(3 * kSec);
  s.server().restartMatch(0x5E4, s.server().nextTick(), 1);
  s.runFor(4 * kSec);
  for (const ClientRig& r : s.rigs()) {
    const ClientNetStats& st = r.client->netStats();
    CHECK(st.matchRestarts == 1);
    CHECK(st.checksumMismatches == 0);
    CHECK(st.resyncRequests == 0);
  }
}
