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
//           closes or times out (also when it never connected). A
//           signaling socket that closes before it ever opened (refused, or
//           a failed WebSocket handshake) is dialled again, up to four dials
//           100, 200, 300 ms apart, within the connect timeout (v0.44).
//
// Signaling protocol: kfm/net/webrtc_signaling.hpp.
//
// Lobby modes (M6a, architecture §13.5, docs/lobby-protocol.md):
//   server, external signaling  WebRtcServerConfig::externalSignaling: no
//           WebSocket server; the host relays signaling (kfm_server through
//           LobbyLink, kfm/net/lobby_link.hpp). acceptExternalPeer() gives a
//           peer the lobby announced a PeerId (same numbering, maxPeers and
//           connect timeout as a socket peer), deliverSignaling() feeds it
//           the client's offer and candidates, takeOutgoingSignaling()
//           returns the answer and candidates to relay, in order, and
//           takeDroppedExternalPeers() the external peers the transport
//           dropped itself (disconnect(), failure, timeout) so the host can
//           tell the lobby. closeExternalPeer() is the lobby saying the peer
//           left: the peer is dropped (Disconnected if it was announced).
//   client, lobby request       WebRtcClientConfig::lobby set and
//           signalingUrl the lobby's base URL (ws[s]://host[:port]; "/client" is
//           appended when it has no path).
//           On open the transport sends the create or join request (a
//           join with LobbyRequest::token is a rejoin, lobby protocol v1.2,
//           and the reply carries the same token); the reply must be `joined` (connectionToken()
//           becomes the room's token, lobbyRoom() the code and region, and only then the peer
//           connection is created and the offer sent) or `refused`
//           (Disconnected, lastError() "lobby refused: <reason>"). After
//           `joined` the socket carries plain signaling; a `closed` frame
//           or the socket closing at any time is Disconnected, lastError()
//           "lobby closed: <reason>" or "lobby connection closed". The socket
//           stays open for the transport's lifetime (the lobby counts a
//           room's occupancy by it). The connect timeout covers the lobby
//           reply too. The message layer's token: the NetClient's Hello goes
//           out once the transport is Connected, which is after `joined`, so
//           a client hands the token over with NetClient::setToken(
//           transport.connectionToken()) before its first pump() after
//           lobbyRoom() appears (or builds the NetClient only then).
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
// Send buffers: the unreliable lane drops a message once 64 KB wait to be
// sent; the reliable lane cannot drop one, so a peer with more than
// maxReliableBufferedBytes waiting is disconnected (lastError() says so) and
// reported at the next poll(), which bounds what a peer that stops reading
// costs the sender.
//
// The connection token (§6.7) is held for the message layer, which sends it
// in its first reliable message; the transport never validates it.
#pragma once

#include "kfm/net/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kfm::net {

// libdatachannel's log level (stdout via plog). Native only; the browser
// build logs nothing of its own.
enum class WebRtcLogLevel : uint8_t { None = 0, Fatal, Error, Warning, Info, Debug, Verbose };

inline constexpr uint32_t kDefaultWebRtcConnectTimeoutMs = 10'000;
// The reliable lane's send buffer per peer: a peer with more than this still
// unsent (it stopped reading, or its link cannot keep up) is dropped rather
// than left to grow the sender's memory. A match sends ~100 KB/s and a full
// snapshot is at most kMaxReliableBytes, so this is seconds of backlog.
inline constexpr size_t kDefaultMaxReliableBufferedBytes = size_t{1024} * 1024;

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
  // WebSocket arrived (or acceptExternalPeer() returned it) is dropped (no
  // event: it was never announced).
  uint32_t connectTimeoutMs{kDefaultWebRtcConnectTimeoutMs};
  // The host relays signaling (see "Lobby modes" above): no WebSocket server
  // is created and signalingPort/signalingBindAddress are ignored.
  bool externalSignaling{false};
  // Reliable bytes a peer may have waiting to be sent; past it the peer is
  // dropped (Disconnected; an external peer is reported by
  // takeDroppedExternalPeers()). See kDefaultMaxReliableBufferedBytes.
  size_t maxReliableBufferedBytes{kDefaultMaxReliableBufferedBytes};
};

