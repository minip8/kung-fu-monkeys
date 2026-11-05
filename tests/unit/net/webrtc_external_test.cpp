// WebRtcTransport's external signaling (M6a, docs/lobby-protocol.md): a
// server and a client created with externalSignaling, the test relaying
// their signaling by hand in one loop (no WebSocket anywhere; the data
// channels still run over real UDP on this host, hence label `network`).
#include "kfm/net/transport.hpp"
#include "kfm/net/webrtc_signaling.hpp"
#include "kfm/net/webrtc_transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace kfm::net;

namespace {

[[nodiscard]] uint64_t steadyNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

struct Side {
  std::unique_ptr<WebRtcTransport> t;
  std::vector<ConnectionEvent> events;
  std::vector<std::vector<std::byte>> received;

  void poll(uint64_t now) {
    ConnectionEvents ev;
    t->poll(now, ev);
    for (const ConnectionEvent& e : ev.span())
      events.push_back(e);
    Packet p;
    while (t->recv(p))
      received.emplace_back(p.data.begin(), p.data.end());
  }
  [[nodiscard]] size_t count(ConnectionEventKind kind) const {
    size_t n = 0;
    for (const ConnectionEvent& e : events)
      n += e.kind == kind ? 1 : 0;
    return n;
  }
};

struct Loop {
  Side server;
  Side client;
  PeerId peer{PeerId::invalid()};  // the client's PeerId on the server
  size_t relayed{0};
  bool relay{true};

