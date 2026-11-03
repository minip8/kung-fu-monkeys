// The transport contract test suite (architecture §6.4): every ITransport
// implementation must pass it. It assumes nothing about the transport beyond
// transport.hpp, so it runs unchanged over the loopback (simulated time) and
// over a real network transport (wall-clock time and timeouts).
//
// To instantiate it for a transport, implement contract::Harness and expand
// the macro once in a test source file:
//
//   struct MyHarness final : kfm::net::contract::Harness { ... };
//   KFM_TRANSPORT_CONTRACT_TESTS("MyTransport", "[net][mytransport]",
//                                [] { return std::make_unique<MyHarness>(); })
//
// Each generated TEST_CASE builds a fresh harness from the factory: one
// server endpoint and one client endpoint whose connection attempt has
// started (neither side has necessarily seen Connected yet). The suite drives
// time only through Harness::advance() and nowNs() and gives up after
// timeoutNs() of the harness clock.
#pragma once

#include "kfm/net/transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace kfm::net::contract {

class Harness {
 public:
  Harness() = default;
  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;
  Harness(Harness&&) = delete;
  Harness& operator=(Harness&&) = delete;
  virtual ~Harness() = default;

  [[nodiscard]] virtual ITransport& server() = 0;
  [[nodiscard]] virtual ITransport& client() = 0;
  // Starts another client connecting to the same server and returns it
  // (owned by the harness), or nullptr when the harness cannot; the
  // multi-client cases then pass trivially.
  [[nodiscard]] virtual ITransport* addClient() { return nullptr; }
  // The clock handed to poll(): monotonic nanoseconds.
  [[nodiscard]] virtual uint64_t nowNs() = 0;
  // Lets a little time pass (the loopback adds a millisecond to its clock; a
  // real transport sleeps briefly or runs its event loop once).
  virtual void advance() = 0;
  // How long anything (connecting, a delivery) may take on that clock.
  [[nodiscard]] virtual uint64_t timeoutNs() const = 0;
};

using HarnessFactory = std::function<std::unique_ptr<Harness>()>;

struct Message {
  PeerId peer{};
  Lane lane{Lane::Unreliable};
  uint64_t receivedNs{0};
  std::vector<std::byte> data;
};

struct Inbox {
  std::vector<ConnectionEvent> events;
  std::vector<Message> messages;

  [[nodiscard]] size_t countEvents(ConnectionEventKind kind) const {
    size_t n = 0;
    for (const ConnectionEvent& e : events)
      n += e.kind == kind ? 1 : 0;
    return n;
  }
  [[nodiscard]] const ConnectionEvent* firstEvent(ConnectionEventKind kind) const {
    for (const ConnectionEvent& e : events) {
      if (e.kind == kind) return &e;
    }
    return nullptr;
  }
};

// A message of `size` bytes (at least 4) whose content identifies `seq`.
[[nodiscard]] inline std::vector<std::byte> payload(uint32_t seq, size_t size) {
  std::vector<std::byte> v(size < 4 ? 4 : size);
  for (size_t i = 0; i < 4; ++i)
    v[i] = static_cast<std::byte>((seq >> (8 * i)) & 0xFFu);
  for (size_t i = 4; i < v.size(); ++i)
    v[i] = static_cast<std::byte>((seq * 31u + static_cast<uint32_t>(i) * 7u) & 0xFFu);
  return v;
}

[[nodiscard]] inline uint32_t seqOf(std::span<const std::byte> data) {
  if (data.size() < 4) return 0xFFFF'FFFFu;
  uint32_t s = 0;
  for (size_t i = 0; i < 4; ++i)
    s |= static_cast<uint32_t>(data[i]) << (8 * i);
  return s;
}

[[nodiscard]] inline bool isPayload(std::span<const std::byte> data, uint32_t seq, size_t size) {
  const std::vector<std::byte> want = payload(seq, size);
  return data.size() == want.size() && std::equal(data.begin(), data.end(), want.begin());
}

// Polls every tracked endpoint and drains its messages into an inbox,
// checking the per-packet rules of transport.hpp on the way.
class Pump {
 public:
  explicit Pump(Harness& h) : h_(h) {
    track(h.server());
    track(h.client());
  }

  void track(ITransport& t) { endpoints_.push_back(Endpoint{&t, {}}); }

  [[nodiscard]] Inbox& inbox(const ITransport& t) {
    for (Endpoint& e : endpoints_) {
      if (e.transport == &t) return e.inbox;
    }
    FAIL("endpoint not tracked");
    return endpoints_.front().inbox;
  }

