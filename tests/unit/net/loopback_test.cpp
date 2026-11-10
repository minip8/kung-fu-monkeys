#include "kfm/net/loopback.hpp"

#include "transport_contract.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

using namespace kfm;
using namespace kfm::net;

namespace {

constexpr uint64_t kMs = 1'000'000;

// The loopback under the contract: simulated time, 1 ms per step.
class LoopbackHarness final : public contract::Harness {
 public:
  explicit LoopbackHarness(const LinkConditions& conditions = {})
      : conditions_(conditions),
        net_(42),
        client_(&net_.connectClient(0, conditions, conditions)) {}

  ITransport& server() override { return net_.server(); }
  ITransport& client() override { return *client_; }
  ITransport* addClient() override { return &net_.connectClient(now_, conditions_, conditions_); }
  uint64_t nowNs() override { return now_; }
  void advance() override { now_ += kMs; }
  [[nodiscard]] uint64_t timeoutNs() const override { return 5'000 * kMs; }

 private:
  LinkConditions conditions_;
  LoopbackNetwork net_;
  LoopbackTransport* client_;
  uint64_t now_{0};
};

// Lossless but slow and jittery: every contract case must still hold (the
// unreliable lane reorders, the reliable lane must hide it).
LinkConditions jitteryLink() {
  LinkConditions c = LinkConditions::uniform(30 * kMs, 20 * kMs);
  c.reliable.lossPpm = 200'000;  // retransmits, invisible to the receiver
  c.reliable.retransmitNs = 60 * kMs;
  return c;
}

}  // namespace

KFM_TRANSPORT_CONTRACT_TESTS("Loopback", "[net][loopback][contract]",
                             [] { return std::make_unique<LoopbackHarness>(); })
KFM_TRANSPORT_CONTRACT_TESTS("Loopback jittery", "[net][loopback][contract]",
                             [] { return std::make_unique<LoopbackHarness>(jitteryLink()); })

// --- emulator behaviour ----------------------------------------------------------

namespace {

struct Arrival {
  uint32_t seq;
  uint64_t sentNs;
  uint64_t receivedNs;
};

// Sends `count` unreliable messages client → server, one per ms, and records
// what arrives. The payload carries its sequence number.
std::vector<Arrival> runUnreliable(LoopbackNetwork& net, LoopbackTransport& client, uint32_t count,
                                   uint64_t drainNs = 2'000 * kMs) {
  LoopbackTransport& server = net.server();
  std::vector<Arrival> out;
  std::vector<uint64_t> sentAt(count);
  uint64_t now = client.nowNs();
  const uint64_t end = now + static_cast<uint64_t>(count) * kMs + drainNs;
  uint32_t next = 0;
  while (now <= end) {
    ConnectionEvents ev;
    client.poll(now, ev);
    server.poll(now, ev);
    Packet p;
    while (server.recv(p)) {
      const uint32_t seq = contract::seqOf(p.data);
      out.push_back(Arrival{seq, sentAt.at(seq), p.receivedNs});
    }
    if (next < count && client.isConnected(kServerPeer)) {
      sentAt[next] = now;
      client.sendUnreliable(kServerPeer, contract::payload(next, 32));
      ++next;
    }
    now += kMs;
  }
  REQUIRE(next == count);
  return out;
}

LoopbackTransport& connected(LoopbackNetwork& net, const LinkConditions& up,
                             const LinkConditions& down = {}) {
  LoopbackTransport& c = net.connectClient(0, up, down);
  ConnectionEvents ev;
  for (uint64_t t = 0; !c.isConnected(kServerPeer) && t < 1'000 * kMs; t += kMs) {
    c.poll(t, ev);
    net.server().poll(t, ev);
  }
  REQUIRE(c.isConnected(kServerPeer));
  return c;
}

}  // namespace

TEST_CASE("Emulator latency is exact without jitter", "[net][loopback][emulator]") {
  LoopbackNetwork net(1);
  LoopbackTransport& c = connected(net, LinkConditions::uniform(45 * kMs));
  const auto got = runUnreliable(net, c, 200);
  REQUIRE(got.size() == 200);
  for (size_t i = 0; i < got.size(); ++i) {
    CHECK(got[i].seq == i);
    CHECK(got[i].receivedNs - got[i].sentNs == 45 * kMs);
  }
}

TEST_CASE("Emulator jitter stays in range and reorders", "[net][loopback][emulator]") {
  LoopbackNetwork net(2);
  LoopbackTransport& c = connected(net, LinkConditions::uniform(50 * kMs, 20 * kMs));
  const auto got = runUnreliable(net, c, 4000);
  REQUIRE(got.size() == 4000);
  uint64_t sum = 0;
  uint32_t inversions = 0;
  for (size_t i = 0; i < got.size(); ++i) {
    const uint64_t d = got[i].receivedNs - got[i].sentNs;
    // Delivery is observed at 1 ms poll granularity, the stamp is exact.
    CHECK(d >= 50 * kMs);
    CHECK(d <= 70 * kMs);
    sum += d;
    if (i > 0 && got[i].seq < got[i - 1].seq) ++inversions;
  }
  const uint64_t mean = sum / got.size();
  CHECK(mean > 59 * kMs);
  CHECK(mean < 61 * kMs);
  CHECK(inversions > 100);  // 20 ms of jitter on 1 ms spacing reorders a lot
}

TEST_CASE("Emulator loss rate matches its setting", "[net][loopback][emulator]") {
  LoopbackNetwork net(3);
  LinkConditions up;
  up.unreliable.lossPpm = 100'000;  // 10 %
  LoopbackTransport& c = connected(net, up);
  const auto got = runUnreliable(net, c, 10'000);
  // Binomial(10000, 0.9): sd = 30; allow ~6 sd.
  CHECK(got.size() > 8'820);
  CHECK(got.size() < 9'180);
  const EmulatorStats st = net.emulatorStats(c.self(), true);
  CHECK(st.unreliableSent == 10'000);
  CHECK(st.unreliableLost == 10'000 - got.size());
  CHECK(c.stats().packetsSent == 10'000);  // the sender cannot tell
  CHECK(net.server().stats().packetsReceived == got.size());
}

TEST_CASE("Emulator duplicates and holds back packets", "[net][loopback][emulator]") {
  LoopbackNetwork net(4);
  LinkConditions up = LinkConditions::uniform(10 * kMs);
  up.unreliable.duplicatePpm = 50'000;  // 5 %
  up.unreliable.reorderPpm = 100'000;   // 10 %, held back by 25 ms
  up.unreliable.reorderDelayNs = 25 * kMs;
  LoopbackTransport& c = connected(net, up);
  const auto got = runUnreliable(net, c, 10'000);
  const size_t dups = got.size() - 10'000;
  CHECK(dups > 400);  // Binomial(10000, 0.05): sd = 22
  CHECK(dups < 600);
  uint32_t late = 0;
  uint32_t overtaken = 0;
  for (size_t i = 0; i < got.size(); ++i) {
    const uint64_t d = got[i].receivedNs - got[i].sentNs;
    CHECK((d == 10 * kMs || d == 35 * kMs));
    late += d == 35 * kMs ? 1 : 0;
    if (i > 0 && got[i].seq < got[i - 1].seq) ++overtaken;
  }
  // 10 % of 10'500 copies, sd ≈ 31.
  CHECK(late > 850);
  CHECK(late < 1'250);
  CHECK(overtaken > 0);
  const EmulatorStats st = net.emulatorStats(c.self(), true);
  CHECK(st.unreliableDuplicated == dups);
  CHECK(st.unreliableHeldBack == late);
}

TEST_CASE("Emulator reliable lane hides loss behind retransmission", "[net][loopback][emulator]") {
  LoopbackNetwork net(5);
  LinkConditions up = LinkConditions::uniform(20 * kMs, 10 * kMs, 300'000);  // 30 % loss
  up.reliable.retransmitNs = 50 * kMs;
  LoopbackTransport& c = connected(net, up, up);
  LoopbackTransport& server = net.server();

  constexpr uint32_t kCount = 2'000;
  std::vector<uint32_t> seqs;
  uint64_t now = c.nowNs();
  uint32_t next = 0;
  uint64_t maxDelay = 0;
  std::vector<uint64_t> sentAt(kCount);
  while (seqs.size() < kCount && now < 60'000 * kMs) {
    ConnectionEvents ev;
    c.poll(now, ev);
    server.poll(now, ev);
    Packet p;
    while (server.recv(p)) {
      CHECK(p.lane == Lane::Reliable);
      const uint32_t s = contract::seqOf(p.data);
      seqs.push_back(s);
      maxDelay = std::max(maxDelay, p.receivedNs - sentAt.at(s));
    }
    if (next < kCount) {
      sentAt[next] = now;
      c.sendReliable(kServerPeer, contract::payload(next, 64));
      ++next;
    }
    now += kMs;
  }
  REQUIRE(seqs.size() == kCount);
  for (uint32_t i = 0; i < kCount; ++i)
    CHECK(seqs[i] == i);
  const EmulatorStats st = net.emulatorStats(c.self(), true);
  CHECK(st.reliableSent == kCount);
  // Geometric: 0.3 / 0.7 retransmits per message on average, ~857.
  CHECK(st.reliableRetransmits > 750);
  CHECK(st.reliableRetransmits < 970);
  CHECK(maxDelay >= 70 * kMs);  // at least one message waited for a retransmit
}

TEST_CASE("Emulator runs are a pure function of the seed", "[net][loopback][emulator]") {
  const auto trace = [](uint64_t seed) {
    LoopbackNetwork net(seed);
    LinkConditions up = LinkConditions::uniform(30 * kMs, 15 * kMs, 50'000);
    up.unreliable.duplicatePpm = 20'000;
    up.unreliable.reorderPpm = 20'000;
    up.unreliable.reorderDelayNs = 40 * kMs;
    LoopbackTransport& c = connected(net, up);
    std::vector<uint64_t> out;
    for (const Arrival& a : runUnreliable(net, c, 1'000)) {
      out.push_back(a.seq);
      out.push_back(a.receivedNs);
    }
    return out;
  };
  CHECK(trace(7) == trace(7));
  CHECK(trace(7) != trace(8));
}

TEST_CASE("Emulator links are independent", "[net][loopback][emulator]") {
  // The draws of one link do not depend on what another link does.
  const auto arrivalsOfB = [](bool aIsLossy) {
    LoopbackNetwork net(9);
    const LinkConditions lossy = LinkConditions::uniform(5 * kMs, 5 * kMs, 500'000);
    LoopbackTransport& a = connected(net, aIsLossy ? lossy : LinkConditions{});
    LoopbackTransport& b = connected(net, LinkConditions::uniform(10 * kMs, 10 * kMs, 100'000));
    std::vector<uint64_t> out;
    uint64_t now = b.nowNs();
    LoopbackTransport& server = net.server();
    for (uint32_t i = 0; i < 2'000; ++i, now += kMs) {
      ConnectionEvents ev;
      a.poll(now, ev);
      b.poll(now, ev);
      server.poll(now, ev);
      Packet p;
      while (server.recv(p)) {
        if (p.peer == b.self()) out.push_back(p.receivedNs * 10'000 + contract::seqOf(p.data));
      }
      if (i < 1'000) {
        a.sendUnreliable(kServerPeer, contract::payload(i, 16));
        b.sendUnreliable(kServerPeer, contract::payload(i, 16));
      }
    }
    return out;
  };
  CHECK(arrivalsOfB(false) == arrivalsOfB(true));
}

TEST_CASE("Loopback connection timing, cut and capacity", "[net][loopback]") {
  LoopbackNetwork net(11);
  LoopbackTransport& c =
      net.connectClient(0, LinkConditions::uniform(30 * kMs), LinkConditions::uniform(10 * kMs));
  LoopbackTransport& s = net.server();
  CHECK(c.self() == PeerId{1});
  CHECK(net.client(PeerId{1}) == &c);
  CHECK(net.client(PeerId{2}) == nullptr);

  ConnectionEvents ce;
  ConnectionEvents se;
  c.poll(9 * kMs, ce);
  s.poll(9 * kMs, se);
  CHECK(ce.count == 0);
  CHECK(se.count == 0);
  c.poll(10 * kMs, ce);  // server-to-client latency
  REQUIRE(ce.count == 1);
  CHECK(ce.items[0].timeNs == 10 * kMs);
  s.poll(29 * kMs, se);
  CHECK(se.count == 0);
  s.poll(30 * kMs, se);  // client-to-server latency
  REQUIRE(se.count == 1);
  CHECK(se.items[0].peer == PeerId{1});

  // In-flight messages die with a cut link.
  c.sendReliable(kServerPeer, contract::payload(1, 8));
  net.cut(c.self());
  ce.clear();
  se.clear();
  c.poll(200 * kMs, ce);
  s.poll(200 * kMs, se);
  REQUIRE(ce.count == 1);
  CHECK(ce.items[0].kind == ConnectionEventKind::Disconnected);
  REQUIRE(se.count == 1);
  CHECK(se.items[0].kind == ConnectionEventKind::Disconnected);
  Packet p;
  CHECK_FALSE(s.recv(p));

  // kMaxPeers open links at most; a closed one frees its place.
  std::vector<LoopbackTransport*> clients;
  clients.reserve(kMaxPeers);
  for (uint16_t i = 0; i < kMaxPeers; ++i)
    clients.push_back(&net.connectClient(300 * kMs));
  LoopbackTransport& extra = net.connectClient(300 * kMs);
  ce.clear();
  extra.poll(300 * kMs, ce);
  REQUIRE(ce.count == 1);
  CHECK(ce.items[0].kind == ConnectionEventKind::Disconnected);
  CHECK_FALSE(extra.isConnected(kServerPeer));
  net.cut(clients[0]->self());
  LoopbackTransport& again = net.connectClient(300 * kMs);
  ce.clear();
  again.poll(300 * kMs, ce);
  REQUIRE(ce.count == 1);
  CHECK(ce.items[0].kind == ConnectionEventKind::Connected);
}

TEST_CASE("Loopback receive queue is bounded", "[net][loopback]") {
  LoopbackNetwork net(12);
  LoopbackTransport& c = connected(net, {});
  for (size_t i = 0; i < LoopbackTransport::kRecvQueueCapacity + 10; ++i)
    c.sendUnreliable(kServerPeer, contract::payload(static_cast<uint32_t>(i), 4));
  ConnectionEvents ev;
  net.server().poll(c.nowNs() + kMs, ev);
  CHECK(net.server().stats().recvDropped == 10);
  CHECK(net.server().stats().packetsReceived == LoopbackTransport::kRecvQueueCapacity);
}

TEST_CASE("Emulator outage drops one direction for a span without disconnecting",
          "[net][loopback][emulator]") {
  LoopbackNetwork net(13);
  const LinkConditions jittery = LinkConditions::uniform(10 * kMs, 4 * kMs);
  LoopbackTransport& c = connected(net, jittery, jittery);
  const uint64_t t0 = c.nowNs();
  // Unreliable client → server messages sent in [t0 + 100, t0 + 400) ms are lost.
  net.setOutage(c.self(), true, Outage{t0 + 100 * kMs, t0 + 400 * kMs, false});
  const auto got = runUnreliable(net, c, 1'000);
  CHECK(got.size() == 700);
  for (const Arrival& a : got) {
    CHECK((a.sentNs < t0 + 100 * kMs || a.sentNs >= t0 + 400 * kMs));
  }
  CHECK(c.isConnected(kServerPeer));
  const EmulatorStats st = net.emulatorStats(c.self(), true);
  CHECK(st.unreliableBlackedOut == 300);
  CHECK(st.unreliableLost == 300);

  // The draws are the same as without the outage: the messages after it
  // arrive exactly when they would have.
  LoopbackNetwork plain(13);
  LoopbackTransport& d = connected(plain, jittery, jittery);
  const auto all = runUnreliable(plain, d, 1'000);
  std::vector<uint64_t> kept;
  for (const Arrival& a : all)
    if (a.sentNs < t0 + 100 * kMs || a.sentNs >= t0 + 400 * kMs)
      kept.push_back(a.receivedNs * 10'000 + a.seq);
  std::vector<uint64_t> seen;
  seen.reserve(got.size());
  for (const Arrival& a : got)
    seen.push_back(a.receivedNs * 10'000 + a.seq);
  CHECK(seen == kept);
}

TEST_CASE("Emulator outage of the reliable lane delays it in order", "[net][loopback][emulator]") {
  LoopbackNetwork net(14);
  LoopbackTransport& c = connected(net, LinkConditions::uniform(20 * kMs));
  LoopbackTransport& server = net.server();
  const uint64_t t0 = c.nowNs();
  net.setOutage(c.self(), true, Outage{t0, t0 + 500 * kMs, true});
  std::vector<std::pair<uint32_t, uint64_t>> seen;
  for (uint64_t now = t0; now < t0 + 1'000 * kMs; now += kMs) {
    ConnectionEvents ev;
    c.poll(now, ev);
    server.poll(now, ev);
    Packet p;
    while (server.recv(p))
      seen.emplace_back(contract::seqOf(p.data), p.receivedNs);
    const auto i = static_cast<uint32_t>((now - t0) / kMs);
    if (i < 600) c.sendReliable(kServerPeer, contract::payload(i, 8));
  }
  REQUIRE(seen.size() == 600);
  for (uint32_t i = 0; i < 600; ++i) {
    CHECK(seen[i].first == i);
    // Held until the outage ends plus the latency, then on time again.
    CHECK(seen[i].second == std::max(t0 + 520 * kMs, t0 + i * kMs + 20 * kMs));
  }
  CHECK(net.emulatorStats(c.self(), true).reliableHeld == 500);
  CHECK(c.isConnected(kServerPeer));
}
