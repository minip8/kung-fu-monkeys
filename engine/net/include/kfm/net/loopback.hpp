// In-process transport with a network emulator (architecture §6.4). A
// LoopbackNetwork is a hub that owns one server endpoint and any number of
// client endpoints, each an ITransport. Messages between them pass through a
// per-link, per-direction, per-lane emulator: one-way latency, jitter, loss,
// duplication and reordering. This is how the netcode is unit-tested and how
// the netsim app runs N clients and a server in one process.
//
// Lanes under emulation:
//   - Unreliable: each copy is lost with probability lossPpm, duplicated with
//     duplicatePpm (the copy draws its own delay), delayed by latency plus a
//     uniform jitter in [0, jitterNs], and, with reorderPpm, held back by an
//     extra reorderDelayNs so later packets overtake it. Jitter alone also
//     reorders.
//   - Reliable: never lost, duplicated or reordered as seen by the receiver.
//     Loss is emulated as retransmission: every lost attempt adds
//     retransmitNs to that message's delivery time (at most kMaxAttempts), and
//     a message is never delivered before the one sent before it on the same
//     link and direction (head-of-line blocking, as with SCTP or TCP).
//     duplicatePpm and reorderPpm are ignored on this lane.
//
// Time and determinism: there is no clock and no thread. A send is stamped
// with the sender's time (its last poll(), or the connectClient() time before
// the first poll); a message becomes readable on the receiver's first poll()
// whose nowNs is at or past its delivery time, in (delivery time, send order)
// order. Its Packet::receivedNs is the delivery time, raised to the
// receiver's previous poll time if the sender's clock was behind. Jitter is
// capped at about 4.29 s. All randomness comes from one kfm::Rng per link direction, seeded
// from the network seed and the client's PeerId, so a run is a pure function
// of the seed and the sequence of calls, and changing the conditions of one
// link never perturbs another link's draws.
//
// Connection: connectClient() opens a link at nowNs; the server sees Connected
// at nowNs + the client-to-server reliable latency, the client at nowNs + the
// server-to-client reliable latency (a handshake, roughly). A disconnect() is
// seen locally on the next poll() and by the remote side after the reliable
// latency, behind every reliable message sent before it. cut() severs a link
// at once on both sides, as a timeout would. PeerIds of clients start at 1 and
// are never reused.
#pragma once

#include "kfm/core/rng.hpp"
#include "kfm/net/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <vector>

namespace kfm::net {

// Conditions of one lane in one direction. Probabilities are in parts per
// million (1'000'000 = always).
struct LaneConditions {
  uint64_t latencyNs{0};               // one-way base delay
  uint64_t jitterNs{0};                // extra delay, uniform in [0, jitterNs]
  uint32_t lossPpm{0};                 // unreliable: drop; reliable: retransmit
  uint32_t duplicatePpm{0};            // unreliable only
  uint32_t reorderPpm{0};              // unreliable only: hold back by reorderDelayNs
  uint64_t reorderDelayNs{0};          // extra delay of a held-back packet
  uint64_t retransmitNs{100'000'000};  // reliable only: cost of one lost attempt
};

// Conditions of one direction of a link.
struct LinkConditions {
  LaneConditions unreliable{};
  LaneConditions reliable{};

  // The same latency, jitter and loss on both lanes, a common shorthand.
  [[nodiscard]] static constexpr LinkConditions uniform(uint64_t latencyNs, uint64_t jitterNs = 0,
                                                        uint32_t lossPpm = 0) {
    LinkConditions c;
    c.unreliable.latencyNs = c.reliable.latencyNs = latencyNs;
    c.unreliable.jitterNs = c.reliable.jitterNs = jitterNs;
    c.unreliable.lossPpm = c.reliable.lossPpm = lossPpm;
    return c;
  }
};

inline constexpr uint32_t kPpmOne = 1'000'000;

// What the emulator did to the traffic of one link direction.
struct EmulatorStats {
  uint64_t unreliableSent{0};
  uint64_t unreliableLost{0};
  uint64_t unreliableDuplicated{0};
  uint64_t unreliableHeldBack{0};
  uint64_t reliableSent{0};
  uint64_t reliableRetransmits{0};
};

class LoopbackNetwork;

class LoopbackTransport final : public ITransport {
  struct Key {
    explicit Key() = default;
  };

 public:
  // Only a LoopbackNetwork creates endpoints (the Key is private).
  LoopbackTransport(Key /*key*/, LoopbackNetwork& net, bool isServer, PeerId self, uint64_t nowNs);
  ~LoopbackTransport() override = default;
  LoopbackTransport(const LoopbackTransport&) = delete;
  LoopbackTransport& operator=(const LoopbackTransport&) = delete;
  LoopbackTransport(LoopbackTransport&&) = delete;
  LoopbackTransport& operator=(LoopbackTransport&&) = delete;