  void step() {
    h_.advance();
    const uint64_t now = h_.nowNs();
    for (Endpoint& e : endpoints_) {
      ConnectionEvents ev;
      e.transport->poll(now, ev);
      CHECK(ev.dropped == 0);
      for (const ConnectionEvent& c : ev.span()) {
        CHECK(c.peer.isValid());
        e.inbox.events.push_back(c);
      }
      Packet p;
      while (e.transport->recv(p)) {
        CHECK(p.peer.isValid());
        CHECK((p.lane == Lane::Unreliable || p.lane == Lane::Reliable));
        CHECK(p.receivedNs <= now);
        e.inbox.messages.push_back(
            Message{p.peer, p.lane, p.receivedNs, {p.data.begin(), p.data.end()}});
      }
    }
  }

  // Steps until pred() holds or the harness timeout passes; returns pred().
  template <class Pred>
  bool until(Pred&& pred) {
    const uint64_t start = h_.nowNs();
    while (!pred()) {
      if (h_.nowNs() - start > h_.timeoutNs()) return pred();
      step();
    }
    return true;
  }

  // Steps for about `ns` of harness time (a settling period).
  void stepFor(uint64_t ns) {
    const uint64_t start = h_.nowNs();
    do {
      step();
    } while (h_.nowNs() - start < ns);
  }

  [[nodiscard]] Harness& harness() { return h_; }

 private:
  struct Endpoint {
    ITransport* transport;
    Inbox inbox;
  };
  Harness& h_;
  std::vector<Endpoint> endpoints_;
};

// Waits until both sides saw Connected; returns the client's PeerId on the
// server.
[[nodiscard]] inline PeerId connectPair(Pump& pump, const ITransport& server,
                                        const ITransport& client) {
  Inbox& s = pump.inbox(server);
  Inbox& c = pump.inbox(client);
  const size_t before = s.countEvents(ConnectionEventKind::Connected);
  const bool connected = pump.until([&] {
    return s.countEvents(ConnectionEventKind::Connected) > before &&
           c.countEvents(ConnectionEventKind::Connected) > 0;
  });
  REQUIRE(connected);
  const ConnectionEvent& ce = *c.firstEvent(ConnectionEventKind::Connected);
  CHECK(ce.peer == kServerPeer);
  PeerId id = PeerId::invalid();
  size_t seen = 0;
  for (const ConnectionEvent& e : s.events) {
    if (e.kind == ConnectionEventKind::Connected && seen++ == before) id = e.peer;
  }
  REQUIRE(id.isValid());
  CHECK(server.isConnected(id));
  CHECK(client.isConnected(kServerPeer));
  return id;
}

// Sends an unreliable message every step until one copy arrives in `to`.
inline void sendUnreliableUntilReceived(Pump& pump, ITransport& from, PeerId peer,
                                        const ITransport& to, uint32_t seq, size_t size) {
  Inbox& in = pump.inbox(to);
  const std::vector<std::byte> msg = payload(seq, size);
  const bool got = pump.until([&] {
    for (const Message& m : in.messages) {
      if (m.lane == Lane::Unreliable && isPayload(m.data, seq, size)) return true;
    }
    from.sendUnreliable(peer, msg);
    return false;
  });
  REQUIRE(got);
}

// --- the cases ------------------------------------------------------------------

inline void connectsBothSides(const HarnessFactory& make) {
  auto h = make();
  Pump pump(*h);
  const PeerId id = connectPair(pump, h->server(), h->client());
  pump.stepFor(h->timeoutNs() / 50);
  CHECK(pump.inbox(h->server()).countEvents(ConnectionEventKind::Connected) == 1);
  CHECK(pump.inbox(h->client()).countEvents(ConnectionEventKind::Connected) == 1);
  CHECK(pump.inbox(h->server()).countEvents(ConnectionEventKind::Disconnected) == 0);
  CHECK(pump.inbox(h->client()).countEvents(ConnectionEventKind::Disconnected) == 0);
  CHECK(h->server().isConnected(id));
  CHECK_FALSE(h->server().isConnected(PeerId{static_cast<PeerId::rep>(id.value + 100)}));
  CHECK_FALSE(h->client().isConnected(PeerId{static_cast<PeerId::rep>(kServerPeer.value + 1)}));
}

