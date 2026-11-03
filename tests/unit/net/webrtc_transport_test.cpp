// WebRtcTransport over real libdatachannel on 127.0.0.1 (label `network`;
// no internet needed: host candidates only, no STUN). Each test case builds
// its own WebRtcFixture with an ephemeral signaling port.
#include "kfm/net/webrtc_transport.hpp"

#include "kfm/net/transport.hpp"
#include "net/webrtc_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace kfm::net;
using namespace kfm::net::test;

namespace {

// Connects a new client and waits for Connected on both ends. Returns the
// client's endpoint index and the PeerId the server gave it.
struct Joined {
  size_t index{0};
  PeerId peer{PeerId::invalid()};
};

[[nodiscard]] Joined join(WebRtcFixture& f) {
  const size_t serverConnectedBefore = f.inbox(0).count(ConnectionEventKind::Connected);
  const size_t c = f.addClient();
  REQUIRE(c != 0);
  const bool connected = f.waitUntil([&] {
    return f.inbox(c).count(ConnectionEventKind::Connected) == 1 &&
           f.inbox(0).count(ConnectionEventKind::Connected) == serverConnectedBefore + 1;
  });
  INFO("client error: " << f.client(c).lastError() << "; server: " << f.server().lastError());
  REQUIRE(connected);
  CHECK(f.inbox(c).lastPeer(ConnectionEventKind::Connected) == kServerPeer);
  return {c, f.inbox(0).lastPeer(ConnectionEventKind::Connected)};
}

[[nodiscard]] std::vector<std::byte> pattern(size_t size, uint8_t seed) {
  std::vector<std::byte> out(size);
  for (size_t i = 0; i < size; ++i)
    out[i] = static_cast<std::byte>((i * 31U + size_t{seed}) & 0xFFU);
  return out;
}

[[nodiscard]] size_t countOn(const Inbox& inbox, Lane lane, const std::vector<std::byte>& data) {
  size_t n = 0;
  for (const auto& p : inbox.packets)
    if (p.lane == lane && p.data == data) ++n;
  return n;
}

}  // namespace