  void sendUnreliable(PeerId peer, std::span<const std::byte> data) override;
  void sendReliable(PeerId peer, std::span<const std::byte> data) override;
  [[nodiscard]] bool recv(Packet& out) override;
  void poll(uint64_t nowNs, ConnectionEvents& events) override;
  void disconnect(PeerId peer) override;
  [[nodiscard]] bool isConnected(PeerId peer) const override;
  [[nodiscard]] TransportStats stats() const override;

  [[nodiscard]] bool isServer() const { return isServer_; }
  // The PeerId under which the server knows this endpoint (kServerPeer for
  // the server itself).
  [[nodiscard]] PeerId self() const { return self_; }
  [[nodiscard]] uint64_t nowNs() const { return nowNs_; }

  // Received messages beyond this many pending ones are dropped and counted.
  static constexpr size_t kRecvQueueCapacity = 8192;

 private:
  friend class LoopbackNetwork;

  enum class PeerState : uint8_t { None = 0, Pending, Connected, Closed };

  struct Peer {
    PeerState state{PeerState::None};
    uint64_t connectAtNs{0};
    uint64_t lastReliableDeliverNs{0};  // of messages this endpoint sent to the peer
  };

  struct InFlight {
    uint64_t deliverAtNs{0};
    uint64_t order{0};  // network-wide send order, breaks ties
    PeerId from{};      // the sender as the receiver knows it
    Lane lane{Lane::Unreliable};
    bool close{false};  // the remote side called disconnect()
    std::vector<std::byte> data;
  };

  struct Received {
    PeerId from{};
    Lane lane{Lane::Unreliable};
    uint64_t receivedNs{0};
    std::vector<std::byte> data;
  };

  [[nodiscard]] Peer* findPeer(PeerId peer);
  [[nodiscard]] const Peer* findPeer(PeerId peer) const;
  void send(PeerId peer, Lane lane, std::span<const std::byte> data);
  void addPeer(PeerId peer, uint64_t connectAtNs);
  void markClosed(PeerId peer, uint64_t atNs);
  void emitDueEvents(ConnectionEvents& events);

  LoopbackNetwork* net_;
  bool isServer_;
  PeerId self_;
  uint64_t nowNs_;
  std::vector<Peer> peers_;  // indexed by PeerId::value
  std::vector<ConnectionEvent> pendingEvents_;
  std::vector<InFlight> inbound_;
  std::deque<Received> queue_;
  Received current_{};  // backs the span handed out by recv()
  TransportStats stats_{};
};

class LoopbackNetwork {
 public:
  explicit LoopbackNetwork(uint64_t seed, uint64_t nowNs = 0);
  ~LoopbackNetwork() = default;
  LoopbackNetwork(const LoopbackNetwork&) = delete;
  LoopbackNetwork& operator=(const LoopbackNetwork&) = delete;
  LoopbackNetwork(LoopbackNetwork&&) = delete;
  LoopbackNetwork& operator=(LoopbackNetwork&&) = delete;

  [[nodiscard]] LoopbackTransport& server() { return *server_; }

  // Opens a new client link at nowNs. toServer and toClient are the
  // conditions of the two directions. When the server already has kMaxPeers
  // open links the client sees Disconnected on its first poll instead.
  // References stay valid for the lifetime of the network.
  LoopbackTransport& connectClient(uint64_t nowNs, const LinkConditions& toServer = {},
                                   const LinkConditions& toClient = {});

  // Changes the conditions of a link from now on (messages in flight keep
  // their delivery times). Unknown clients are ignored.
  void setConditions(PeerId client, const LinkConditions& toServer, const LinkConditions& toClient);

  // Severs a link at once, as a timeout would: both sides see Disconnected on
  // their next poll() and everything in flight is dropped.
  void cut(PeerId client);

  [[nodiscard]] size_t clientCount() const { return links_.size(); }
  // The client endpoint with the given PeerId, or nullptr.
  [[nodiscard]] LoopbackTransport* client(PeerId id);

  [[nodiscard]] EmulatorStats emulatorStats(PeerId client, bool toServer) const;

 private:
  friend class LoopbackTransport;

  struct Direction {
    LinkConditions conditions{};
    Rng rng{};
    EmulatorStats stats{};
  };
  struct Link {
    std::unique_ptr<LoopbackTransport> client;
    Direction toServer{};
    Direction toClient{};
  };

  [[nodiscard]] Link* findLink(PeerId client);
  [[nodiscard]] const Link* findLink(PeerId client) const;
  // Routes one message from `from` to its peer through the emulator.
  void route(LoopbackTransport& from, PeerId to, Lane lane, bool close,
             std::span<const std::byte> data);

  uint64_t seed_;
  uint64_t order_{0};
  uint16_t openLinks_{0};
  std::unique_ptr<LoopbackTransport> server_;
  std::vector<Link> links_;  // links_[i] is the client with PeerId i + 1
};

}  // namespace kfm::net