inline void deliversBothLanesBothWays(const HarnessFactory& make) {
  auto h = make();
  Pump pump(*h);
  ITransport& server = h->server();
  ITransport& client = h->client();
  const PeerId id = connectPair(pump, server, client);

  server.sendReliable(id, payload(1, 64));
  client.sendReliable(kServerPeer, payload(2, 64));
  Inbox& s = pump.inbox(server);
  Inbox& c = pump.inbox(client);
  REQUIRE(pump.until([&] { return !s.messages.empty() && !c.messages.empty(); }));
  CHECK(s.messages[0].peer == id);
  CHECK(s.messages[0].lane == Lane::Reliable);
  CHECK(isPayload(s.messages[0].data, 2, 64));
  CHECK(c.messages[0].peer == kServerPeer);
  CHECK(c.messages[0].lane == Lane::Reliable);
  CHECK(isPayload(c.messages[0].data, 1, 64));

  sendUnreliableUntilReceived(pump, server, id, client, 3, 100);
  sendUnreliableUntilReceived(pump, client, kServerPeer, server, 4, 100);
  for (const Message& m : s.messages)
    CHECK(m.peer == id);
  for (const Message& m : c.messages)
    CHECK(m.peer == kServerPeer);
}

inline void reliableIsOrderedAndExactlyOnce(const HarnessFactory& make) {
  auto h = make();
  Pump pump(*h);
  ITransport& server = h->server();
  ITransport& client = h->client();
  const PeerId id = connectPair(pump, server, client);

  constexpr uint32_t kCount = 300;
  const auto sizeOf = [](uint32_t seq) { return size_t{4} + (seq * 37u) % 1500u; };
  for (uint32_t seq = 0; seq < kCount; ++seq) {
    client.sendReliable(kServerPeer, payload(seq, sizeOf(seq)));
    server.sendReliable(id, payload(seq + 1000, sizeOf(seq)));
    if (seq % 16 == 15) pump.step();
  }
  Inbox& s = pump.inbox(server);
  Inbox& c = pump.inbox(client);
  REQUIRE(pump.until([&] { return s.messages.size() >= kCount && c.messages.size() >= kCount; }));
  pump.stepFor(h->timeoutNs() / 50);  // nothing more may arrive
  REQUIRE(s.messages.size() == kCount);
  REQUIRE(c.messages.size() == kCount);
  for (uint32_t seq = 0; seq < kCount; ++seq) {
    CHECK(s.messages[seq].lane == Lane::Reliable);
    CHECK(isPayload(s.messages[seq].data, seq, sizeOf(seq)));
    CHECK(c.messages[seq].lane == Lane::Reliable);
    CHECK(isPayload(c.messages[seq].data, seq + 1000, sizeOf(seq)));
  }
}

inline void reliableCarriesMessagesUpToTheLimit(const HarnessFactory& make) {
  auto h = make();
  Pump pump(*h);
  ITransport& server = h->server();
  ITransport& client = h->client();
  const PeerId id = connectPair(pump, server, client);

  const std::vector<size_t> sizes{kMaxReliableBytes, 4, kMaxReliableBytes / 2 + 3, 5000};
  for (uint32_t i = 0; i < sizes.size(); ++i) {
    server.sendReliable(id, payload(i, sizes[i]));
    client.sendReliable(kServerPeer, payload(i + 10, sizes[i]));
  }
  // Over the limit: dropped at the sender and counted, never delivered.
  const TransportStats sBefore = server.stats();
  const TransportStats cBefore = client.stats();
  server.sendReliable(id, payload(99, kMaxReliableBytes + 1));
  client.sendReliable(kServerPeer, payload(99, kMaxReliableBytes + 1));
  CHECK(server.stats().sendErrors == sBefore.sendErrors + 1);
  CHECK(client.stats().sendErrors == cBefore.sendErrors + 1);
  CHECK(server.stats().packetsSent == sBefore.packetsSent);
  CHECK(client.stats().packetsSent == cBefore.packetsSent);
  server.sendReliable(id, payload(50, 8));  // marker behind the oversize one
  client.sendReliable(kServerPeer, payload(60, 8));

  Inbox& s = pump.inbox(server);
  Inbox& c = pump.inbox(client);
  REQUIRE(pump.until(
      [&] { return s.messages.size() > sizes.size() && c.messages.size() > sizes.size(); }));
  for (uint32_t i = 0; i < sizes.size(); ++i) {
    CHECK(isPayload(c.messages[i].data, i, sizes[i]));
    CHECK(isPayload(s.messages[i].data, i + 10, sizes[i]));
  }
  CHECK(isPayload(c.messages[sizes.size()].data, 50, 8));
  CHECK(isPayload(s.messages[sizes.size()].data, 60, 8));
}

