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
// Per tick T: bot slots get their inputs from the bot source (below),
// reserved slots of a disconnected peer get an idle input, the assembler builds InputFrame(T)
// (repeat-last for missing inputs, the server never waits), the world steps,
// and every active peer gets AuthInputs with the frames it has not
// acknowledged (at most authRedundancy), its arrival margin and, every
// checksumEveryTicks, the checksum of the stepped world.
//
// Bot source (§8: server-side bots write straight into the authoritative
// frame). ServerConfig::botInputs, when set, is called once per tick before
// assembly with the world about to be stepped (tick T, after a restart that
// takes effect at T has replaced it) and a frame with tick == T, present ==
// bots() and idle inputs; it writes frame.inputs[s] for the bot slots it
// drives (an untouched slot stays idle) and nothing else is read back. The
// source sees no network and no wall clock; on the first tick of a restarted
// match matchIndex() has already advanced and botSeed() is the seed that
// restartMatch() was given, which is how the host learns to re-seed its
// agents before they act on the new world. Unset, the bot slots run the
// scripted input generator (sim::scriptedInput over an Rng seeded with
// botSeed, test scaffolding, §9) exactly as before: kfm_netsim, the
// netcode tests and the soak gates. engine/net never sees engine/ai (§4);
// the app (kfm_server's ServerHost) wires an ai::AgentHost in here.
//
// Human lead (§8 human-likeness): from each Input message the server
// estimates how far ahead of it the sending client runs, lead ≈ arrival
// margin + one-way latency, with the one-way latency taken as half the round
// trip that the message's acknowledgement reveals: the client held
// authoritative frame A (sent when the server's next tick was A + 1) and the
// message arrived when it is S, so rtt ≈ S − A − 1 ticks and lead ≈
// (N − S) + (S − A − 1) / 2 for its newest input tick N, smoothed (EMA, gain
// 1/8, Q8). humanLeadTicks() is the median over the active peers holding
// slots, rounded and clamped to kMaxHumanLeadTicks, 0 when there are none:
// the delay the host gives its bots in a mixed match.
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
//
// Match restart (§6.6): restartMatch() announces MatchConfig{config,
// startTick} to every active peer at once (and to anyone who joins before
// startTick); at startTick the world becomes initialWorld(config) at that
// tick, every active peer gets a Snapshot of it, and the bot Rng is
// reseeded. Ticks never go back and the roster carries over. A client drops
// the server checksums of ticks at or after startTick until it has loaded a
// snapshot of the new match (they describe a world it cannot have
// predicted), so a restart announced a few ticks ahead costs no resync.
//
// Shutdown: shutdown() sends Disconnect{ServerShuttingDown} to every peer
// and rejects later Hellos with RejectReason::ServerShuttingDown; the host
// keeps pumping briefly so the messages leave, then closeAll() (or
// destroying the transport) closes the connections.
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

// Writes the bot slots' inputs for frame.tick (see the header comment).
using BotInputSource = std::function<void(
    const sim::WorldState& world, const sim::MatchContent& content, sim::InputFrame& frame)>;

// humanLeadTicks() never reports more (one second: far beyond any lead the
// client controller runs at, §6.2).
inline constexpr uint8_t kMaxHumanLeadTicks = sim::kTickHz;

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
  // Seed of the first match's bots: the scripted generator's Rng when
  // botInputs is unset, the host's to use otherwise (botSeed()).
  uint64_t botSeed{0xB07B07};
  sim::ScriptedInputConfig botConfig{};  // the scripted generator only
  // The bot slots' inputs; unset = the scripted generator.
  BotInputSource botInputs;
  // Player slots open to peers and bots: the lowest maxPlayers (1..kMaxPlayers).
  uint8_t maxPlayers{sim::kMaxPlayers};
};

enum class ServerEventKind : uint8_t {
  PeerJoined = 0,   // handshake accepted; slots assigned (empty for a spectator)
  PeerReconnected,  // a token reclaimed reserved slots
  PeerRejected,     // Hello refused; reason = RejectReason
  PeerLeft,         // closed or timed out; slots reserved or released
  PeerKicked,       // reason = DisconnectReason
  SlotsReleased,    // slots left the roster (quit, grace expired, kick)
  ResyncServed,     // a ResyncRequest was answered with a snapshot
  MatchRestarted,   // restartMatch() took effect at this tick; snapshots sent
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
  uint64_t matchRestarts{0};
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
  int32_t leadEmaQ8{0};               // estimated lead (margin + rtt/2), smoothed, Q8 ticks
  uint32_t leadSamples{0};            // Input messages that fed leadEmaQ8
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

  // Schedules a new match with `seed` and the then-current roster at
  // startTick (raised to nextTick() when earlier); see the header comment.
  // MatchConfig goes out now; a later call before startTick replaces it.
  // The first form keeps the current arena, mode and power-up set; the
  // second takes them from `config` (indices into the server's content; its
  // seed is the new seed, its players are ignored: the roster carries over).
  void restartMatch(uint64_t seed, Tick startTick, uint64_t botSeed);
  void restartMatch(const sim::MatchConfig& config, Tick startTick, uint64_t botSeed);
  [[nodiscard]] std::optional<Tick> pendingRestart() const;
  // 0 for the first match, +1 per restart that took effect.
  [[nodiscard]] uint32_t matchIndex() const { return matchIndex_; }
  [[nodiscard]] const sim::MatchConfig& matchConfig() const { return config_.match; }
  // The bot seed of the current match: ServerConfig::botSeed, then the one
  // passed to the restartMatch() that took effect.
  [[nodiscard]] uint64_t botSeed() const { return botSeed_; }
  // The median estimated lead of the active peers that hold slots (see the
  // header comment), 0 when no such peer has sent an input yet.
  [[nodiscard]] uint8_t humanLeadTicks() const;

  // Disconnect{ServerShuttingDown} to every peer; later Hellos are rejected.
  // Idempotent. The peers stay until closeAll() or the transport closes them.
  void shutdown();
  [[nodiscard]] bool shuttingDown() const { return shuttingDown_; }
  // Closes every peer on the transport and forgets it (slots released, no
  // reservations).
  void closeAll();

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
  uint64_t botSeed_{0};
  uint64_t nowNs_{0};
  std::vector<Peer> peers_;  // capacity kMaxPeers, reserved at construction
  std::array<Reservation, sim::kMaxPlayers> reservations_{};
  std::vector<ServerEvent> events_;
  ServerStats stats_{};
  std::vector<std::byte> buf_;  // encode scratch, kMaxReliableBytes
  SnapshotMsg snapshot_{};      // encode scratch
  ConnectionEvents connEvents_{};
  struct PendingRestart {
    sim::MatchConfig config{};  // its players are ignored (announced())
    uint64_t botSeed{0};
    Tick startTick{};
  };
  // The match config clients are told: seed, arena, mode and power-up set
  // with the current roster.
  [[nodiscard]] sim::MatchConfig announced(const sim::MatchConfig& config) const {
    sim::MatchConfig c = config;
    c.players = present_;
    return c;
  }
  std::optional<PendingRestart> restart_;
  void applyRestart(Tick t, const PendingRestart& r);
  uint32_t matchIndex_{0};
  bool shuttingDown_{false};
};

}  // namespace kfm::net
