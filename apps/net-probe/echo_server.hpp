// The server side of the WebRTC probe (native only): a WebRtcTransport
// server that echoes every probe message back to its sender on the same
// lane (makeEcho: first byte replaced by kEchoTag) and logs one line per
// connect, disconnect and counter report on stdout.
#pragma once

#include "kfm/net/transport.hpp"
#include "kfm/net/webrtc_transport.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <string>

namespace kfm::netprobe {

struct PeerCounters {
  std::array<uint64_t, 2> received{};  // per lane
  std::array<uint64_t, 2> echoed{};
  uint64_t bytes{0};
  uint64_t ignored{0};  // not a probe message
};

class EchoServer {
 public:
  // port 0 = ephemeral, see port(); an empty bindAddress = all interfaces.
  [[nodiscard]] static std::expected<std::unique_ptr<EchoServer>, std::string> create(
      uint16_t port, const std::string& bindAddress);

  explicit EchoServer(std::unique_ptr<net::WebRtcTransport> transport);

  // One poll + echo step.
  void tick(uint64_t nowNs);

  [[nodiscard]] uint16_t port() const { return transport_->listenPort(); }
  [[nodiscard]] uint64_t connects() const { return connects_; }
  [[nodiscard]] uint64_t disconnects() const { return disconnects_; }
  // Logs the counters of every connected peer and the transport totals.
  void report() const;

 private:
  static void logPeer(const char* what, net::PeerId peer, const PeerCounters& c);

  std::unique_ptr<net::WebRtcTransport> transport_;
  std::map<uint16_t, PeerCounters> peers_;  // connected peers
  uint64_t connects_{0};
  uint64_t disconnects_{0};
};

}  // namespace kfm::netprobe
