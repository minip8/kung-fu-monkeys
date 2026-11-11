// Burst loss over the loopback emulator (architecture §6.6 "A gap in the
// authoritative stream"): one direction of a link blacks out for a while
// without disconnecting. The server resends at most authRedundancy frames
// per AuthInputs, so a server-to-client outage longer than that leaves a gap
// no later message fills; the client must notice and resync, a player and a
// spectator alike, and be back in agreement with the server soon after.
//
// Issue #66 widens the conditions: the high-RTT links of §6.2, independent
// loss on top of an outage, a ResyncRequest or a Snapshot held on the
// reliable lane (around the server's minResyncIntervalNs and the client's
// resyncRetryNs) and two outages in quick succession. Every case waits for
// agreement no longer than recoveryBoundNs(), derived from those constants.
#include "kfm/net/netsim.hpp"
#include "kfm/sim/step.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace kfm;
using namespace kfm::net;
using namespace kfm::net::netsim;

namespace {

// The largest server tick period, rounded up (16.67 ms).
constexpr uint64_t kTickNs = (kSec + sim::kTickHz - 1) / sim::kTickHz;

// Both directions of every link of a case: netsim's link(latency, jitter,
// loss), a one-way delay in [latency − jitter, latency + jitter].
struct LinkSpec {
  uint64_t latencyNs{40 * kMs};
  uint64_t jitterNs{5 * kMs};
  uint32_t lossPpm{0};

