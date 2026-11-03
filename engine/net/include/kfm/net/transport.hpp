// The transport seam (architecture §6.3, §6.4). The message layer above it
// asks a transport for exactly two lanes: unreliable-unordered (inputs and
// authoritative frames) and reliable-ordered (control messages, snapshots).
// Every implementation (LoopbackTransport + NetEmulator, WebRtcTransport, an
// optional GnsTransport) sits behind this interface and passes the same
// contract test suite. Nothing above this header knows which one is in use.
//
// Roles: an endpoint is either a client with a single peer (the server,
// kServerPeer) or a server with up to kMaxPeers peers. PeerIds are assigned
// by the transport and reported through a Connected event; they are never
// reused within the lifetime of a transport.
//
// Time: the caller owns the clock. poll(nowNs, events) is called once per
// frame (or per emulated step) with the caller's monotonic time; messages
// that become readable during that poll carry receivedNs on the same clock.
// The emulator uses nowNs to deliver delayed packets, so a netsim over the
// loopback is fully deterministic from its seed.
//
// Ownership: recv() exposes the front message of the receive queue; its data
// span is valid until the next recv() or poll() on the same transport.
//
// Contract details pinned by the contract suite (tests/unit/net/
// transport_contract.hpp) and both implementations (v0.17):
// - A zero-length message is a valid message on either lane.
// - stats() counts user messages and their payload bytes only, never
//   transport framing, retransmissions or signaling; rejected sends are not
//   counted as sent.
// - disconnect() of a peer that never finished connecting closes it without
//   an event (it was never announced); a second disconnect() reports nothing.
// - The local side sees Disconnected on the first poll() after disconnect().
// - No method throws; the WebRTC transport catches at its boundary.
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>

namespace kfm::net {

struct PeerId {
  using rep = uint16_t;
  rep value{};

  static constexpr rep kInvalidValue = 0xFFFF;
  [[nodiscard]] static constexpr PeerId invalid() { return PeerId{kInvalidValue}; }
  [[nodiscard]] constexpr bool isValid() const { return value != kInvalidValue; }

  constexpr auto operator<=>(const PeerId&) const = default;
};

// On a client endpoint the server is always this peer.
inline constexpr PeerId kServerPeer{0};
inline constexpr uint16_t kMaxPeers = 64;

enum class Lane : uint8_t {
  Unreliable = 0,  // may be lost, duplicated or reordered; never fragmented
  Reliable,        // delivered once, in order; the transport fragments large messages
};

// An unreliable message must fit one datagram without fragmentation (§6.3).
inline constexpr size_t kMaxUnreliableBytes = 1200;
// Reliable messages carry snapshots (§6.3: 5–20 KB) with headroom.
inline constexpr size_t kMaxReliableBytes = size_t{64} * 1024;

struct Packet {
  PeerId peer{PeerId::invalid()};
  Lane lane{Lane::Unreliable};
  uint64_t receivedNs{0};           // caller's clock, see above
  std::span<const std::byte> data;  // valid until the next recv() or poll()
};

enum class ConnectionEventKind : uint8_t {
  Connected = 0,  // peer is reachable on both lanes
  Disconnected,   // peer closed, timed out or was disconnected locally
};

struct ConnectionEvent {
  ConnectionEventKind kind{ConnectionEventKind::Connected};
  PeerId peer{PeerId::invalid()};
  uint64_t timeNs{0};
};

// Fixed-capacity sink for poll(); events beyond the capacity are counted.
struct ConnectionEvents {
  static constexpr size_t kCapacity = static_cast<size_t>(2 * kMaxPeers);
  std::array<ConnectionEvent, kCapacity> items{};
  size_t count{0};
  size_t dropped{0};

  constexpr void clear() {
    count = 0;
    dropped = 0;
  }
  constexpr void push(const ConnectionEvent& e) {
    if (count < kCapacity) {
      items[count++] = e;
    } else {
      ++dropped;
    }
  }
  [[nodiscard]] constexpr std::span<const ConnectionEvent> span() const {
    return {items.data(), count};
  }
};

struct TransportStats {
  uint64_t packetsSent{0};
  uint64_t packetsReceived{0};
  uint64_t bytesSent{0};
  uint64_t bytesReceived{0};
  uint64_t sendErrors{0};   // too large, unknown peer, not connected
  uint64_t recvDropped{0};  // receive queue full
};

struct ITransport {
  ITransport() = default;
  ITransport(const ITransport&) = delete;
  ITransport& operator=(const ITransport&) = delete;
  ITransport(ITransport&&) = delete;
  ITransport& operator=(ITransport&&) = delete;
  virtual ~ITransport() = default;

  // Both send calls copy `data`; the caller's buffer can be reused at once.
  // A message over the lane's limit or to an unknown peer is dropped and
  // counted in stats().sendErrors. Sending never blocks.
  virtual void sendUnreliable(PeerId peer, std::span<const std::byte> data) = 0;
  virtual void sendReliable(PeerId peer, std::span<const std::byte> data) = 0;

  // Pops the next readable message into `out`. Returns false when none is
  // pending. Messages from one peer on the reliable lane come out in order.
  [[nodiscard]] virtual bool recv(Packet& out) = 0;

  // Advances the transport to nowNs (delivers emulated packets, drains
  // callback queues) and appends connection events that happened since the
  // last poll. Call once per frame or tick before recv().
  virtual void poll(uint64_t nowNs, ConnectionEvents& events) = 0;

  // Closes the peer; a Disconnected event follows on the next poll().
  virtual void disconnect(PeerId peer) = 0;

  [[nodiscard]] virtual bool isConnected(PeerId peer) const = 0;
  [[nodiscard]] virtual TransportStats stats() const = 0;
};

}  // namespace kfm::net
