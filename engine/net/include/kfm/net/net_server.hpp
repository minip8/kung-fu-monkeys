// NetServer: the authoritative side of the netcode (architecture §6.1, §6.6,
// §6.7). It owns the authoritative WorldState, the InputFrameAssembler, the
// FrameHistory of authoritative frames and a table of peers, and drives them
// over any ITransport. It has no clock and no thread: the host calls
// pump(nowNs) to drain the transport and tick(nowNs) once per 60 Hz tick.
//
// Per host frame:
//   server.pump(now);                       // handshakes, inputs, pings, timeouts
//   while (a tick is due) server.tick(now); // assemble, step, broadcast
//   read server.events() / tickEvents(), then server.clearEvents();
//
// Handshake: Hello (reliable) → protocol version, content hash, build id,
// token (ServerConfig::acceptToken) and slot count are checked → Welcome
// (assigned slots, MatchConfig, server tick) + Snapshot of the current world
// + PlayerJoined broadcast, or Reject. A joiner's slots are present in the
// frames from the next tick; the sim spawns them at the next round start
// (§5.4 roster). requestedSlots == 0 makes a spectator.
//
// Per tick T: bot slots get sim::scriptedInput, reserved slots of a
// disconnected peer get an idle input, the assembler builds InputFrame(T)
// (repeat-last for missing inputs, the server never waits), the world steps,
// and every active peer gets AuthInputs with the frames it has not
// acknowledged (at most authRedundancy), its arrival margin and, every
// checksumEveryTicks, the checksum of the stepped world.
//
// Abuse resistance (§6.7): inputs are accepted only for slots the sending
// peer owns, buttons are range-checked, ticks must lie in the assembler's
// window, Input messages are rate-limited per peer (token bucket), and a peer
// that keeps sending malformed or foreign messages is kicked.
//
// Reconnect (§6.6): when a peer that holds slots and presented a token drops,
// its slots stay reserved (present, idle) for reconnectGraceTicks; a Hello
// with the same token reclaims them and gets a fresh snapshot. A Hello whose
// token belongs to a still-connected peer takes the slots over from it.
#pragma once

#include "kfm/core/rng.hpp"
#include "kfm/core/types.hpp"
#include "kfm/net/input_history.hpp"
#include "kfm/net/messages.hpp"
#include "kfm/net/transport.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/scripted_inputs.hpp"
#include "kfm/sim/world_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace kfm::net {

