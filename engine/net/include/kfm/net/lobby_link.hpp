// LobbyLink: a game server's connection to the lobby service (M6a,
// architecture §13.5, docs/lobby-protocol.md §5-6). Native only, part of
// kfm_net_webrtc; it drives a WebRtcTransport created with
// WebRtcServerConfig::externalSignaling.
//
// It owns one WebSocket to <lobby>/server, sends `register` when it opens,
// and once registered:
//   - `peer` from the lobby: a transport PeerId (acceptExternalPeer(); a
//     full transport answers `peer_gone` at once) and the peer's token,
//     which acceptsToken() then accepts (ServerConfig::acceptToken);
//   - `signal` from the lobby: deliverSignaling() to that peer; the
//     transport's takeOutgoingSignaling() goes back as `signal` envelopes;
//   - `peer_gone` from the lobby: closeExternalPeer(), the token is void;
//     a peer the transport dropped itself (takeDroppedExternalPeers(): a
//     kick, a failure, a timeout) is sent to the lobby as `peer_gone`;
//   - `room` / `room_closed`: the room code (room()); the host decides what
//     a new room means (kfm_server restarts the match when no human is
//     connected, spec §6 point 5);
//   - setCapacity() sends `capacity` when the value changes.
// If the socket closes, fails, or does not open and register within
// openTimeoutMs, every lobby peer is closed on the transport, the tokens and
// the room are forgotten (the lobby has closed them on its side), and the
// link reconnects after a backoff that starts at reconnectMinMs, doubles per
// failed attempt up to reconnectMaxMs and resets once registered. A
// `refused` reply (bad secret) is reported and retried the same way: the
// process stays up.
//
// Threading: the socket's callbacks (libdatachannel threads) only append to a
// mutex-guarded queue; poll() does everything on the caller's thread,
// including all sends. Call it often (kfm_server: every frame, ~1 ms) and
// after the transport's own poll() so signaling flows promptly. Nothing
// throws: the rtc:: API is caught at this boundary like the transport's.
// This header includes nothing from rtc::.
#pragma once

#include "kfm/net/transport.hpp"
#include "kfm/net/webrtc_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace kfm::net {

struct LobbyLinkConfig {
  // "ws[s]://host[:port]"; "/server" is appended when the URL has no path.
  std::string url;
  std::string secret;
  std::string region;  // e.g. "eu"
  std::string name;    // free text for the lobby's logs, e.g. "eu-1:3"
  uint32_t buildId{0};
  uint32_t contentHash{0};
  uint16_t capacity{0};  // human slots: max players minus the server's bots
  uint32_t reconnectMinMs{1'000};
  uint32_t reconnectMaxMs{30'000};
  uint32_t openTimeoutMs{10'000};  // open + registered within this, else retry
};

enum class LobbyLinkState : uint8_t {
  Connecting = 0,  // the socket is opening
  Registering,     // `register` sent, waiting for the reply
  Registered,      // live: rooms and peers flow
  Waiting,         // lost or refused; reconnecting at retryAt
};

enum class LobbyEventKind : uint8_t {
  Registered = 0,  // the lobby accepted the registration
  Refused,         // text = the reason (bad_secret, bad_request)
  RoomAssigned,    // text = the room code
  RoomClosed,      // text = the room code (also when the link is lost)
  PeerJoined,      // peer + lobbyPeer: a client was announced
  PeerGone,        // peer + lobbyPeer; byServer: we dropped it, else the lobby said so
  LinkLost,        // text = why; retryInMs = the backoff before the next attempt
};

struct LobbyEvent {
  LobbyEventKind kind{LobbyEventKind::Registered};
  PeerId peer{PeerId::invalid()};
  std::string lobbyPeer;  // the lobby's peer id (a decimal string)
  std::string text;
  bool byServer{false};
  uint32_t retryInMs{0};
};

class LobbyLink {
 public:
  struct Impl;

  // Starts connecting at once. Errors: a malformed URL, an empty secret or
  // region, a transport without externalSignaling.
  [[nodiscard]] static std::expected<std::unique_ptr<LobbyLink>, std::string> create(
      WebRtcTransport& transport, LobbyLinkConfig config, uint64_t nowNs);

  explicit LobbyLink(std::unique_ptr<Impl> impl);
  LobbyLink(const LobbyLink&) = delete;
  LobbyLink& operator=(const LobbyLink&) = delete;
  LobbyLink(LobbyLink&&) = delete;
  LobbyLink& operator=(LobbyLink&&) = delete;
  ~LobbyLink();  // closes the socket; the transport's peers are left alone

  // Does the link's work (see the header comment) and returns what happened
  // since the last call, in order. nowNs: the caller's monotonic clock.
  [[nodiscard]] std::vector<LobbyEvent> poll(uint64_t nowNs);
  // The human slots still free; sent as `capacity` when it changes (and
  // with every `register`).
  void setCapacity(uint16_t capacity);

  // A token of a peer the lobby announced and that has not gone since (the
  // token as ASCII bytes, as WebRtcTransport's lobby client sends it).
  [[nodiscard]] bool acceptsToken(std::span<const std::byte> token) const;

  [[nodiscard]] LobbyLinkState state() const;
  // The room this server hosts now, if any.
  [[nodiscard]] std::optional<std::string> room() const;
  // Lobby peers mapped to transport peers now.
  [[nodiscard]] size_t peerCount() const;
  [[nodiscard]] uint16_t capacity() const;
  // Connection attempts after the first.
  [[nodiscard]] uint32_t reconnects() const;
  // One short status for log lines: "registered room=ABCDE peers=2 capacity=7",
  // "waiting retry_in=4s", ...
  [[nodiscard]] std::string statusText(uint64_t nowNs) const;
  // The last socket or protocol error, for logs.
  [[nodiscard]] std::string lastError() const;
  // The URL actually dialled (with /server).
  [[nodiscard]] std::string url() const;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace kfm::net