inline void unreliableRespectsItsLimit(const HarnessFactory& make) {
  auto h = make();
  Pump pump(*h);
  ITransport& server = h->server();
  ITransport& client = h->client();
  const PeerId id = connectPair(pump, server, client);

  sendUnreliableUntilReceived(pump, client, kServerPeer, server, 7, kMaxUnreliableBytes);
  sendUnreliableUntilReceived(pump, server, id, client, 8, kMaxUnreliableBytes);

  const TransportStats sBefore = server.stats();
  const TransportStats cBefore = client.stats();
  client.sendUnreliable(kServerPeer, payload(9, kMaxUnreliableBytes + 1));
  server.sendUnreliable(id, payload(9, kMaxUnreliableBytes + 1));
  CHECK(client.stats().sendErrors == cBefore.sendErrors + 1);
  CHECK(server.stats().sendErrors == sBefore.sendErrors + 1);
  CHECK(client.stats().packetsSent == cBefore.packetsSent);
  CHECK(server.stats().packetsSent == sBefore.packetsSent);

  // Once a later reliable marker is through, the oversize one never shows.
  client.sendReliable(kServerPeer, payload(10, 8));
  server.sendReliable(id, payload(10, 8));
  Inbox& s = pump.inbox(server);
  Inbox& c = pump.inbox(client);
  const auto hasMarker = [](const Inbox& in) {
    return std::ranges::any_of(in.messages,
                               [](const Message& m) { return isPayload(m.data, 10, 8); });
  };
  REQUIRE(pump.until([&] { return hasMarker(s) && hasMarker(c); }));
  pump.stepFor(h->timeoutNs() / 50);
  for (const Message& m : s.messages)
    CHECK(m.data.size() <= kMaxUnreliableBytes);
  for (const Message& m : c.messages)
    CHECK(m.data.size() <= kMaxUnreliableBytes);
}

inline void sendingToAnUnknownPeerIsAnError(const HarnessFactory& make) {
  auto h = make();
  Pump pump(*h);
  ITransport& server = h->server();
  ITransport& client = h->client();
  const PeerId id = connectPair(pump, server, client);

  const PeerId stranger{static_cast<PeerId::rep>(id.value + 1000)};
  const std::vector<std::byte> msg = payload(1, 16);
  const TransportStats s0 = server.stats();
  server.sendReliable(stranger, msg);
  server.sendUnreliable(stranger, msg);
  server.sendReliable(PeerId::invalid(), msg);
  CHECK(server.stats().sendErrors == s0.sendErrors + 3);
  CHECK(server.stats().packetsSent == s0.packetsSent);

  const TransportStats c0 = client.stats();
  client.sendReliable(PeerId{static_cast<PeerId::rep>(kServerPeer.value + 1)}, msg);
  client.sendUnreliable(PeerId::invalid(), msg);
  CHECK(client.stats().sendErrors == c0.sendErrors + 2);
  CHECK(client.stats().packetsSent == c0.packetsSent);
}

inline void clientDisconnectIsSeenByBothSides(const HarnessFactory& make) {
  auto h = make();
  Pump pump(*h);
  ITransport& server = h->server();
  ITransport& client = h->client();
  const PeerId id = connectPair(pump, server, client);

  client.disconnect(kServerPeer);
  CHECK_FALSE(client.isConnected(kServerPeer));
  pump.step();  // the local side sees it on the next poll
  const ConnectionEvent* local = pump.inbox(client).firstEvent(ConnectionEventKind::Disconnected);
  REQUIRE(local != nullptr);
  CHECK(local->peer == kServerPeer);

  Inbox& s = pump.inbox(server);
  REQUIRE(pump.until([&] { return s.countEvents(ConnectionEventKind::Disconnected) > 0; }));
  CHECK(s.firstEvent(ConnectionEventKind::Disconnected)->peer == id);
  CHECK_FALSE(server.isConnected(id));

  const TransportStats c0 = client.stats();
  const TransportStats s0 = server.stats();
  client.sendReliable(kServerPeer, payload(1, 8));
  server.sendUnreliable(id, payload(1, 8));
  CHECK(client.stats().sendErrors == c0.sendErrors + 1);
  CHECK(server.stats().sendErrors == s0.sendErrors + 1);
  pump.stepFor(h->timeoutNs() / 50);
  CHECK(pump.inbox(client).countEvents(ConnectionEventKind::Disconnected) == 1);
  CHECK(s.countEvents(ConnectionEventKind::Disconnected) == 1);
}

