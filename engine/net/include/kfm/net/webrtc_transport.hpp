// WebRtcTransport: the production ITransport (architecture §6.4). Two WebRTC
// data channels per peer over DTLS/SCTP/UDP: "u" (unordered,
// maxRetransmits = 0) is the unreliable lane, "r" (ordered, reliable) the
// reliable lane. Natively it runs on libdatachannel, in the browser on
// datachannel-wasm (the same rtc:: API over RTCPeerConnection); this one
// implementation serves the dedicated server, the native client and the
// browser client.
//
// Roles:
//   server  createServer() listens for signaling on a plain WebSocket
//           (ws://, TLS is a follow-up) and accepts up to maxPeers clients.
//           Each client gets one RTCPeerConnection; its PeerId is assigned
//           when its WebSocket arrives (1, 2, 3, ..., never reused) and
//           reported by Connected once both of its channels are open.
//           Native only: in the browser createServer() returns an error.
//   client  createClient() opens the signaling WebSocket at signalingUrl
//           (ws://host:port; any path is ignored by our server), creates the
//           peer connection and both channels, sends the offer and applies
//           the answer. The server is kServerPeer; Connected follows once
//           both channels are open, Disconnected if the connection fails,
//           closes or times out (also when it never connected).
//
// Signaling protocol: kfm/net/webrtc_signaling.hpp.
//
// Threading: libdatachannel calls back on its own threads (in the browser on
// the main thread). Callbacks only append to a mutex-guarded queue; poll()
// drains it on the caller's thread and does all the work there (peer
// connections are created, answered and closed inside poll()). recv() never
// touches the queue: it pops what the last poll() made readable, and the
// returned span stays valid until the next recv() or poll().
//
// Exceptions: the rtc:: API throws; this target is compiled with exceptions
// and catches everything at the boundary (lastError() keeps the message),
// so callers built with -fno-exceptions can use it. This header includes
// nothing from rtc::.
//
// The connection token (§6.7) is held for the message layer, which sends it
// in its first reliable message; the transport never validates it.
#pragma once

#include "kfm/net/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace kfm::net {

// libdatachannel's log level (stdout via plog). Native only; the browser
// build logs nothing of its own.
enum class WebRtcLogLevel : uint8_t { None = 0, Fatal, Error, Warning, Info, Debug, Verbose };

inline constexpr uint32_t kDefaultWebRtcConnectTimeoutMs = 10'000;

struct WebRtcServerConfig {
  uint16_t signalingPort{
      0};  // TCP port of the signaling WebSocket; 0 = ephemeral, see listenPort()
  std::string signalingBindAddress;  // empty = all interfaces
  std::string stunServer;            // "stun:host:port"; empty = none (a public server needs none)
  // UDP ports ICE may use, for firewalls; 0/0 = any. With iceUdpMux every
  // peer shares icePortBegin. Native only.
  uint16_t icePortBegin{0};
  uint16_t icePortEnd{0};
  bool iceUdpMux{false};
  uint16_t maxPeers{kMaxPeers};  // clients beyond it have their WebSocket closed; ≤ kMaxPeers
  // A client that has not opened both channels this long after its
  // WebSocket arrived is dropped (no event: it was never announced).
  uint32_t connectTimeoutMs{kDefaultWebRtcConnectTimeoutMs};
};

struct WebRtcClientConfig {
  std::string signalingUrl;                // "ws://host:port"
  std::string stunServer;                  // "stun:host:port"; empty = host candidates only
  std::vector<std::byte> connectionToken;  // for the message layer, see connectionToken()
  // No Connected within this time of the first poll() -> Disconnected.
  uint32_t connectTimeoutMs{kDefaultWebRtcConnectTimeoutMs};
};

class WebRtcTransport final : public ITransport {
 public:
  struct Impl;

  // Errors (bind failure, malformed URL, unsupported role) come back as a
  // message; nothing throws.
  [[nodiscard]] static std::expected<std::unique_ptr<WebRtcTransport>, std::string> createServer(
      const WebRtcServerConfig& config);
  [[nodiscard]] static std::expected<std::unique_ptr<WebRtcTransport>, std::string> createClient(
      const WebRtcClientConfig& config);

  // Sets libdatachannel's log level. Only the first call in a process takes
  // effect; creating a transport first makes it Warning.
  static void initLogging(WebRtcLogLevel level);

  explicit WebRtcTransport(std::unique_ptr<Impl> impl);
  WebRtcTransport(const WebRtcTransport&) = delete;
  WebRtcTransport& operator=(const WebRtcTransport&) = delete;
  WebRtcTransport(WebRtcTransport&&) = delete;
  WebRtcTransport& operator=(WebRtcTransport&&) = delete;
  ~WebRtcTransport() override;  // closes every peer; no events

  void sendUnreliable(PeerId peer, std::span<const std::byte> data) override;
  void sendReliable(PeerId peer, std::span<const std::byte> data) override;
  [[nodiscard]] bool recv(Packet& out) override;
  void poll(uint64_t nowNs, ConnectionEvents& events) override;
  void disconnect(PeerId peer) override;
  [[nodiscard]] bool isConnected(PeerId peer) const override;
  [[nodiscard]] TransportStats stats() const override;

  [[nodiscard]] bool isServer() const;
  // Server: the bound signaling port (resolves signalingPort = 0). Client: 0.
  [[nodiscard]] uint16_t listenPort() const;
  // Client: the token from the config. Server: empty.
  [[nodiscard]] std::span<const std::byte> connectionToken() const;
  // The last exception caught at the boundary or signaling error, for logs.
  [[nodiscard]] std::string lastError() const;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace kfm::net
