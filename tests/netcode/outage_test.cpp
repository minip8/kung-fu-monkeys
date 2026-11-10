// Burst loss over the loopback emulator (architecture §6.6 "A gap in the
// authoritative stream"): one direction of a link blacks out for a while
// without disconnecting. The server resends at most authRedundancy frames
// per AuthInputs, so a server-to-client outage longer than that leaves a gap
// no later message fills; the client must notice and resync, a player and a
// spectator alike, and be back in agreement with the server soon after.
#include "kfm/net/netsim.hpp"
#include "kfm/sim/step.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace kfm;
using namespace kfm::net;
using namespace kfm::net::netsim;

namespace {

struct Baseline {
  uint64_t compared{0};
  uint64_t mismatches{0};
  uint64_t resyncs{0};
  uint64_t overflows{0};
  uint64_t snapshots{0};
};

Baseline baseline(const ClientRig& r) {
  const ClientNetStats& st = r.client->netStats();
  return {st.checksumsCompared, st.checksumMismatches, st.resyncRequests, st.rollbackOverflows,
          st.snapshotsLoaded};
}

// In agreement: the client holds every authoritative frame to within a few
// ticks of the server's newest and has compared a checksum since `b`; a
// spectator also stands on the server's state of its tick.
bool agrees(const NetSim& s, const ClientRig& r, const Baseline& b) {
  const NetClient& c = *r.client;
  const std::optional<Tick> confirmed = c.confirmedTick();
  if (!c.running() || !confirmed) return false;
  const int32_t behind = s.server().nextTick().minus(*confirmed);
  if (behind > 20) return false;
  if (c.netStats().checksumsCompared <= b.compared) return false;
  if (c.spectator() && sim::checksum(c.world()) != s.serverSum(c.localTick())) return false;
  return true;
}

struct OutageCase {
  const char* name;
  uint64_t durationNs;
  bool toServer;  // false: server to client
  bool reliable;  // the reliable lane too
};

void runOutage(const OutageCase& oc) {
  ServerConfig sc;
  sc.match.seed = 0x0A7A;
  NetSim s(sc, 0x0A7A);
  const LinkConditions l = link(40 * kMs, 5 * kMs);
  ClientRig& a = s.addClient(l, l, tokenConfig("a"));
  ClientRig& b = s.addClient(l, l, tokenConfig("b"));
  ClientRig& w = s.addClient(l, l, tokenConfig("watcher", 0));
  REQUIRE(s.runUntil([&] { return s.allRunning(); }, 3 * kSec));
  s.runFor(4 * kSec);
  // The server's lead estimate of the player that loses the link, in ticks.
  const auto leadOfA = [&] {
    const std::optional<ServerPeerInfo> info = s.server().peerInfo(a.transport->self());
    return info ? (info->leadEmaQ8 + 128) / 256 : 0;
  };
  const int32_t leadBefore = leadOfA();

  // Both a player and the spectator lose the link's chosen direction.
  const uint64_t from = s.now();
  const uint64_t until = from + oc.durationNs;
  for (const ClientRig* r : {&a, &w}) {
    s.network().setOutage(r->transport->self(), oc.toServer, Outage{from, until, oc.reliable});
  }
  int32_t leadMax = leadBefore;
  // When the player first asks for a snapshot, from the outage's start.
  const uint64_t resyncsBefore = a.client->netStats().resyncRequests;
  uint64_t firstResyncMs = 0;
  s.afterServerTick = [&] {
    leadMax = std::max(leadMax, leadOfA());
    if (firstResyncMs == 0 && a.client->netStats().resyncRequests > resyncsBefore)
      firstResyncMs = (s.now() - from) / kMs;
  };
  s.runFor(oc.durationNs);
  const Baseline a0 = baseline(a);
  const Baseline w0 = baseline(w);
  const Baseline b0 = baseline(b);

  // Bounded recovery: 1 s after the outage (RTT 80 ms).
  uint64_t aMs = 0;
  uint64_t wMs = 0;
  const uint64_t end = s.now();
  const bool recovered = s.runUntil(
      [&] {
        if (aMs == 0 && agrees(s, a, a0)) aMs = (s.now() - end) / kMs;
        if (wMs == 0 && agrees(s, w, w0)) wMs = (s.now() - end) / kMs;
        return aMs != 0 && wMs != 0;
      },
      kSec);
  const ClientNetStats& sa = a.client->netStats();
  const ClientNetStats& sw = w.client->netStats();
  std::printf(
      "[outage] %s: player %s after %llu ms (resyncs +%llu, overflows +%llu, snapshots +%llu, "
      "compared +%llu, mismatches +%llu, gap resyncs %llu), spectator %s after %llu ms (resyncs "
      "+%llu, snapshots +%llu, compared +%llu, tick %u vs server %u), server lead %d -> max %d\n",
      oc.name, aMs != 0 ? "agrees" : "DOES NOT AGREE", static_cast<unsigned long long>(aMs),
      static_cast<unsigned long long>(sa.resyncRequests - a0.resyncs),
      static_cast<unsigned long long>(sa.rollbackOverflows - a0.overflows),
      static_cast<unsigned long long>(sa.snapshotsLoaded - a0.snapshots),
      static_cast<unsigned long long>(sa.checksumsCompared - a0.compared),
      static_cast<unsigned long long>(sa.checksumMismatches - a0.mismatches),
      static_cast<unsigned long long>(sa.gapResyncs), wMs != 0 ? "agrees" : "DOES NOT AGREE",
      static_cast<unsigned long long>(wMs),
      static_cast<unsigned long long>(sw.resyncRequests - w0.resyncs),
      static_cast<unsigned long long>(sw.snapshotsLoaded - w0.snapshots),
      static_cast<unsigned long long>(sw.checksumsCompared - w0.compared),
      w.client->localTick().value, s.server().nextTick().value, leadBefore, leadMax);
  CHECK(recovered);
  // One snapshot each repairs a lost stretch of the stream; nothing to
  // repair when only the inputs were lost.
  const uint64_t want = oc.toServer ? 0 : 1;
  CHECK(sa.resyncRequests - a0.resyncs == want);
  CHECK(sw.resyncRequests - w0.resyncs == want);
  CHECK(sa.gapResyncs == want);
  CHECK(sw.gapResyncs == want);

  // And it stays so: checksums keep being compared and keep matching.
  const Baseline a1 = baseline(a);
  const Baseline w1 = baseline(w);
  s.runFor(3 * kSec);
  s.afterServerTick = nullptr;
  std::printf(
      "[outage] %s, 3 s later: player resyncs +%llu, overflows +%llu, compared +%llu; spectator "
      "resyncs +%llu, tick %u vs server %u; server lead max %d; first player resync %llu ms after "
      "the outage began\n",
      oc.name, static_cast<unsigned long long>(sa.resyncRequests - a0.resyncs),
      static_cast<unsigned long long>(sa.rollbackOverflows - a0.overflows),
      static_cast<unsigned long long>(sa.checksumsCompared - a0.compared),
      static_cast<unsigned long long>(sw.resyncRequests - w0.resyncs), w.client->localTick().value,
      s.server().nextTick().value, leadMax, static_cast<unsigned long long>(firstResyncMs));
  for (const auto& [r, b1] : {std::pair{&a, a1}, std::pair{&w, w1}, std::pair{&b, b0}}) {
    const ClientNetStats& st = r->client->netStats();
    CHECK(r->client->running());
    CHECK(st.checksumsCompared > b1.compared + 10);
    CHECK(st.checksumMismatches == 0);
    CHECK(st.rollbackOverflows == 0);
  }
  CHECK(agrees(s, w, w1));
  // The untouched player never needed a snapshot.
  CHECK(b.client->netStats().resyncRequests == 0);
  // Stale acknowledgements during the outage do not inflate the human lead
  // the bots are delayed by (§8 human-likeness).
  CHECK(leadMax <= leadBefore + 2);
}

}  // namespace