struct ServerConfig {
  // Seed and initial roster of the match (bots are added with setBot()).
  sim::MatchConfig match{};
  uint32_t buildId{0};
  // A checksum of the world rides in AuthInputs every this many ticks (0 = never).
  uint16_t checksumEveryTicks{10};
  // How long a dropped peer's slots stay reserved (default 10 s).
  uint32_t reconnectGraceTicks{10 * sim::kTickHz};
  // No packet from a peer for this long disconnects it.
  uint64_t peerTimeoutNs{5'000'000'000ull};
  // Input messages per second per peer: the tick rate plus slack (§6.7).
  uint32_t inputMsgsPerSecond{sim::kTickHz + 30};
  // Protocol errors (undecodable, foreign slot, bad range, too early, out of
  // sequence) tolerated per protocolErrorWindowNs before a kick.
  uint32_t maxProtocolErrors{32};
  uint64_t protocolErrorWindowNs{10'000'000'000ull};
  // Frames per AuthInputs at most (the unacknowledged ones are resent).
  uint8_t authRedundancy{kMaxInputRedundancy};
  // A peer may ask for a resync snapshot at most this often.
  uint64_t minResyncIntervalNs{250'000'000ull};
  // Token check; empty accepts every token (including none).
  std::function<bool(std::span<const std::byte> token)> acceptToken;
  // Seed of the Rng that drives the bot slots.
  uint64_t botSeed{0xB07B07};
  sim::ScriptedInputConfig botConfig{};
};

enum class ServerEventKind : uint8_t {
  PeerJoined = 0,   // handshake accepted; slots assigned (empty for a spectator)
  PeerReconnected,  // a token reclaimed reserved slots
  PeerRejected,     // Hello refused; reason = RejectReason
  PeerLeft,         // closed or timed out; slots reserved or released
  PeerKicked,       // reason = DisconnectReason
  SlotsReleased,    // slots left the roster (quit, grace expired, kick)
  ResyncServed,     // a ResyncRequest was answered with a snapshot
};

struct ServerEvent {
  ServerEventKind kind{ServerEventKind::PeerJoined};
  PeerId peer{PeerId::invalid()};
  sim::PlayerMask slots{0};
  Tick tick{};
  uint8_t reason{0};
};

struct ServerStats {
  uint64_t ticks{0};
  uint64_t substitutedInputs{0};  // slot-ticks of connected peers repeated by the assembler
  uint32_t substitutedLastTick{0};
  uint64_t resyncsServed{0};
  uint64_t snapshotsSent{0};
  uint64_t inputsAccepted{0};  // slot-ticks stored for a future tick
  uint64_t inputsLate{0};      // redundant copies of ticks already assembled
  uint64_t rejectedInputs{0};  // messages or slot-ticks refused (§6.7)
  uint64_t rateLimited{0};     // Input messages over the rate limit
  uint64_t protocolErrors{0};
  uint64_t kicks{0};
  uint64_t joins{0};
  uint64_t reconnects{0};
  uint64_t rejects{0};
  uint64_t bytesSent{0};
  uint64_t bytesReceived{0};
};

enum class PeerState : uint8_t { AwaitingHello = 0, Active };

struct ServerPeerInfo {
  PeerId peer{PeerId::invalid()};
  PeerState state{PeerState::AwaitingHello};
  sim::PlayerMask slots{0};
  std::optional<Tick> lastInputTick;  // newest input tick received
  std::optional<Tick> lastAuthAck;    // newest authoritative tick it holds
  int8_t lastMargin{0};               // inputTick − serverTick of its newest input
  int32_t marginEmaQ8{0};             // smoothed, Q8 ticks
  uint64_t inputMsgs{0};
  uint64_t substitutions{0};  // its slot-ticks the assembler had to repeat
  uint64_t rejectedInputs{0};
  uint64_t rateLimited{0};
  uint64_t protocolErrors{0};
  uint64_t resyncsServed{0};
  uint64_t bytesIn{0};
  uint64_t bytesOut{0};
};

class NetServer {
 public:
  NetServer(ITransport& transport, const sim::ContentDB& content, ServerConfig config);
  ~NetServer() = default;
  NetServer(const NetServer&) = delete;
  NetServer& operator=(const NetServer&) = delete;
  NetServer(NetServer&&) = delete;
  NetServer& operator=(NetServer&&) = delete;

  // Marks a free slot as a server-side bot (present from the next tick) or
  // releases it. Returns false when the slot is owned by a peer.
  bool setBot(PlayerId slot, bool bot);

  // Drains the transport: connection events, handshakes, inputs, pings,
  // resync requests; then disconnects peers silent for peerTimeoutNs.
  void pump(uint64_t nowNs);
  // Assembles, steps and broadcasts one tick.
  void tick(uint64_t nowNs);

  // Disconnects a peer (Disconnect{Kicked}); its slots are released.
  void kick(PeerId peer, DisconnectReason reason = DisconnectReason::Kicked);

  [[nodiscard]] const sim::WorldState& world() const { return world_; }
  // The tick that tick() assembles next (== world().tick).
  [[nodiscard]] Tick nextTick() const { return assembler_.nextTick(); }
  [[nodiscard]] sim::PlayerMask present() const { return present_; }
  [[nodiscard]] sim::PlayerMask bots() const { return bots_; }
  [[nodiscard]] const FrameHistory& frames() const { return frames_; }
  // SimEvents of the last tick() (logging, bots).
  [[nodiscard]] const sim::EventSink& tickEvents() const { return sink_; }

  // Events since the last clearEvents().
  [[nodiscard]] std::span<const ServerEvent> events() const { return events_; }
  void clearEvents() { events_.clear(); }

  [[nodiscard]] const ServerStats& stats() const { return stats_; }
  [[nodiscard]] std::optional<ServerPeerInfo> peerInfo(PeerId peer) const;
  [[nodiscard]] std::vector<ServerPeerInfo> peers() const;
  // The peer that owns a slot (invalid for a bot, a reservation or a free slot).
  [[nodiscard]] PeerId ownerOf(PlayerId slot) const;

 private:
  struct Peer {
    ServerPeerInfo info{};
    bool used{false};
    uint64_t lastRecvNs{0};
    uint64_t bucketNs{0};  // token bucket, in ns × messages
    uint64_t bucketAtNs{0};
    uint64_t errorWindowStartNs{0};
    uint32_t errorsInWindow{0};
    uint64_t lastResyncNs{0};
    bool hasResynced{false};
    std::optional<int8_t> marginSinceSend;  // min since the last AuthInputs
    std::array<std::byte, kMaxTokenBytes> token{};
    uint8_t tokenLength{0};
  };
  struct Reservation {
    bool used{false};
    sim::PlayerMask slots{0};
    Tick expires{};
    std::array<std::byte, kMaxTokenBytes> token{};
    uint8_t tokenLength{0};
  };

  [[nodiscard]] Peer* findPeer(PeerId id);
  [[nodiscard]] const Peer* findPeer(PeerId id) const;
  Peer* addPeer(PeerId id, uint64_t nowNs);
  void removePeer(Peer& p, bool reserve);
  void onPacket(Peer& p, const Packet& packet);
  void onHello(Peer& p, const Packet& packet);
  void onInput(Peer& p, const Packet& packet);
  void onJoinMatch(Peer& p, const Packet& packet);
  void onResync(Peer& p, const Packet& packet);
  void protocolError(Peer& p, uint64_t nowNs);
  void reject(Peer& p, RejectReason reason);
  [[nodiscard]] sim::PlayerMask freeSlots() const;
  [[nodiscard]] sim::PlayerMask reservedSlots() const;
  void assignSlots(Peer& p, sim::PlayerMask slots);
  void releaseSlots(sim::PlayerMask slots, PeerId peer);
  void welcome(Peer& p);
  void sendSnapshot(Peer& p);
  void broadcastAuth();
  void expireReservations();
  void sendReliable(Peer& p, std::span<const std::byte> bytes);
  void sendUnreliable(Peer& p, std::span<const std::byte> bytes);
  template <class M>
  void sendReliableMsg(Peer& p, const M& m);
  void emit(ServerEventKind kind, PeerId peer, sim::PlayerMask slots, uint8_t reason = 0);

  ITransport* transport_;
  const sim::ContentDB* content_;
  ServerConfig config_;
  uint32_t contentHash_{0};
  sim::WorldState world_{};
  InputFrameAssembler assembler_;
  FrameHistory frames_{};
  sim::EventSink sink_{};
  sim::PlayerMask present_{0};
  sim::PlayerMask bots_{0};
  Rng botRng_{};
  uint64_t nowNs_{0};
  std::vector<Peer> peers_;  // capacity kMaxPeers, reserved at construction
  std::array<Reservation, sim::kMaxPlayers> reservations_{};
  std::vector<ServerEvent> events_;
  ServerStats stats_{};
  std::vector<std::byte> buf_;  // encode scratch, kMaxReliableBytes
  SnapshotMsg snapshot_{};      // encode scratch
  ConnectionEvents connEvents_{};
};

}  // namespace kfm::net