inline void serverDisconnectIsSeenByBothSides(const HarnessFactory& make) {
  auto h = make();
  Pump pump(*h);
  ITransport& server = h->server();
  const ITransport& client = h->client();
  const PeerId id = connectPair(pump, server, client);

  server.disconnect(id);
  CHECK_FALSE(server.isConnected(id));
  pump.step();
  const ConnectionEvent* local = pump.inbox(server).firstEvent(ConnectionEventKind::Disconnected);
  REQUIRE(local != nullptr);
  CHECK(local->peer == id);

  Inbox& c = pump.inbox(client);
  REQUIRE(pump.until([&] { return c.countEvents(ConnectionEventKind::Disconnected) > 0; }));
  CHECK(c.firstEvent(ConnectionEventKind::Disconnected)->peer == kServerPeer);
  CHECK_FALSE(client.isConnected(kServerPeer));
  server.disconnect(id);  // a second disconnect is harmless and reports nothing new
  pump.stepFor(h->timeoutNs() / 50);
  CHECK(pump.inbox(server).countEvents(ConnectionEventKind::Disconnected) == 1);
  CHECK(c.countEvents(ConnectionEventKind::Disconnected) == 1);
}

inline void statsCountMessagesAndBytes(const HarnessFactory& make) {
  auto h = make();
  Pump pump(*h);
  ITransport& server = h->server();
  ITransport& client = h->client();
  const PeerId id = connectPair(pump, server, client);

  const TransportStats c0 = client.stats();
  const TransportStats s0 = server.stats();
  for (uint32_t i = 0; i < 5; ++i)
    client.sendReliable(kServerPeer, payload(i, 100));
  server.sendReliable(id, payload(9, 300));
  CHECK(client.stats().packetsSent == c0.packetsSent + 5);
  CHECK(client.stats().bytesSent == c0.bytesSent + 500);
  CHECK(server.stats().packetsSent == s0.packetsSent + 1);
  CHECK(server.stats().bytesSent == s0.bytesSent + 300);

  Inbox& s = pump.inbox(server);
  Inbox& c = pump.inbox(client);
  REQUIRE(pump.until([&] { return s.messages.size() >= 5 && !c.messages.empty(); }));
  CHECK(server.stats().packetsReceived == s0.packetsReceived + 5);
  CHECK(server.stats().bytesReceived == s0.bytesReceived + 500);
  CHECK(client.stats().packetsReceived == c0.packetsReceived + 1);
  CHECK(client.stats().bytesReceived == c0.bytesReceived + 300);
  CHECK(server.stats().sendErrors == s0.sendErrors);
  CHECK(client.stats().sendErrors == c0.sendErrors);
  CHECK(server.stats().recvDropped == 0);
  CHECK(client.stats().recvDropped == 0);
}

// recv() returns false when empty, copies are taken at send time, and each
// packet's span stays valid until the next recv() or poll().
inline void recvSpanRules(const HarnessFactory& make) {
  auto h = make();
  Pump pump(*h);
  ITransport& server = h->server();
  ITransport& client = h->client();
  const PeerId id = connectPair(pump, server, client);
  pump.stepFor(h->timeoutNs() / 50);

  Packet p;
  CHECK_FALSE(server.recv(p));
  CHECK_FALSE(client.recv(p));

  const uint64_t sentAt = h->nowNs();
  std::vector<std::byte> buf = payload(1, 200);
  client.sendReliable(kServerPeer, buf);
  buf = payload(2, 300);  // the transport copied the first one
  client.sendReliable(kServerPeer, buf);
  std::ranges::fill(buf, std::byte{0});

  // Poll the server directly (no Pump draining) until both are readable.
  std::vector<Message> got;
  const uint64_t start = h->nowNs();
  while (got.size() < 2 && h->nowNs() - start <= h->timeoutNs()) {
    h->advance();
    ConnectionEvents ev;
    client.poll(h->nowNs(), ev);  // a real transport may flush on poll
    while (client.recv(p)) {
    }
    server.poll(h->nowNs(), ev);
    while (server.recv(p)) {
      // Check the span right away, before the next recv() may reuse it.
      CHECK(p.peer == id);
      CHECK(p.lane == Lane::Reliable);
      CHECK(p.receivedNs >= sentAt);
      CHECK(p.receivedNs <= h->nowNs());
      const uint32_t n = static_cast<uint32_t>(got.size()) + 1;
      CHECK(isPayload(p.data, n, n == 1 ? 200 : 300));
      got.push_back(Message{p.peer, p.lane, p.receivedNs, {p.data.begin(), p.data.end()}});
    }
  }
  REQUIRE(got.size() == 2);
  CHECK(got[0].receivedNs <= got[1].receivedNs);
  CHECK_FALSE(server.recv(p));
}

