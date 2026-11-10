// The probe's echo server, see echo_server.hpp.
#include "echo_server.hpp"

#include "kfm/net/transport.hpp"
#include "kfm/net/webrtc_transport.hpp"
#include "probe_client.hpp"

#include <cstdint>
#include <cstdio>
#include <expected>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace kfm::netprobe {

std::expected<std::unique_ptr<EchoServer>, std::string> EchoServer::create(
    uint16_t port, const std::string& bindAddress) {
  net::WebRtcServerConfig config;
  config.signalingPort = port;
  config.signalingBindAddress = bindAddress;
  // The probe measures throughput: it echoes megabytes of reliable data in
  // a burst, which a game server never sends, so the backlog cap that drops
  // a peer that is not draining (1 MiB by default) is off here.
  config.maxReliableBufferedBytes = std::numeric_limits<size_t>::max();
  auto transport = net::WebRtcTransport::createServer(config);
  if (!transport) return std::unexpected(transport.error());
  return std::make_unique<EchoServer>(std::move(*transport));
}

EchoServer::EchoServer(std::unique_ptr<net::WebRtcTransport> transport)
    : transport_(std::move(transport)) {}

void EchoServer::tick(uint64_t nowNs) {
  net::ConnectionEvents events;
  transport_->poll(nowNs, events);
  for (const auto& e : events.span()) {
    if (e.kind == net::ConnectionEventKind::Connected) {
      ++connects_;
      peers_[e.peer.value] = PeerCounters{};
      std::printf("server: peer %u connected\n", unsigned{e.peer.value});
    } else {
      ++disconnects_;
      const auto it = peers_.find(e.peer.value);
      logPeer("disconnected", e.peer, it != peers_.end() ? it->second : PeerCounters{});
      if (it != peers_.end()) peers_.erase(it);
    }
  }
  if (events.dropped != 0) std::printf("server: %zu connection events dropped\n", events.dropped);
  net::Packet packet;
  while (transport_->recv(packet)) {
    auto& c = peers_[packet.peer.value];
    const auto lane = static_cast<size_t>(packet.lane);
    ++c.received.at(lane);
    c.bytes += packet.data.size();
    const auto echo = makeEcho(packet.data);
    if (echo.empty()) {
      ++c.ignored;
      continue;
    }
    if (packet.lane == net::Lane::Reliable) {
      transport_->sendReliable(packet.peer, echo);
    } else {
      transport_->sendUnreliable(packet.peer, echo);
    }
    ++c.echoed.at(lane);
  }
  std::fflush(stdout);
}

void EchoServer::report() const {
  for (const auto& [id, c] : peers_)
    logPeer("counters", net::PeerId{id}, c);
  const net::TransportStats s = transport_->stats();
  std::printf(
      "server: totals connects=%llu disconnects=%llu packets_sent=%llu packets_received=%llu "
      "bytes_sent=%llu bytes_received=%llu send_errors=%llu recv_dropped=%llu\n",
      static_cast<unsigned long long>(connects_), static_cast<unsigned long long>(disconnects_),
      static_cast<unsigned long long>(s.packetsSent),
      static_cast<unsigned long long>(s.packetsReceived),
      static_cast<unsigned long long>(s.bytesSent),
      static_cast<unsigned long long>(s.bytesReceived),
      static_cast<unsigned long long>(s.sendErrors),
      static_cast<unsigned long long>(s.recvDropped));
  std::fflush(stdout);
}

void EchoServer::logPeer(const char* what, net::PeerId peer, const PeerCounters& c) {
  std::printf(
      "server: peer %u %s received_unreliable=%llu received_reliable=%llu "
      "echoed_unreliable=%llu echoed_reliable=%llu bytes=%llu ignored=%llu\n",
      unsigned{peer.value}, what, static_cast<unsigned long long>(c.received[0]),
      static_cast<unsigned long long>(c.received[1]), static_cast<unsigned long long>(c.echoed[0]),
      static_cast<unsigned long long>(c.echoed[1]), static_cast<unsigned long long>(c.bytes),
      static_cast<unsigned long long>(c.ignored));
}

}  // namespace kfm::netprobe
