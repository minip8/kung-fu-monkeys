// Stalled and hidden clients over the loopback emulator (architecture §6.6,
// v0.92): a player whose host stops running for a while (a main-thread stall,
// a hidden browser tab whose requestAnimationFrame stopped) must come back to
// a live session in agreement with the server, and a player whose link really
// is lost keeps its slot and its score for the grace window.
#include "kfm/net/netsim.hpp"
#include "kfm/sim/step.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <bit>
#include <cstdint>
#include <optional>

using namespace kfm;
using namespace kfm::net;
using namespace kfm::net::netsim;

namespace {

struct Mark {
  uint64_t compared{0};
  uint64_t mismatches{0};
};

Mark mark(const ClientRig& r) {
  return {r.client->netStats().checksumsCompared, r.client->netStats().checksumMismatches};
}

// Back in agreement: running, every authoritative frame held to within a few
// ticks of the server's newest, a checksum compared since `m` and none
// mismatched, and predicting ahead of the server again.
bool agrees(const NetSim& s, const ClientRig& r, const Mark& m) {
  const NetClient& c = *r.client;
  const std::optional<Tick> confirmed = c.confirmedTick();
  if (!c.running() || c.holding() || !confirmed) return false;
  if (s.server().nextTick().minus(*confirmed) > 20) return false;
  if (c.netStats().checksumsCompared <= m.compared) return false;
  if (c.netStats().checksumMismatches != m.mismatches) return false;
  return c.spectator() || c.localTick() >= s.server().nextTick();
}

}  // namespace

// A main-thread stall: the client is not run at all (no pump, no ticks) and
// then resumes; its link stays up the whole time.
TEST_CASE("a client that stops running for a few seconds comes back in agreement",
          "[netcode][stall]") {
  const uint64_t stall =
      GENERATE(uint64_t{1} * kSec, uint64_t{3} * kSec, uint64_t{6} * kSec, uint64_t{9} * kSec);
  ServerConfig sc;
  sc.match.seed = 0x5A11;
  NetSim s(sc, 23);
  const LinkConditions l = link(40 * kMs, 5 * kMs);
  ClientRig& a = s.addClient(l, l, tokenConfig("stalls"));
  ClientRig& b = s.addClient(l, l, tokenConfig("steady"));
  REQUIRE(s.runUntil([&] { return s.allRunning(); }, 3 * kSec));
  s.runFor(4 * kSec);
  const sim::PlayerMask slots = a.client->localSlots();
  const uint64_t joins = s.server().stats().joins;

  a.active = false;
  s.runFor(stall);
  a.active = true;
  const Mark m = mark(a);
  INFO("stall " << stall / kMs << " ms");
  REQUIRE(s.runUntil([&] { return agrees(s, a, m); }, 2 * kSec));
  CHECK(a.client->localSlots() == slots);
  CHECK(s.server().ownerOf(PlayerId{0}) == a.transport->self());
  CHECK(s.server().stats().joins == joins);
  CHECK(s.server().stats().reconnects == 0);
  CHECK(b.client->running());
}