TEST_CASE("a 0.4 s server-to-client outage: the player and the spectator recover",
          "[netcode][outage]") {
  runOutage({"s->c 0.4 s", 400 * kMs, false, false});
}

TEST_CASE("a 1 s server-to-client outage: the player and the spectator recover",
          "[netcode][outage]") {
  runOutage({"s->c 1 s", kSec, false, false});
}

TEST_CASE("a 3 s server-to-client outage: the player and the spectator recover",
          "[netcode][outage]") {
  runOutage({"s->c 3 s", 3 * kSec, false, false});
}

TEST_CASE("a 1 s server-to-client outage of both lanes: the player and the spectator recover",
          "[netcode][outage]") {
  runOutage({"s->c 1 s both lanes", kSec, false, true});
}

TEST_CASE("a 1 s client-to-server outage needs no repair", "[netcode][outage]") {
  runOutage({"c->s 1 s", kSec, true, false});
}

TEST_CASE("an outage shorter than the redundancy needs no snapshot", "[netcode][outage]") {
  // 150 ms = 9 ticks: every frame of the gap rides again in a later message.
  const ServerConfig sc;
  NetSim s(sc, 0x0A7B);
  const LinkConditions l = link(40 * kMs, 5 * kMs);
  ClientRig& a = s.addClient(l, l, tokenConfig("a"));
  ClientRig& w = s.addClient(l, l, tokenConfig("watcher", 0));
  REQUIRE(s.runUntil([&] { return s.allRunning(); }, 3 * kSec));
  s.runFor(2 * kSec);
  for (const ClientRig* r : {&a, &w})
    s.network().setOutage(r->transport->self(), false, Outage{s.now(), s.now() + 150 * kMs});
  s.runFor(3 * kSec);
  for (const ClientRig* r : {&a, &w}) {
    const ClientNetStats& st = r->client->netStats();
    CHECK(st.resyncRequests == 0);
    CHECK(st.checksumMismatches == 0);
    CHECK(st.snapshotsLoaded == 1);
  }
  CHECK(agrees(s, w, {}));
}