  [[nodiscard]] LinkConditions conditions() const { return link(latencyNs, jitterNs, lossPpm); }
  [[nodiscard]] uint64_t hopNs() const { return latencyNs + jitterNs; }  // one way, at most
  [[nodiscard]] uint64_t retransmitNs() const { return conditions().reliable.retransmitNs; }
};

// How far the client's confirmed frames may trail the server's newest and
// still count as caught up: the frames in flight for one hop plus the 17-tick
// slack of the original 40 ± 5 ms cases (3 + 17 = 20 ticks there).
int32_t maxBehindTicks(const LinkSpec& l) {
  return static_cast<int32_t>((l.hopNs() + kTickNs - 1) / kTickNs) + 17;
}

struct Baseline {
  uint64_t compared{0};
  uint64_t mismatches{0};
  uint64_t resyncs{0};
  uint64_t gapResyncs{0};
  uint64_t overflows{0};
  uint64_t snapshots{0};
};

Baseline baseline(const ClientRig& r) {
  const ClientNetStats& st = r.client->netStats();
  return {st.checksumsCompared, st.checksumMismatches, st.resyncRequests,
          st.gapResyncs,        st.rollbackOverflows,  st.snapshotsLoaded};
}

// In agreement: the client holds every authoritative frame to within a few
// ticks of the server's newest and has compared a checksum since `b`; a
// spectator also stands on the server's state of its tick.
bool agrees(const NetSim& s, const ClientRig& r, const Baseline& b, int32_t maxBehind = 20) {
  const NetClient& c = *r.client;
  const std::optional<Tick> confirmed = c.confirmedTick();
  if (!c.running() || !confirmed) return false;
  const int32_t behind = s.server().nextTick().minus(*confirmed);
  if (behind > maxBehind) return false;
  if (c.netStats().checksumsCompared <= b.compared) return false;
  if (c.spectator() && sim::checksum(c.world()) != s.serverSum(c.localTick())) return false;
  return true;
}

// The server's answers to one peer's ResyncRequests.
uint64_t served(const NetSim& s, const ClientRig& r) {
  const std::optional<ServerPeerInfo> info = s.server().peerInfo(r.transport->self());
  return info ? info->resyncsServed : 0;
}

// Reliable retransmissions the emulator drew on a client's link, both ways.
uint64_t retransmits(NetSim& s, const ClientRig& r) {
  const PeerId id = r.transport->self();
  return s.network().emulatorStats(id, true).reliableRetransmits +
         s.network().emulatorStats(id, false).reliableRetransmits;
}

// The reliable messages an outage held on a client's link, both ways.
uint64_t held(NetSim& s, const ClientRig& r) {
  const PeerId id = r.transport->self();
  return s.network().emulatorStats(id, true).reliableHeld +
         s.network().emulatorStats(id, false).reliableHeld;
}

// The repair's critical path after a server-to-client outage ends, from its
// own constants (§6.6), each hop at most one one-way delay (l.hopNs()):
//   1. the server's next AuthInputs (sent within a tick) reaches the client,
//      whose window then starts past the frame it lacks: 1 tick + 1 hop;
//   2. the client waits gapResyncNs and sends a ResyncRequest on its next
//      frame (1 ms apart): gapResyncNs + 1 ms;
//   3. the request reaches the server, which serves it at its next pump:
//      1 hop + 1 ms;
//   4. the snapshot (1 hop) and the first checksum after its tick (sent at
//      most checksumEveryTicks ticks after serving, then 1 hop) reach the
//      client, which compares it on its next frame: checksumEveryTicks ticks
//      + 1 hop + 1 ms (the snapshot arrives inside that).
// = 3 hops + gapResyncNs + (checksumEveryTicks + 1) ticks + 3 ms.
// With independent loss, the first window after the outage and the first
// checksum-bearing AuthInputs may be lost too: two more checksum periods
// (a checksum lost twice in a row has probability p², 1% at 10%), and each
// reliable retransmission the emulator drew (on the request or the snapshot,
// the only reliable traffic in steady play) adds retransmitNs.
// A reliable message held until holdNs after the outage's end takes the path
// from step 3 or 4 from there: holdNs + 2 hops + (checksumEveryTicks + 1)
// ticks + 3 ms, whichever is later. The server's minResyncIntervalNs never
// lengthens the path: it drops a request only within 250 ms of serving an
// earlier one, whose snapshot is then still on its way down the same ordered
// lane or already loaded; and a new gap after a loaded snapshot of tick T
// needs a window starting past T + 1, sent authRedundancy (16) ticks after
// T, plus gapResyncNs, 417 ms after serving at the earliest.
uint64_t recoveryBoundNs(const LinkSpec& l, const ServerConfig& sc, const ClientConfig& cc,
                         uint64_t retransmitsDrawn, uint64_t holdNs = 0) {
  const uint64_t checksumNs = uint64_t{sc.checksumEveryTicks} * kTickNs;
  const uint64_t tail = checksumNs + kTickNs + 3 * kMs;
  uint64_t bound = 3 * l.hopNs() + cc.gapResyncNs + tail;
  if (holdNs > 0) bound = std::max(bound, holdNs + 2 * l.hopNs() + tail);
  if (l.lossPpm > 0) bound += 2 * checksumNs;
  return bound + retransmitsDrawn * l.retransmitNs();
}

// Two players (`a` loses its link, `b` does not) and a spectator `w` (loses
// its link too) on the same link conditions, warmed up for 4 s.
struct Rigs {
  ClientRig& a;
  ClientRig& b;
  ClientRig& w;
};

Rigs addRigs(NetSim& s, const LinkSpec& spec) {
  const LinkConditions l = spec.conditions();
  ClientRig& a = s.addClient(l, l, tokenConfig("a"));
  ClientRig& b = s.addClient(l, l, tokenConfig("b"));
  ClientRig& w = s.addClient(l, l, tokenConfig("watcher", 0));
  REQUIRE(s.runUntil([&] { return s.allRunning(); }, 3 * kSec));
  s.runFor(4 * kSec);
  return {a, b, w};
}

// The server's lead estimate of a player, in ticks.
int32_t leadOf(const NetSim& s, const ClientRig& r) {
  const std::optional<ServerPeerInfo> info = s.server().peerInfo(r.transport->self());
  return info ? (info->leadEmaQ8 + 128) / 256 : 0;
}

// Whether a player's misprediction can fall outside both the 32-state ring
// and the 128-tick frame history (the rollback-overflow resync, §6.2's
// backstop) while it repairs a gap. The first frames after the outage are
// authRedundancy ticks older than the server's newest, which the client
// sees a hop late while predicting `lead` ticks ahead: when that reaches
// kRollbackWindow, their misprediction can only be re-simulated from the
// anchor at verified_, which the outage left more than kInputHistoryTicks
// behind for long outages, and the backstop asks for the snapshot (on the
// first window, before gapResyncNs passes) or waits for the one the gap
// asked for: one rollback-overflow episode either way. Below it the ring
// holds the state and the backstop never fires.
bool overflowPossible(const ServerConfig& sc, const LinkSpec& l, const ClientRig& a) {
  const auto hopTicks = static_cast<int32_t>((l.hopNs() + kTickNs - 1) / kTickNs);
  const int32_t reach = int32_t{sc.authRedundancy} + hopTicks + a.client->netStats().leadTicks;
  return std::cmp_greater_equal(reach, kRollbackWindow);
}

// Every ResyncRequest of a repair is the gap's (gapResyncs) or a
// rollback-overflow episode's; never a mismatch's. `overflows` is the
// case's number of episodes, which only overflowPossible() allows. An
// episode counts once until a snapshot loads and sends a request of its
// own only when no gap's is outstanding (none of these cases holds one
// long enough to retry it), so gap <= requests <= gap + episodes, and each
// episode ends in a loaded snapshot.
void checkRequests(const ClientRig& c, const Baseline& base, uint64_t requests, uint64_t overflows,
                   bool overflowPossible) {
  const ClientNetStats& st = c.client->netStats();
  const uint64_t req = st.resyncRequests - base.resyncs;
  const uint64_t gap = st.gapResyncs - base.gapResyncs;
  const uint64_t episodes = st.rollbackOverflows - base.overflows;
  const uint64_t loaded = st.snapshotsLoaded - base.snapshots;
  INFO("requests " << req << ", gap " << gap << ", overflows +" << episodes << ", snapshots +"
                   << loaded);
  CHECK((overflowPossible || overflows == 0));
  CHECK(req == requests);
  CHECK(st.checksumMismatches == base.mismatches);
  CHECK(episodes == overflows);
  CHECK(gap <= req);
  CHECK(req <= gap + episodes);
  CHECK(episodes <= loaded);
}

// When one client first agrees again after s.now() at the call, and the
// reliable retransmissions drawn on its link until then.
struct Agreement {
  std::optional<uint64_t> afterNs;
  uint64_t retransmits{0};
};

// Runs until both the player and the spectator agree (or capNs passes).
std::pair<Agreement, Agreement> awaitAgreement(NetSim& s, const ClientRig& a, const Baseline& a0,
                                               const ClientRig& w, const Baseline& w0,
                                               int32_t maxBehind, uint64_t capNs) {
  const uint64_t start = s.now();
  const uint64_t ra0 = retransmits(s, a);
  const uint64_t rw0 = retransmits(s, w);
  Agreement pa;
  Agreement pw;
  static_cast<void>(s.runUntil(
      [&] {
        if (!pa.afterNs && agrees(s, a, a0, maxBehind)) {
          pa.afterNs = s.now() - start;
          pa.retransmits = retransmits(s, a) - ra0;
        }
        if (!pw.afterNs && agrees(s, w, w0, maxBehind)) {
          pw.afterNs = s.now() - start;
          pw.retransmits = retransmits(s, w) - rw0;
        }
        return pa.afterNs && pw.afterNs;
      },
      capNs));
  return {pa, pw};
}

// Checks one client agreed within the bound for its link and the
// retransmissions drawn on it.
void checkWithin(const char* who, const Agreement& g, const LinkSpec& l, const ServerConfig& sc,
                 const ClientConfig& cc, uint64_t holdNs) {
  const uint64_t bound = recoveryBoundNs(l, sc, cc, g.retransmits, holdNs);
  INFO(who << " bound " << bound / kMs << " ms, " << g.retransmits << " retransmits drawn");
  REQUIRE(g.afterNs.has_value());
  CHECK(g.afterNs.value_or(0) <= bound);
}

// And it stays so for 3 s: checksums keep being compared and keep matching,
// for every client, no new overflow episode starts and no snapshot is asked
// for or loaded (an episode still awaiting one would ask again within
// resyncRetryNs), and the spectator still stands on the server's state.
void checkStaysInAgreement(NetSim& s, const Rigs& r, const Baseline& b0, int32_t maxBehind) {
  const Baseline a1 = baseline(r.a);
  const Baseline w1 = baseline(r.w);
  const Baseline b1 = baseline(r.b);
  s.runFor(3 * kSec);
  for (const auto& [rig, base] : {std::pair{&r.a, a1}, std::pair{&r.w, w1}, std::pair{&r.b, b1}}) {
    const ClientNetStats& st = rig->client->netStats();
    CHECK(rig->client->running());
    CHECK(st.checksumsCompared > base.compared + 10);
    CHECK(st.checksumMismatches == 0);
    CHECK(st.rollbackOverflows == base.overflows);
    CHECK(st.resyncRequests == base.resyncs);
    CHECK(st.snapshotsLoaded == base.snapshots);
  }
  CHECK(agrees(s, r.w, w1, maxBehind));
  // The untouched player never needed a snapshot, and never overflowed.
  CHECK(r.b.client->netStats().resyncRequests == b0.resyncs);
  CHECK(r.b.client->netStats().rollbackOverflows == b0.overflows);
}

// Issue #84: a spectator steps only authoritative frames, spectatorDelayTicks
// behind the newest it has confirmed: its backlog, confirmed + 1 − its tick,
// is what ticksDue() and spectatorRatePpm() hold at the delay. A snapshot
// that arrives late while the unreliable stream kept coming (the "snapshot
// held" cases; a reliable retransmission after loss) is older than the
// frames the spectator holds, and the load runs it over them to the delay
// at once. At the end of the client frame that loaded a snapshot the
// backlog is then at most the delay plus the frames that same frame
// received after the snapshot: AuthInputs leave one per server tick (at
// least kSec / kTickHz apart) with one-way delays at most 2 · jitter apart,
// and a frame's 1 ms of messages is delivered in arrival order, so the
// newest frame moves at most ceil((2 · jitter + 1 ms) / tick) ticks past the
// newest one before. (On a lossy link each AuthInputs lost just before
// those adds its tick; the bound leaves no room for that, and the lossy
// cases here meet it anyway.) A snapshot not older than the stream leaves a
// backlog under the delay, as before the fix. Before it, the backlog after
// a late load was the snapshot's age: 15 and 16 ticks after the 1.5 s holds,
// 18 at 300 ms with 10% loss, drained at lead.maxDeviationPpm (2%).
int32_t arrivalTicks(const LinkSpec& l) {
  constexpr uint64_t kMinTickNs = kSec / sim::kTickHz;
  return static_cast<int32_t>((2 * l.jitterNs + kMs + kMinTickNs - 1) / kMinTickNs);
}

// Samples the spectator at the end of every client frame in which it loaded
// a snapshot.
struct SpectatorLoads {
  const ClientRig* rig{nullptr};
  uint64_t seen{0};       // its snapshotsLoaded so far
  uint64_t loaded{0};     // snapshots loaded since the first sample's baseline
  int32_t maxBacklog{0};  // the largest backlog at the end of a loading frame
  uint32_t offServer{0};  // loading frames that left it off the server's state