  // One step: poll both, then hand each side's signaling to the other.
  void step() {
    const uint64_t now = steadyNs();
    server.poll(now);
    client.poll(now);
    if (!relay) return;
    for (const OutgoingSignal& s : client.t->takeOutgoingSignaling()) {
      CHECK(s.peer == kServerPeer);
      CHECK(signaling::decode(s.text).has_value());
      CHECK(server.t->deliverSignaling(peer, s.text));
      ++relayed;
    }
    for (const OutgoingSignal& s : server.t->takeOutgoingSignaling()) {
      CHECK(s.peer == peer);
      CHECK(client.t->deliverSignaling(kServerPeer, s.text));
      ++relayed;
    }
  }
  bool runUntil(const std::function<bool()>& done, uint64_t timeoutNs = 15'000'000'000ull) {
    const uint64_t end = steadyNs() + timeoutNs;
    while (steadyNs() < end) {
      step();
      if (done()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
  }
};

[[nodiscard]] std::unique_ptr<WebRtcTransport> externalServer(uint16_t maxPeers = kMaxPeers,
                                                              uint32_t timeoutMs = 10'000) {
  WebRtcServerConfig sc;
  sc.externalSignaling = true;
  sc.maxPeers = maxPeers;
  sc.connectTimeoutMs = timeoutMs;
  auto s = WebRtcTransport::createServer(sc);
  REQUIRE(s.has_value());
  return s ? std::move(*s) : nullptr;
}

[[nodiscard]] std::unique_ptr<WebRtcTransport> externalClient() {
  WebRtcClientConfig cc;
  cc.externalSignaling = true;
  auto c = WebRtcTransport::createClient(cc);
  REQUIRE(c.has_value());
  return c ? std::move(*c) : nullptr;
}

}  // namespace

TEST_CASE("webrtc external signaling: a hand-relayed pair connects and talks",
          "[net][webrtc][external]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  Loop loop;
  loop.server.t = externalServer();
  loop.client.t = externalClient();
  CHECK(loop.server.t->listenPort() == 0);  // no socket at all
  const std::optional<PeerId> peer = loop.server.t->acceptExternalPeer();
  REQUIRE(peer.has_value());
  loop.peer = peer.value_or(PeerId::invalid());
  CHECK(loop.peer == PeerId{1});

  const bool connected = loop.runUntil([&] {
    return loop.server.count(ConnectionEventKind::Connected) == 1 &&
           loop.client.count(ConnectionEventKind::Connected) == 1;
  });
  INFO("server: " << loop.server.t->lastError() << "; client: " << loop.client.t->lastError());
  REQUIRE(connected);
  CHECK(loop.relayed >= 2);  // at least the offer and the answer
  CHECK(loop.server.t->isConnected(loop.peer));

  const std::vector<std::byte> ping = {std::byte{1}, std::byte{2}, std::byte{3}};
  loop.client.t->sendReliable(kServerPeer, ping);
  loop.server.t->sendReliable(loop.peer, ping);
  REQUIRE(loop.runUntil(
      [&] { return !loop.server.received.empty() && !loop.client.received.empty(); }));
  CHECK(loop.server.received.front() == ping);
  CHECK(loop.client.received.front() == ping);

  // The host drops the peer: reported for the lobby, no new signaling for it.
  loop.server.t->disconnect(loop.peer);
  const std::vector<PeerId> dropped = loop.server.t->takeDroppedExternalPeers();
  REQUIRE(dropped.size() == 1);
  CHECK(dropped.front() == loop.peer);
  CHECK(loop.server.t->takeDroppedExternalPeers().empty());
  CHECK_FALSE(loop.server.t->deliverSignaling(loop.peer, R"({"type":"offer","sdp":"x"})"));
  CHECK(loop.runUntil([&] { return loop.client.count(ConnectionEventKind::Disconnected) == 1; }));
}

TEST_CASE("webrtc external signaling: the lobby closing a connected peer disconnects it",
          "[net][webrtc][external]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  Loop loop;
  loop.server.t = externalServer();
  loop.client.t = externalClient();
  loop.peer = loop.server.t->acceptExternalPeer().value_or(PeerId::invalid());
  REQUIRE(loop.runUntil([&] { return loop.server.count(ConnectionEventKind::Connected) == 1; }));

  loop.server.t->closeExternalPeer(loop.peer);
  CHECK_FALSE(loop.server.t->isConnected(loop.peer));
  loop.step();
  CHECK(loop.server.count(ConnectionEventKind::Disconnected) == 1);
  CHECK(loop.server.events.back().peer == loop.peer);
  CHECK(loop.server.t->takeDroppedExternalPeers().empty());  // the lobby knows already
}

TEST_CASE("webrtc external signaling: limits, timeouts and misuse", "[net][webrtc][external]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  auto server = externalServer(/*maxPeers=*/2, /*timeoutMs=*/50);
  const std::optional<PeerId> a = server->acceptExternalPeer();
  const std::optional<PeerId> b = server->acceptExternalPeer();
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK_FALSE(server->acceptExternalPeer().has_value());  // maxPeers

  CHECK_FALSE(server->deliverSignaling(PeerId{99}, R"({"type":"offer","sdp":"x"})"));
  CHECK_FALSE(server->deliverSignaling(a.value_or(PeerId{}), "not json"));
  CHECK_FALSE(server->deliverSignaling(a.value_or(PeerId{}), R"({"type":"closed"})"));

  // The lobby closes one; the freed place is taken by a new, higher PeerId.
  server->closeExternalPeer(b.value_or(PeerId{}));
  const std::optional<PeerId> c = server->acceptExternalPeer();
  REQUIRE(c.has_value());
  CHECK(c.value_or(PeerId{}).value > b.value_or(PeerId{}).value);

  // Nobody signals: the connect timeout drops both silently (never
  // announced) and reports them for the lobby.
  ConnectionEvents ev;
  server->poll(1'000'000'000ull, ev);  // sets the deadlines
  server->poll(2'000'000'000ull, ev);  // past them
  CHECK(ev.count == 0);
  const std::vector<PeerId> dropped = server->takeDroppedExternalPeers();
  REQUIRE(dropped.size() == 2);
  CHECK(dropped[0] == a.value_or(PeerId{}));
  CHECK(dropped[1] == c.value_or(PeerId{}));
  CHECK(server->takeOutgoingSignaling().empty());

  // A socket server has none of it.
  WebRtcServerConfig sc;
  sc.signalingBindAddress = "127.0.0.1";
  auto socketServer = WebRtcTransport::createServer(sc);
  REQUIRE(socketServer.has_value());
  if (!socketServer) return;
  CHECK_FALSE((*socketServer)->acceptExternalPeer().has_value());

  // A lobby request needs its region or code, and excludes external signaling.
  WebRtcClientConfig cc;
  cc.signalingUrl = "ws://127.0.0.1:1/client";
  cc.lobby = LobbyRequest{};
  CHECK_FALSE(WebRtcTransport::createClient(cc).has_value());  // create without a region
  cc.lobby->region = "eu";
  cc.externalSignaling = true;
  CHECK_FALSE(WebRtcTransport::createClient(cc).has_value());
}