TEST_CASE("WebRtcTransport: client of an absent server reports Disconnected", "[net][webrtc]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  // Bind a server to get a free port, then close it again.
  uint16_t port = 0;
  {
    WebRtcServerConfig sc;
    sc.signalingBindAddress = "127.0.0.1";
    auto s = WebRtcTransport::createServer(sc);
    REQUIRE(s.has_value());
    port = (*s)->listenPort();
  }
  WebRtcClientConfig config;
  config.signalingUrl = "ws://127.0.0.1:" + std::to_string(port);
  config.connectTimeoutMs = 5'000;
  auto client = WebRtcTransport::createClient(config);
  REQUIRE(client.has_value());
  ConnectionEvents events;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  bool disconnected = false;
  while (!disconnected && std::chrono::steady_clock::now() < deadline) {
    events.clear();
    (*client)->poll(WebRtcFixture::nowNs(), events);
    for (const auto& e : events.span())
      disconnected |= e.kind == ConnectionEventKind::Disconnected && e.peer == kServerPeer;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(disconnected);
  CHECK_FALSE((*client)->isConnected(kServerPeer));
}

TEST_CASE("WebRtcTransport: one client, both lanes, both directions", "[net][webrtc]") {
  WebRtcFixture f;
  INFO(f.error());
  REQUIRE(f.ok());
  CHECK(f.server().isServer());
  CHECK(f.server().listenPort() != 0);

  const auto token = bytesOf("token-123");
  const size_t c = f.addClient(token);
  REQUIRE(c == 1);
  CHECK_FALSE(f.client(c).isServer());
  CHECK(std::vector<std::byte>(f.client(c).connectionToken().begin(),
                               f.client(c).connectionToken().end()) == token);
  REQUIRE(f.waitUntil([&] {
    return f.inbox(c).count(ConnectionEventKind::Connected) == 1 &&
           f.inbox(0).count(ConnectionEventKind::Connected) == 1;
  }));
  const PeerId peer = f.inbox(0).lastPeer(ConnectionEventKind::Connected);
  CHECK(peer == PeerId{1});
  CHECK(f.server().isConnected(peer));
  CHECK(f.client(c).isConnected(kServerPeer));
  CHECK_FALSE(f.server().isConnected(PeerId{7}));

  const auto up = bytesOf("hello server");
  const auto down = bytesOf("hello client");
  f.endpoint(c).sendReliable(kServerPeer, up);
  f.endpoint(0).sendReliable(peer, down);
  REQUIRE(f.waitUntil([&] {
    return countOn(f.inbox(0), Lane::Reliable, up) == 1 &&
           countOn(f.inbox(c), Lane::Reliable, down) == 1;
  }));
  CHECK(f.inbox(0).packets.front().peer == peer);
  CHECK(f.inbox(c).packets.front().peer == kServerPeer);

  // Unreliable: loopback rarely loses anything, but keep sending until one
  // arrives so the test never depends on that.
  const auto upU = bytesOf("u-up");
  const auto downU = bytesOf("u-down");
  REQUIRE(f.waitUntil([&] {
    f.endpoint(c).sendUnreliable(kServerPeer, upU);
    f.endpoint(0).sendUnreliable(peer, downU);
    return countOn(f.inbox(0), Lane::Unreliable, upU) >= 1 &&
           countOn(f.inbox(c), Lane::Unreliable, downU) >= 1;
  }));

  // receivedNs is on the caller's clock and not in the future.
  const uint64_t now = WebRtcFixture::nowNs();
  for (const auto& p : f.inbox(0).packets)
    CHECK(p.receivedNs <= now);

  const TransportStats s = f.server().stats();
  CHECK(s.packetsSent >= 2);
  CHECK(s.packetsReceived >= 2);
  CHECK(s.bytesReceived >= up.size() + upU.size());
  CHECK(s.sendErrors == 0);
  const TransportStats cs = f.client(c).stats();
  CHECK(cs.packetsSent >= 2);
  CHECK(cs.bytesSent >= up.size() + upU.size());
}

TEST_CASE("WebRtcTransport: reliable lane is ordered and carries 60 KB", "[net][webrtc]") {
  WebRtcFixture f;
  REQUIRE(f.ok());
  const auto [c, peer] = join(f);

  constexpr uint32_t kCount = 500;
  for (uint32_t i = 0; i < kCount; ++i) {
    const std::vector<std::byte> msg = {static_cast<std::byte>(i & 0xFFU),
                                        static_cast<std::byte>(i >> 8U)};
    f.endpoint(c).sendReliable(kServerPeer, msg);
    f.endpoint(0).sendReliable(peer, msg);
  }
  const auto big = pattern(size_t{60} * 1024, 7);
  const auto biggest = pattern(kMaxReliableBytes, 9);
  f.endpoint(0).sendReliable(peer, big);
  f.endpoint(c).sendReliable(kServerPeer, biggest);

  REQUIRE(f.waitUntil([&] {
    return f.inbox(0).packets.size() == kCount + 1 && f.inbox(c).packets.size() == kCount + 1;
  }));
  for (const size_t e : {size_t{0}, c}) {
    const auto& packets = f.inbox(e).packets;
    for (uint32_t i = 0; i < kCount; ++i) {
      REQUIRE(packets[i].lane == Lane::Reliable);
      const uint32_t got = std::to_integer<uint32_t>(packets[i].data[0]) |
                           (std::to_integer<uint32_t>(packets[i].data[1]) << 8U);
      REQUIRE(got == i);
    }
  }
  CHECK(f.inbox(c).packets.back().data == big);
  CHECK(f.inbox(0).packets.back().data == biggest);
  CHECK(f.server().stats().sendErrors == 0);
  CHECK(f.client(c).stats().sendErrors == 0);
}

TEST_CASE("WebRtcTransport: size limits and unknown peers count as send errors", "[net][webrtc]") {
  WebRtcFixture f;
  REQUIRE(f.ok());
  const auto [c, peer] = join(f);

  const auto oversizeU = pattern(kMaxUnreliableBytes + 1, 1);
  const auto maxU = pattern(kMaxUnreliableBytes, 2);
  const auto oversizeR = pattern(kMaxReliableBytes + 1, 3);
  f.endpoint(c).sendUnreliable(kServerPeer, oversizeU);
  f.endpoint(0).sendUnreliable(peer, oversizeU);
  f.endpoint(c).sendReliable(kServerPeer, oversizeR);
  f.endpoint(0).sendReliable(PeerId{42}, maxU);
  f.endpoint(0).sendUnreliable(PeerId::invalid(), maxU);
  CHECK(f.client(c).stats().sendErrors == 2);
  CHECK(f.server().stats().sendErrors == 3);
  CHECK(f.client(c).stats().packetsSent == 0);

  // A message exactly at the unreliable limit goes through.
  REQUIRE(f.waitUntil([&] {
    f.endpoint(c).sendUnreliable(kServerPeer, maxU);
    return countOn(f.inbox(0), Lane::Unreliable, maxU) >= 1;
  }));
  CHECK(countOn(f.inbox(0), Lane::Unreliable, oversizeU) == 0);
  CHECK(countOn(f.inbox(0), Lane::Reliable, oversizeR) == 0);
}

TEST_CASE("WebRtcTransport: three clients get distinct ids and their own traffic",
          "[net][webrtc]") {
  WebRtcFixture f;
  REQUIRE(f.ok());
  std::vector<Joined> joined;
  joined.reserve(3);
  for (int i = 0; i < 3; ++i)
    joined.push_back(join(f));

  std::set<uint16_t> ids;
  for (const auto& j : joined)
    ids.insert(j.peer.value);
  CHECK(ids == std::set<uint16_t>{1, 2, 3});

  for (const auto& j : joined) {
    f.endpoint(0).sendReliable(j.peer, bytesOf("to " + std::to_string(j.peer.value)));
    f.endpoint(j.index).sendReliable(kServerPeer, bytesOf("from " + std::to_string(j.peer.value)));
  }
  REQUIRE(f.waitUntil([&] {
    if (f.inbox(0).packets.size() < 3) return false;
    for (const auto& j : joined)
      if (f.inbox(j.index).packets.empty()) return false;
    return true;
  }));
  for (const auto& j : joined) {
    REQUIRE(f.inbox(j.index).packets.size() == 1);
    CHECK(f.inbox(j.index).packets[0].data == bytesOf("to " + std::to_string(j.peer.value)));
  }
  for (const auto& p : f.inbox(0).packets)
    CHECK(p.data == bytesOf("from " + std::to_string(p.peer.value)));

  // Broadcast on the unreliable lane reaches everyone.
  const auto frame = bytesOf("auth-frame");
  REQUIRE(f.waitUntil([&] {
    for (const auto& j : joined)
      f.endpoint(0).sendUnreliable(j.peer, frame);
    for (const auto& j : joined)
      if (countOn(f.inbox(j.index), Lane::Unreliable, frame) == 0) return false;
    return true;
  }));
}

TEST_CASE("WebRtcTransport: disconnects are reported on both ends", "[net][webrtc]") {
  WebRtcFixture f;
  REQUIRE(f.ok());
  const auto a = join(f);
  const auto b = join(f);

  SECTION("server disconnects a client") {
    f.endpoint(0).disconnect(a.peer);
    CHECK_FALSE(f.server().isConnected(a.peer));
    REQUIRE(f.waitUntil([&] {
      return f.inbox(0).count(ConnectionEventKind::Disconnected) == 1 &&
             f.inbox(a.index).count(ConnectionEventKind::Disconnected) == 1;
    }));
    CHECK(f.inbox(0).lastPeer(ConnectionEventKind::Disconnected) == a.peer);
    CHECK_FALSE(f.client(a.index).isConnected(kServerPeer));
    // Sending to it now is an error; the other client is unaffected.
    const uint64_t errors = f.server().stats().sendErrors;
    f.endpoint(0).sendReliable(a.peer, bytesOf("late"));
    CHECK(f.server().stats().sendErrors == errors + 1);
    CHECK(f.server().isConnected(b.peer));
  }

  SECTION("client disconnects") {
    f.endpoint(b.index).disconnect(kServerPeer);
    REQUIRE(f.waitUntil([&] {
      return f.inbox(b.index).count(ConnectionEventKind::Disconnected) == 1 &&
             f.inbox(0).count(ConnectionEventKind::Disconnected) == 1;
    }));
    CHECK(f.inbox(0).lastPeer(ConnectionEventKind::Disconnected) == b.peer);
    CHECK(f.server().isConnected(a.peer));
  }

  SECTION("client goes away without a word") {
    f.removeClient(a.index);
    REQUIRE(f.waitUntil([&] { return f.inbox(0).count(ConnectionEventKind::Disconnected) == 1; }));
    CHECK(f.inbox(0).lastPeer(ConnectionEventKind::Disconnected) == a.peer);
  }

  // A new client after a disconnect never reuses an id.
  const auto c = join(f);
  CHECK(c.peer == PeerId{3});
}