inline void serverTellsClientsApart(const HarnessFactory& make) {
  auto h = make();
  Pump pump(*h);
  ITransport& server = h->server();
  ITransport& client = h->client();
  const PeerId id1 = connectPair(pump, server, client);
  ITransport* second = h->addClient();
  if (second == nullptr) {
    SUCCEED("harness supports a single client only");
    return;
  }
  pump.track(*second);
  const PeerId id2 = connectPair(pump, server, *second);
  CHECK(id1 != id2);

  server.sendReliable(id1, payload(1, 32));
  server.sendReliable(id2, payload(2, 32));
  second->sendReliable(kServerPeer, payload(3, 32));
  client.sendReliable(kServerPeer, payload(4, 32));
  Inbox& s = pump.inbox(server);
  Inbox& c1 = pump.inbox(client);
  Inbox& c2 = pump.inbox(*second);
  REQUIRE(pump.until(
      [&] { return s.messages.size() >= 2 && !c1.messages.empty() && !c2.messages.empty(); }));
  CHECK(isPayload(c1.messages[0].data, 1, 32));
  CHECK(isPayload(c2.messages[0].data, 2, 32));
  for (const Message& m : s.messages) {
    if (seqOf(m.data) == 3) CHECK(m.peer == id2);
    if (seqOf(m.data) == 4) CHECK(m.peer == id1);
  }

  // PeerIds are never reused: a client connecting after one left gets a new id.
  client.disconnect(kServerPeer);
  REQUIRE(pump.until([&] { return s.countEvents(ConnectionEventKind::Disconnected) > 0; }));
  CHECK(server.isConnected(id2));
  ITransport* third = h->addClient();
  REQUIRE(third != nullptr);
  pump.track(*third);
  const PeerId id3 = connectPair(pump, server, *third);
  CHECK(id3 != id1);
  CHECK(id3 != id2);
}

}  // namespace kfm::net::contract

// Expands to one TEST_CASE per contract case. `name` and `tags` are string
// literals; `factory` is anything convertible to contract::HarnessFactory.
#define KFM_TRANSPORT_CONTRACT_CASE(name, tags, factory, fn)                 \
  TEST_CASE(name ": " #fn, tags) {                                           \
    ::kfm::net::contract::fn(::kfm::net::contract::HarnessFactory(factory)); \
  }

#define KFM_TRANSPORT_CONTRACT_TESTS(name, tags, factory)                               \
  KFM_TRANSPORT_CONTRACT_CASE(name, tags, factory, connectsBothSides)                   \
  KFM_TRANSPORT_CONTRACT_CASE(name, tags, factory, deliversBothLanesBothWays)           \
  KFM_TRANSPORT_CONTRACT_CASE(name, tags, factory, reliableIsOrderedAndExactlyOnce)     \
  KFM_TRANSPORT_CONTRACT_CASE(name, tags, factory, reliableCarriesMessagesUpToTheLimit) \
  KFM_TRANSPORT_CONTRACT_CASE(name, tags, factory, unreliableRespectsItsLimit)          \
  KFM_TRANSPORT_CONTRACT_CASE(name, tags, factory, sendingToAnUnknownPeerIsAnError)     \
  KFM_TRANSPORT_CONTRACT_CASE(name, tags, factory, clientDisconnectIsSeenByBothSides)   \
  KFM_TRANSPORT_CONTRACT_CASE(name, tags, factory, serverDisconnectIsSeenByBothSides)   \
  KFM_TRANSPORT_CONTRACT_CASE(name, tags, factory, statsCountMessagesAndBytes)          \
  KFM_TRANSPORT_CONTRACT_CASE(name, tags, factory, recvSpanRules)                       \
  KFM_TRANSPORT_CONTRACT_CASE(name, tags, factory, serverTellsClientsApart)