// A client's request to the lobby (docs/lobby-protocol.md §3): create a room
// in `region`, or join the room `code`. buildId and contentHash are sent as
// 8 lower-case hex digits each and must match the room's server.
struct LobbyRequest {
  // Regions: ask for every registered region and which can host a room now;
  // the reply fills lobbyRegions() and the lobby then closes the socket
  // (Disconnected, no error).
  enum class Kind : uint8_t { Create = 0, Join, Regions };
  Kind kind{Kind::Create};
  std::string region;  // Create: e.g. "eu"
  std::string code;    // Join: 5 characters, any case
  // Join (lobby protocol v1.2): rejoin the room as the player this token was
  // issued to (32 lower-case hex digits; empty: a new player). The lobby
  // answers with the same token, so the server's reconnect reclaims the
  // player's slots.
  std::string token;
  uint32_t buildId{0};
  uint32_t contentHash{0};
};

// One region of the lobby's `regions` reply (docs/lobby-protocol.md §2-3, v1.1).
struct LobbyRegion {
  std::string name;  // e.g. "eu"
  // A free server in it at the time of the reply (listed in `free`). Always
  // true against a v1 lobby, which sends no `free` (availability unknown).
  bool free{true};
  friend bool operator==(const LobbyRegion&, const LobbyRegion&) = default;
};

// The room a lobby client joined (from the lobby's `joined` reply).
struct LobbyRoom {
  std::string code;  // upper case, e.g. "ABCDE"
  std::string region;
};

struct WebRtcClientConfig {
  // "ws://host:port" (the dedicated server's own socket, any path), or the
  // lobby's base URL "ws[s]://host[:port]" when `lobby` is set ("/client"
  // is appended unless the URL has a path).
  std::string signalingUrl;
  std::string stunServer;                  // "stun:host:port"; empty = host candidates only
  std::vector<std::byte> connectionToken;  // for the message layer, see connectionToken()
  // No Connected within this time of the first poll() -> Disconnected.
  uint32_t connectTimeoutMs{kDefaultWebRtcConnectTimeoutMs};
  // Set: lobby mode. The token comes from the lobby and replaces
  // connectionToken.
  std::optional<LobbyRequest> lobby;
  // The caller relays signaling (tests, in-process relays): no WebSocket,
  // signalingUrl is ignored, the offer is made at once; the server peer's
  // messages go in through deliverSignaling(kServerPeer, ...) and out
  // through takeOutgoingSignaling(). Not with `lobby`.
  bool externalSignaling{false};
};

// A signaling message an external-signaling transport wants relayed to `peer`.
struct OutgoingSignal {
  PeerId peer{PeerId::invalid()};
  std::string text;  // one kfm/net/webrtc_signaling.hpp message (answer, candidate)
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
  // Client: the token from the config, or in lobby mode the lobby's token
  // (its 32 hex characters as ASCII bytes; empty until `joined`). Server:
  // empty.
  [[nodiscard]] std::span<const std::byte> connectionToken() const;
  // The last exception caught at the boundary or signaling error, for logs.
  [[nodiscard]] std::string lastError() const;

  // External signaling (a server or client created with externalSignaling;
  // on any other transport these do nothing and return nothing). All run on
  // the caller's thread.
  //
  // Server: a new peer for a client the lobby announced; nullopt at maxPeers.
  [[nodiscard]] std::optional<PeerId> acceptExternalPeer();
  // One signaling message from that peer (a client: kServerPeer), as if it
  // arrived on its socket. false: unknown peer, or not a signaling message
  // (ignored).
  bool deliverSignaling(PeerId peer, std::string_view jsonText);
  // The lobby says the peer left: dropped now, Disconnected at the next
  // poll() if it was Connected. Not reported by takeDroppedExternalPeers().
  void closeExternalPeer(PeerId peer);
  // The answers and candidates produced since the last call, in order, for
  // live peers only.
  [[nodiscard]] std::vector<OutgoingSignal> takeOutgoingSignaling();
  // External peers the transport dropped on its own since the last call
  // (disconnect(), connection failure, connect timeout), in order.
  [[nodiscard]] std::vector<PeerId> takeDroppedExternalPeers();

  // Client in lobby mode: the room once `joined` arrived; nullopt before,
  // and always without a lobby request.
  [[nodiscard]] std::optional<LobbyRoom> lobbyRoom() const;
  // The lobby's answer to a Regions request (docs/lobby-protocol.md §3), once it
  // arrived: every registered region in the lobby's order, each with whether
  // it has a free server; possibly none.
  [[nodiscard]] std::optional<std::vector<LobbyRegion>> lobbyRegions() const;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace kfm::net