  void sample(const NetSim& s, const ClientRig& c) {
    if (&c != rig) return;
    const NetClient& cl = *c.client;
    const uint64_t n = cl.netStats().snapshotsLoaded;
    if (n == seen) return;
    loaded += n - seen;
    seen = n;
    const std::optional<Tick> confirmed = cl.confirmedTick();
    const int32_t backlog = confirmed ? confirmed->plus(1).minus(cl.localTick()) : 0;
    maxBacklog = std::max(maxBacklog, backlog);
    if (sim::checksum(cl.world()) != s.serverSum(cl.localTick())) ++offServer;
  }

  // At the end of each loading frame: within arrivalTicks() of the delay
  // and on the server's state of its tick (it stepped authoritative frames
  // only), for every snapshot the case expects.
  void check(const char* name, const LinkSpec& l, uint64_t expectLoads) const {
    const int32_t delay = ClientConfig{}.spectatorDelayTicks;
    std::printf(
        "[spectator] %s: %llu snapshots loaded, backlog at most %d after a load (bound %d "
        "= delay %d + %d)\n",
        name, static_cast<unsigned long long>(loaded), maxBacklog, delay + arrivalTicks(l), delay,
        arrivalTicks(l));
    INFO(name);
    CHECK(loaded == expectLoads);
    CHECK(offServer == 0);
    CHECK(maxBacklog <= delay + arrivalTicks(l));
  }
};

// A reliable-only hold (Outage{.., reliable, !unreliable}) of one direction
// of the player's and the spectator's links from the outage's end for ns:
// the targeted loss of the ResyncRequest (toServer) or of the Snapshot's
// first attempt (server to client), retransmitted when the hold ends.
struct Hold {
  bool toServer{false};
  uint64_t ns{0};
};

// The repair each affected client is expected to make, counted from before
// the outage.
struct Expect {
  uint64_t requests{1};  // ResyncRequests sent, all for the gap
  uint64_t served{1};    // answered by the server (the rest dropped by its limit)
  uint64_t held{0};      // reliable messages the hold delayed
};

struct OutageCase {
  const char* name{""};
  uint64_t durationNs{0};
  bool toServer{false};  // false: server to client
  bool reliable{false};  // the reliable lane too
  LinkSpec link{};
  uint64_t seed{0x0A7A};
  Hold hold{};
  std::optional<Expect> expect{std::nullopt};  // default: one repair, none for client to server
  uint64_t overflows{0};  // the player's rollback-overflow episodes (the spectator never has one)
};

void runOutage(const OutageCase& oc) {
  ServerConfig sc;
  sc.match.seed = oc.seed;
  const ClientConfig cc = tokenConfig("a");
  NetSim s(sc, oc.seed);
  const Rigs r = addRigs(s, oc.link);
  ClientRig& a = r.a;
  ClientRig& w = r.w;
  const int32_t maxBehind = maxBehindTicks(oc.link);
  const int32_t leadBefore = leadOf(s, a);
  const bool overflow = overflowPossible(sc, oc.link, a);
  const Baseline a0 = baseline(a);
  const Baseline w0 = baseline(w);
  const Baseline b0 = baseline(r.b);
  const uint64_t aServed0 = served(s, a);
  const uint64_t wServed0 = served(s, w);

  // Both a player and the spectator lose the link's chosen direction.
  const uint64_t from = s.now();
  const uint64_t until = from + oc.durationNs;
  for (const ClientRig* c : {&a, &w}) {
    s.network().setOutage(c->transport->self(), oc.toServer, Outage{from, until, oc.reliable});
    if (oc.hold.ns > 0 && oc.hold.toServer != oc.toServer) {
      s.network().setOutage(c->transport->self(), oc.hold.toServer,
                            Outage{until, until + oc.hold.ns, true, false});
    }
  }
  int32_t leadMax = leadBefore;
  SpectatorLoads loads{&w, w0.snapshots};
  s.afterClientFrame = [&](ClientRig& c) { loads.sample(s, c); };
  // When the player first asks for a snapshot, from the outage's start.
  uint64_t firstResyncMs = 0;
  s.afterServerTick = [&] {
    leadMax = std::max(leadMax, leadOf(s, a));
    if (firstResyncMs == 0 && a.client->netStats().resyncRequests > a0.resyncs)
      firstResyncMs = (s.now() - from) / kMs;
  };
  s.runFor(oc.durationNs);
  // A hold of the outage's own direction replaces it once it is over.
  if (oc.hold.ns > 0 && oc.hold.toServer == oc.toServer) {
    for (const ClientRig* c : {&a, &w}) {
      s.network().setOutage(c->transport->self(), oc.hold.toServer,
                            Outage{until, until + oc.hold.ns, true, false});
    }
  }
  const Baseline a1 = baseline(a);
  const Baseline w1 = baseline(w);

  // Bounded recovery (recoveryBoundNs); the loop runs 2 s past the bound so
  // a late recovery reports how late.
  const uint64_t cap = recoveryBoundNs(oc.link, sc, cc, 0, oc.hold.ns) + 2 * kSec;
  const auto [pa, pw] = awaitAgreement(s, a, a1, w, w1, maxBehind, cap);
  // Let a held retry, or its snapshot, land before counting.
  s.runFor(oc.link.hopNs() * 2 + 20 * kMs);
  const ClientNetStats& sa = a.client->netStats();
  const ClientNetStats& sw = w.client->netStats();
  std::printf(
      "[outage] %s: player %s after %llu ms of %llu (resyncs +%llu, gap +%llu, served +%llu, "
      "snapshots +%llu, overflows +%llu, compared +%llu, retransmits %llu, held %llu), "
      "spectator %s after %llu ms of %llu (resyncs +%llu, gap +%llu, served +%llu, snapshots "
      "+%llu, retransmits %llu, held %llu, tick %u vs server %u), server lead %d -> max %d, "
      "first player resync %llu ms after the outage began, overflow %s\n",
      oc.name, pa.afterNs ? "agrees" : "DOES NOT AGREE",
      static_cast<unsigned long long>(pa.afterNs.value_or(0) / kMs),
      static_cast<unsigned long long>(recoveryBoundNs(oc.link, sc, cc, pa.retransmits, oc.hold.ns) /
                                      kMs),
      static_cast<unsigned long long>(sa.resyncRequests - a0.resyncs),
      static_cast<unsigned long long>(sa.gapResyncs - a0.gapResyncs),
      static_cast<unsigned long long>(served(s, a) - aServed0),
      static_cast<unsigned long long>(sa.snapshotsLoaded - a0.snapshots),
      static_cast<unsigned long long>(sa.rollbackOverflows - a0.overflows),
      static_cast<unsigned long long>(sa.checksumsCompared - a1.compared),
      static_cast<unsigned long long>(pa.retransmits), static_cast<unsigned long long>(held(s, a)),
      pw.afterNs ? "agrees" : "DOES NOT AGREE",
      static_cast<unsigned long long>(pw.afterNs.value_or(0) / kMs),
      static_cast<unsigned long long>(recoveryBoundNs(oc.link, sc, cc, pw.retransmits, oc.hold.ns) /
                                      kMs),
      static_cast<unsigned long long>(sw.resyncRequests - w0.resyncs),
      static_cast<unsigned long long>(sw.gapResyncs - w0.gapResyncs),
      static_cast<unsigned long long>(served(s, w) - wServed0),
      static_cast<unsigned long long>(sw.snapshotsLoaded - w0.snapshots),
      static_cast<unsigned long long>(pw.retransmits), static_cast<unsigned long long>(held(s, w)),
      w.client->localTick().value, s.server().nextTick().value, leadBefore, leadMax,
      static_cast<unsigned long long>(firstResyncMs), overflow ? "possible" : "impossible");
  checkWithin("player", pa, oc.link, sc, cc, oc.hold.ns);
  checkWithin("spectator", pw, oc.link, sc, cc, oc.hold.ns);
  // One snapshot each repairs a lost stretch of the stream unless the case
  // says otherwise; nothing to repair when only the inputs were lost. Every
  // snapshot served is loaded. A spectator never predicts, so never overflows.
  const uint64_t want = oc.toServer ? 0 : 1;
  const Expect e = oc.expect.value_or(Expect{want, want, 0});
  checkRequests(a, a0, e.requests, oc.overflows, overflow);
  checkRequests(w, w0, e.requests, 0, false);
  for (const auto& [c, base, srv0] : {std::tuple{&a, a0, aServed0}, std::tuple{&w, w0, wServed0}}) {
    const ClientNetStats& st = c->client->netStats();
    CHECK(served(s, *c) - srv0 == e.served);
    CHECK(st.snapshotsLoaded - base.snapshots == e.served);
    CHECK(held(s, *c) == e.held);
  }

  checkStaysInAgreement(s, r, b0, maxBehind);
  s.afterServerTick = nullptr;
  s.afterClientFrame = nullptr;
  loads.check(oc.name, oc.link, e.served);
  // Stale acknowledgements during the outage do not inflate the human lead
  // the bots are delayed by (§8 human-likeness).
  CHECK(leadMax <= leadBefore + 2);
}

// Two server-to-client outages of the player's and the spectator's links:
// the second, secondNs long, starts for each client as soon as it has sent
// its first ResyncRequest (before the repair completes: its snapshot comes
// down the reliable lane during the second outage) or as soon as it has
// loaded its snapshot (just after the repair completes). Either way the
// second outage leaves a gap of its own (secondNs is longer than the frames
// in flight plus the 16-frame window) and needs a second snapshot, which the
// server's limit does not drop: the second request reaches the server at
// least the window (16 ticks) + gapResyncNs after the first was served,
// past minResyncIntervalNs.
struct TwoOutagesCase {
  const char* name{""};
  uint64_t firstNs{0};
  uint64_t secondNs{0};
  bool afterRepair{false};  // false: the second starts when the first request is sent
  LinkSpec link{};
  uint64_t seed{0x0A7C};
  uint64_t overflows{0};  // the player's rollback-overflow episodes over both repairs
};

void runTwoOutages(const TwoOutagesCase& tc) {
  ServerConfig sc;
  sc.match.seed = tc.seed;
  const ClientConfig cc = tokenConfig("a");
  NetSim s(sc, tc.seed);
  const Rigs r = addRigs(s, tc.link);
  const int32_t maxBehind = maxBehindTicks(tc.link);
  const Baseline b0 = baseline(r.b);
  const int32_t leadBefore = leadOf(s, r.a);
  const bool overflow = overflowPossible(sc, tc.link, r.a);

  struct Track {
    ClientRig* rig;
    Baseline before;
    uint64_t served0;
    std::optional<uint64_t> secondFrom;
  };
  std::vector<Track> tracks;
  for (ClientRig* c : {&r.a, &r.w})
    tracks.push_back({c, baseline(*c), served(s, *c), std::nullopt});
  const uint64_t from = s.now();
  for (const Track& t : tracks)
    s.network().setOutage(t.rig->transport->self(), false, Outage{from, from + tc.firstNs});
  int32_t leadMax = leadBefore;
  s.afterServerTick = [&] { leadMax = std::max(leadMax, leadOf(s, r.a)); };
  SpectatorLoads loads{&r.w, tracks[1].before.snapshots};
  s.afterClientFrame = [&](ClientRig& c) {
    loads.sample(s, c);
    for (Track& t : tracks) {
      if (t.rig != &c || t.secondFrom) continue;
      const ClientNetStats& st = c.client->netStats();
      const bool due = tc.afterRepair ? st.snapshotsLoaded > t.before.snapshots
                                      : st.resyncRequests > t.before.resyncs;
      if (!due) continue;
      t.secondFrom = s.now();
      s.network().setOutage(c.transport->self(), false, Outage{s.now(), s.now() + tc.secondNs});
    }
  };
  // Long enough for both repairs to start (the first bound) and the second
  // outage to end.
  const uint64_t firstBound = recoveryBoundNs(tc.link, sc, cc, 0);
  REQUIRE(s.runUntil([&] { return tracks[0].secondFrom && tracks[1].secondFrom; },
                     tc.firstNs + firstBound));
  // The hook keeps sampling the spectator; both second outages are set.
  const uint64_t secondUntil =
      std::max(tracks[0].secondFrom.value_or(0), tracks[1].secondFrom.value_or(0)) + tc.secondNs;
  s.runFor(secondUntil - s.now());
  // Each client measured from the end of its own second outage.
  const Baseline a1 = baseline(r.a);
  const Baseline w1 = baseline(r.w);
  const auto [pa, pw] = awaitAgreement(s, r.a, a1, r.w, w1, maxBehind, firstBound + 2 * kSec);
  s.runFor(tc.link.hopNs() * 2 + 20 * kMs);
  for (size_t i = 0; i < tracks.size(); ++i) {
    const Track& t = tracks[i];
    const Agreement& g = i == 0 ? pa : pw;
    const uint64_t lag =
        secondUntil - (t.secondFrom.value_or(0) + tc.secondNs);  // its outage ended earlier
    const ClientNetStats& st = t.rig->client->netStats();
    std::printf(
        "[outage] %s: %s, second outage from %llu ms after the first began, %s %llu ms after "
        "it ended, bound %llu (resyncs +%llu, gap +%llu, served +%llu, snapshots +%llu, overflows "
        "+%llu, mismatches +%llu)\n",
        tc.name, i == 0 ? "player" : "spectator",
        static_cast<unsigned long long>((t.secondFrom.value_or(0) - from) / kMs),
        g.afterNs ? "agrees" : "DOES NOT AGREE",
        static_cast<unsigned long long>((g.afterNs.value_or(0) + lag) / kMs),
        static_cast<unsigned long long>(recoveryBoundNs(tc.link, sc, cc, g.retransmits) / kMs),
        static_cast<unsigned long long>(st.resyncRequests - t.before.resyncs),
        static_cast<unsigned long long>(st.gapResyncs - t.before.gapResyncs),
        static_cast<unsigned long long>(served(s, *t.rig) - t.served0),
        static_cast<unsigned long long>(st.snapshotsLoaded - t.before.snapshots),
        static_cast<unsigned long long>(st.rollbackOverflows - t.before.overflows),
        static_cast<unsigned long long>(st.checksumMismatches - t.before.mismatches));
    INFO((i == 0 ? "player" : "spectator"));
    REQUIRE(g.afterNs.has_value());
    CHECK(g.afterNs.value_or(0) + lag <= recoveryBoundNs(tc.link, sc, cc, g.retransmits));
    // One repair per outage, none dropped by the server's limit.
    checkRequests(*t.rig, t.before, 2, i == 0 ? tc.overflows : 0, i == 0 && overflow);
    CHECK(served(s, *t.rig) - t.served0 == 2);
    CHECK(st.snapshotsLoaded - t.before.snapshots == 2);
  }
  checkStaysInAgreement(s, r, b0, maxBehind);
  s.afterServerTick = nullptr;
  s.afterClientFrame = nullptr;
  loads.check(tc.name, tc.link, 2);
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

// --- issue #66: high RTT (§6.2's 200, 300 and 500 ms rows) -------------------------
//
// Where the backstop fires (a 3 s outage at 300 ms, 1 s or more at 500 ms;
// overflowPossible() holds from 300 ms on) the repair is one
// rollback-overflow episode: after 3 s the overflow's own request goes out
// on the first window (gap +0); after 1 s at 500 ms the gap asks first and
// the overflow waits for that snapshot (issue #85: one episode, not one per
// frame awaited).

TEST_CASE("outages at 200 ms RTT ± 30 ms: the player and the spectator recover",
          "[netcode][outage][outage-rtt]") {
  const LinkSpec l{100 * kMs, 30 * kMs};
  runOutage({"200 ms ±30, s->c 0.4 s", 400 * kMs, false, false, l});
  runOutage({"200 ms ±30, s->c 1 s", kSec, false, false, l});
  runOutage({"200 ms ±30, s->c 3 s", 3 * kSec, false, false, l});
}

TEST_CASE("outages at 300 ms RTT ± 10 ms: the player and the spectator recover",
          "[netcode][outage][outage-rtt]") {
  const LinkSpec l{150 * kMs, 10 * kMs};
  runOutage({"300 ms ±10, s->c 0.4 s", 400 * kMs, false, false, l});
  runOutage({"300 ms ±10, s->c 1 s", kSec, false, false, l});
  runOutage({.name = "300 ms ±10, s->c 3 s", .durationNs = 3 * kSec, .link = l, .overflows = 1});
}

TEST_CASE("outages at 500 ms RTT ± 30 ms: the player and the spectator recover",
          "[netcode][outage][outage-rtt]") {
  const LinkSpec l{250 * kMs, 30 * kMs};
  runOutage({"500 ms ±30, s->c 0.4 s", 400 * kMs, false, false, l});
  runOutage({.name = "500 ms ±30, s->c 1 s", .durationNs = kSec, .link = l, .overflows = 1});
  runOutage({.name = "500 ms ±30, s->c 3 s", .durationNs = 3 * kSec, .link = l, .overflows = 1});
}

// --- issue #66: independent loss on top of an outage -------------------------------

TEST_CASE("outages with 2 to 10% loss on top: the player and the spectator recover",
          "[netcode][outage][outage-loss]") {
  runOutage({"80 ms ±5, 2%, s->c 3 s", 3 * kSec, false, false, {40 * kMs, 5 * kMs, 20'000}});
  runOutage({"80 ms ±5, 10%, s->c 1 s", kSec, false, false, {40 * kMs, 5 * kMs, 100'000}});
  runOutage({"200 ms ±30, 5%, s->c 1 s", kSec, false, false, {100 * kMs, 30 * kMs, 50'000}});
  runOutage({"300 ms ±10, 10%, s->c 1 s", kSec, false, false, {150 * kMs, 10 * kMs, 100'000}});
  runOutage({"500 ms ±30, 5%, s->c 0.4 s", 400 * kMs, false, false, {250 * kMs, 30 * kMs, 50'000}});
}

// --- issue #66: the ResyncRequest or the Snapshot held on the reliable lane --------
//
// Over 80 ms ± 5 ms the client sends its request about 200 ms after the
// outage ends (one hop, then gapResyncNs) and the server serves it one hop
// later; the holds below start at the outage's end and are sized around
// resyncRetryNs (1 s after the request) and minResyncIntervalNs (250 ms).

TEST_CASE("a ResyncRequest held 0.5 s: one late repair, no retry",
          "[netcode][outage][outage-hold]") {
  // Released 0.3 s after it was sent, inside the 1 s retry.
  runOutage(
      {"request held 0.5 s", kSec, false, false, {}, 0x0A7A, {true, 500 * kMs}, Expect{1, 1, 1}});
}

TEST_CASE("a ResyncRequest held past the retry: the server's limit drops the retry",
          "[netcode][outage][outage-hold]") {
  // The request (at ≈ 0.2 s) and its retry (≈ 1.2 s) are both held to 1.4 s
  // and reach the server together: the first is served, the second falls
  // inside minResyncIntervalNs and is dropped, harmlessly, since the snapshot
  // answering the first is already on its way.
  for (const uint32_t loss : {0U, 100'000U}) {
    const std::string name = "request held 1.4 s, " + std::to_string(loss / 10'000) + "% loss";
    runOutage({name.c_str(),
               kSec,
               false,
               false,
               {40 * kMs, 5 * kMs, loss},
               0x0A7A,
               {true, 1'400 * kMs},
               Expect{2, 1, 2}});
  }
}

TEST_CASE("a Snapshot's first attempt held 0.3 s: one late repair, no retry",
          "[netcode][outage][outage-hold]") {
  runOutage(
      {"snapshot held 0.3 s", kSec, false, false, {}, 0x0A7A, {false, 300 * kMs}, Expect{1, 1, 1}});
}

TEST_CASE("a Snapshot held past the retry: the retry is served and both snapshots load",
          "[netcode][outage][outage-hold]") {
  // The snapshot (served at ≈ 0.25 s) is held to 1.5 s; the client asks
  // again at ≈ 1.2 s, a second after its request, and the server, a second
  // past its last answer, sends a second snapshot, held behind the first.
  for (const uint32_t loss : {0U, 100'000U}) {
    const std::string name = "snapshot held 1.5 s, " + std::to_string(loss / 10'000) + "% loss";
    runOutage({name.c_str(),
               kSec,
               false,
               false,
               {40 * kMs, 5 * kMs, loss},
               0x0A7A,
               {false, 1'500 * kMs},
               Expect{2, 2, 2}});
  }
}

// --- issue #66: two outages in quick succession ------------------------------------

TEST_CASE("a second outage before the first repair completes needs a second snapshot",
          "[netcode][outage][outage-twice]") {
  runTwoOutages({"twice, before the repair, 80 ms", kSec, 400 * kMs, false});
  runTwoOutages({"twice, before the repair, 500 ms", kSec, kSec, false, {250 * kMs, 30 * kMs}});
}

TEST_CASE("a second outage just after the first repair completes needs a second snapshot",
          "[netcode][outage][outage-twice]") {
  runTwoOutages({"twice, after the repair, 80 ms", kSec, 400 * kMs, true});
  // At 500 ms one of the two repairs is also a rollback-overflow episode.
  runTwoOutages(
      {"twice, after the repair, 500 ms", kSec, kSec, true, {250 * kMs, 30 * kMs}, 0x0A7C, 1});
}