// A hidden browser tab (§6.6 "A hidden client"): requestAnimationFrame
// stops, a timer still wakes the page a few times a second (or, throttled,
// once), and the client goes to the background: it pumps, pings and handles
// control messages but runs no ticks. The server hears its pings (no
// timeout) and stands its monkey still; on return one snapshot brings it
// back, whatever the time away.
TEST_CASE("a client in the background stays in the room and resyncs on return",
          "[netcode][stall][background]") {
  const uint64_t away = GENERATE(uint64_t{30} * kSec, uint64_t{120} * kSec, uint64_t{600} * kSec);
  const uint64_t pumpEvery = GENERATE(uint64_t{50} * kMs, uint64_t{1} * kSec);
  ServerConfig sc;
  sc.match.seed = 0xB6;
  NetSim s(sc, 29);
  const LinkConditions l = link(40 * kMs, 5 * kMs, 10'000);
  ClientRig& a = s.addClient(l, l, tokenConfig("hides"));
  ClientRig& b = s.addClient(l, l, tokenConfig("plays"));
  REQUIRE(s.runUntil([&] { return s.allRunning(); }, 3 * kSec));
  s.runFor(4 * kSec);
  const sim::PlayerMask slots = a.client->localSlots();
  const PlayerId slot{static_cast<PlayerId::rep>(std::countr_zero(slots))};
  const PeerId peer = a.transport->self();
  INFO("away " << away / kSec << " s, pumped every " << pumpEvery / kMs << " ms");

  a.client->setBackground(true);
  a.frameEveryNs = pumpEvery;
  const uint64_t idledBefore = s.server().stats().idledInputs;
  s.runFor(away);
  // Still the same peer, never timed out; its slot stood still after
  // idleAfterTicks and nothing ran on the client meanwhile.
  CHECK(s.server().ownerOf(slot) == peer);
  CHECK(s.server().stats().timeouts == 0);
  CHECK(a.client->running());
  CHECK(a.client->holding());
  const uint64_t awayTicks = away * sim::kTickHz / kSec;
  CHECK(s.server().stats().idledInputs - idledBefore >= awayTicks - sc.idleAfterTicks - 60);
  if (const sim::InputFrame* f = s.server().frames().get(s.server().nextTick().plus(-1))) {
    CHECK(f->inputs[slot.value].moveMag == 0);
    CHECK(f->inputs[slot.value].buttons == 0);
  }

  a.client->setBackground(false);
  a.frameEveryNs = 0;
  const Mark m = mark(a);
  const uint64_t ticksBefore = a.client->netStats().ticks;
  REQUIRE(s.runUntil([&] { return agrees(s, a, m); }, 2 * kSec));
  CHECK(a.client->netStats().wakes == 1);
  CHECK(a.client->localSlots() == slots);
  CHECK(s.server().stats().reconnects == 0);
  // One snapshot and a run forward of about a lead, never the time away.
  CHECK(a.client->netStats().ticks - ticksBefore < 2 * sim::kTickHz);
  CHECK(a.client->netStats().runForwardClamped == 0);
  s.runFor(3 * kSec);
  CHECK(a.client->netStats().checksumMismatches == m.mismatches);
  CHECK(b.client->running());
}

// A link that really is lost: past the server's 15 s the peer is dropped and
// its slot (with its monkey and its score) is reserved for the 60 s grace; a
// reconnect inside it lands in the same slot, one after it joins afresh.
TEST_CASE("a dropped player keeps its slot and score for the grace window",
          "[netcode][stall][reconnect]") {
  const bool inside = GENERATE(true, false);
  ServerConfig sc;
  sc.match.seed = 0x6A;
  NetSim s(sc, 31);
  const LinkConditions l = link(40 * kMs, 5 * kMs);
  ClientRig& a = s.addClient(l, l, tokenConfig("drops"));
  ClientRig& b = s.addClient(l, l, tokenConfig("stays"));
  REQUIRE(s.runUntil([&] { return s.allRunning(); }, 3 * kSec));
  s.runFor(4 * kSec);
  const int8_t score = s.server().world().rules.scores[0];

  // The host stops altogether (a frozen page, a laptop lid): nothing is sent.
  a.active = false;
  s.runFor(14 * kSec);
  CHECK(s.server().stats().timeouts == 0);  // 15 s: not yet
  s.runFor(2 * kSec);
  CHECK(s.server().stats().timeouts == 1);
  CHECK(sim::maskHas(s.server().present(), PlayerId{0}));  // reserved
  a.active = true;
  // Back: the server closed the link and said why.
  REQUIRE(s.runUntil([&] { return a.client->state() == ClientState::Disconnected; }, kSec));
  CHECK(a.client->disconnectCause() == DisconnectCause::ServerDisconnect);
  CHECK(a.client->disconnectReason() == DisconnectReason::Timeout);

  if (!inside) {
    a.active = false;
    s.runFor(uint64_t{sc.reconnectGraceTicks} * kSec / sim::kTickHz);
    a.active = true;
    CHECK(!sim::maskHas(s.server().present(), PlayerId{0}));
  } else {
    // Present the whole time: its monkey and its score stay in the match.
    for (int i = 0; i < 50; ++i) {
      s.runFor(kSec);
      REQUIRE(sim::maskHas(s.server().present(), PlayerId{0}));
    }
    CHECK(s.server().world().rules.scores[0] >= score);
  }
  s.reconnect(a);
  REQUIRE(s.runUntil([&] { return a.client->running(); }, 3 * kSec));
  CHECK(s.server().stats().reconnects == (inside ? 1u : 0u));
  CHECK(a.client->localSlots() == 0b001);  // the lowest free slot either way here
  CHECK(b.client->running());
}

// The client gives up first: 10 s without a packet from the server.
TEST_CASE("a client hearing nothing for 10 s ends the session as silent", "[netcode][stall]") {
  NetSim s(ServerConfig{}, 37);
  const LinkConditions l = link(20 * kMs);
  ClientRig& a = s.addClient(l, l, tokenConfig("deaf"));
  REQUIRE(s.runUntil([&] { return s.allRunning(); }, 3 * kSec));
  s.network().setOutage(a.transport->self(), false,
                        Outage{.fromNs = s.now(), .untilNs = s.now() + 60 * kSec});
  s.runFor(9 * kSec);
  CHECK(a.client->running());
  CHECK(a.client->silentNs(s.now()) > 8 * kSec);
  REQUIRE(s.runUntil([&] { return a.client->state() == ClientState::Disconnected; }, 2 * kSec));
  CHECK(a.client->disconnectCause() == DisconnectCause::ServerSilent);
  CHECK(s.server().stats().timeouts == 0);  // the server still hears it
}
