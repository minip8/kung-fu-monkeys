// NetClient: prediction and rollback against a NetServer (architecture §6.1,
// §6.2, §6.6, §6.8, §6.9). No clock, no thread, no heap after construction:
// the host passes its monotonic nowNs. sizeof(NetClient) is large (a ring of
// kRollbackWindow WorldStates plus histories), so hosts and tests allocate it
// with std::make_unique, never on the stack.
//
// Per host frame (the §6.2 client tick order):
//   client.beginFrame();                 // clears the frame's rollback report and events
//   client.pump(now);                    // auth frames, snapshots, control, pings
//   client.reconcile();                  // roll back and re-simulate on misprediction
//   for (n = client.ticksDue(now); n > 0; --n) {
//     sample one PlayerInput per local slot, ascending slot order
//     client.tick(inputs);               // send with redundancy, predict, save, step
//   }
//   if (client.frameReport().rolledBack) presentation::onRollback(ps, prevWorld(), world(), ...);
//   render world() / prevWorld() (alpha = renderPhaseQ16() / 65536) with drainEvents().
//
// connect() starts a session over a transport (Hello + Ping). On Welcome +
// Snapshot + an RTT sample the client loads the world at the snapshot tick T
// and runs forward to L = estimated server tick + targetLeadTicks() − input
// delay, then its TickClock runs at the LeadController's 98–102 % rate. A
// later snapshot (resync) is a hard load at its tick followed by a run
// forward to the current L. A reconnect is connect() on a new transport with
// the same token. A spectator (requestedSlots = 0) never predicts: it steps
// only authoritative frames and stays spectatorDelayTicks behind the newest.
//
// Prediction: local slots use the inputs submitted for that tick (input delay
// D: the inputs passed at local tick L are for tick L + D); remote slots and
// the present mask repeat the newest authoritative frame before the tick.
// Button edges are detected by the sim, so a repeated held button never
// re-fires. Every authoritative frame is compared with the frame the client
// used for its tick; the earliest difference restores the saved state of that
// tick and re-simulates to L. Server checksums are compared at confirmed
// ticks against the saved state of that tick (skipped when it is older than
// the ring); a mismatch sends ResyncRequest.
//
// Match restart: a MatchConfig{config, startTick} from the server announces
// a new match whose world starts at startTick. The client keeps predicting
// its current timeline, drops server checksums of ticks at or after
// startTick (they describe the new world) and hard-loads the Snapshot the
// server sends at startTick, then predicts forward as after a resync
// (FrameReport::matchRestarted, snapshotLoaded and rolledBack are set).
//
// The room (v0.42, protocol 6, §6.6): the client keeps the newest RoomState
// (roomState()) and sends the host's RoomSettings and Start. While the room
// is not Playing (Waiting or Over) the server does not tick, so the client
// holds too: ticksDue() returns 0 and its clock restarts on every call (no
// backlog builds up), while pings keep the link and the RTT estimate alive
// and no input is sent. Without any RoomState (a server on the M3 flow,
// kfm_netsim) nothing is ever held. When the room turns Playing again the
// client keeps holding until the new match's snapshot has arrived and a Pong
// shows the server ticking past it, then loads it and runs forward to
// estimated server tick + target lead, exactly as the initial sync does
// (lead controller reset, clock restarted), so the stale lead of before the
// pause never carries over.
//
// Events (§5.9) pass through an EventJournal: re-emitted events of a
// re-simulation are deduplicated, abandoned undelivered ones dropped, events
// of verified ticks are final.
#pragma once

#include "kfm/core/types.hpp"
#include "kfm/net/clock_sync.hpp"
#include "kfm/net/event_journal.hpp"
#include "kfm/net/input_history.hpp"
#include "kfm/net/messages.hpp"
#include "kfm/net/transport.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/world_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace kfm::net {

// Saved states for rollback (§6.1: 32 ticks ≈ 530 ms).
inline constexpr uint32_t kRollbackWindow = 32;
inline constexpr uint8_t kMaxInputDelayTicks = 3;
// Rollback depth histogram buckets: depths 0..62, the last bucket is >= 63.
inline constexpr uint32_t kRollbackDepthBuckets = 64;

struct ClientConfig {
  uint32_t buildId{0};
  std::array<std::byte, kMaxTokenBytes> token{};
  uint8_t tokenLength{0};
  uint8_t requestedSlots{1};  // 0 = spectate
  Nickname nickname{};        // v0.42: sent in Hello
  // Fixed input delay, 0..kMaxInputDelayTicks (§6.2).
  uint8_t inputDelayTicks{0};
  // Inputs per Input message at most (the unacknowledged ones are resent).
  uint8_t inputRedundancy{kMaxInputRedundancy};
  // A spectator stays this many ticks behind the newest confirmed frame.
  uint8_t spectatorDelayTicks{3};
  uint64_t pingIntervalNs{250'000'000ull};
  uint64_t syncPingIntervalNs{50'000'000ull};  // until the session runs
  uint64_t serverTimeoutNs{5'000'000'000ull};
  uint64_t resyncRetryNs{1'000'000'000ull};
  // RTT samples needed before the initial sync sets the lead.
  uint32_t syncRttSamples{3};
  // Ticks ticksDue() returns at most per call; a larger backlog is dropped.
  uint32_t maxTicksPerCall{8};
  LeadControllerConfig lead{};

  void setToken(std::span<const std::byte> bytes);
};

enum class ClientState : uint8_t {
  Idle = 0,      // never connected
  Connecting,    // Hello sent, waiting for Welcome
  Syncing,       // Welcome received; waiting for the snapshot and an RTT sample
  Running,       // predicting
  Disconnected,  // transport closed, timed out or kicked
  Rejected,      // the server refused the Hello (rejectReason())
};

// What happened since beginFrame(); a host applies presentation::onRollback
// when rolledBack is set (§6.9).
struct FrameReport {
  bool rolledBack{false};  // the timeline changed (rollback or snapshot load)
  uint32_t rollbacks{0};
  uint32_t maxDepth{0};  // ticks re-simulated by the deepest rollback
  bool snapshotLoaded{false};
  bool matchRestarted{false};  // the snapshot was the start of an announced new match
};

struct ClientNetStats {
  // Link.
  uint64_t rttNs{0};
  uint64_t jitterNs{0};
  uint64_t minRttNs{0};
  uint64_t maxRttNs{0};
  uint32_t lossPpm{0};  // authoritative packets lost, estimated from their newest tick
  // Rollback.
  std::array<uint64_t, kRollbackDepthBuckets> rollbackDepth{};  // [d]: rollbacks of depth d
  uint64_t rollbacks{0};
  uint64_t resimulatedTicks{0};
  uint32_t maxRollbackDepth{0};
  uint64_t mispredictedTicks{0};    // authoritative frames that differed from the prediction
  uint64_t mispredictedRemote{0};   // ... in a remote slot's input
  uint64_t mispredictedLocal{0};    // ... in a local slot's input (server substituted)
  uint64_t mispredictedPresent{0};  // ... in the present mask
  uint64_t substitutionsSeen{0};    // confirmed local slot-ticks whose input the server repeated
  uint64_t anchorRollbacks{0};      // deeper than the ring: re-simulated from the anchor
  uint64_t rollbackOverflows{0};    // deeper than the frame history (resync instead)
  // Clock and lead.
  int32_t leadTicks{0};        // L − estimated server tick at the last ticksDue()
  int32_t targetLeadTicks{0};  // §6.2 target from RTT and jitter
  int8_t arrivalMargin{0};     // last reported by the server
  int32_t arrivalMarginQ8{0};  // smoothed
  uint32_t ratePpm{kRateOnePpm};
  uint64_t hardJumps{0};
  uint64_t clockDroppedTicks{0};
  // Consistency.
  uint64_t checksumsCompared{0};
  uint64_t checksumsSkipped{0};
  uint64_t checksumMismatches{0};
  uint64_t resyncRequests{0};
  uint64_t snapshotsLoaded{0};
  uint64_t matchRestarts{0};  // snapshots of an announced new match loaded
  // Traffic.
  uint64_t bytesSent{0};
  uint64_t bytesReceived{0};
  uint64_t packetsSent{0};
  uint64_t packetsReceived{0};
  uint64_t authPackets{0};
  uint64_t decodeErrors{0};
  uint64_t ticks{0};  // local ticks stepped by tick()
};

// Passed to the desync hook (dev builds dump both states, §6.6).
struct DesyncInfo {
  Tick tick{};
  uint32_t serverChecksum{0};
  uint32_t localChecksum{0};
  const sim::WorldState* local{nullptr};
};

class NetClient {
 public:
  explicit NetClient(const sim::ContentDB& content, const ClientConfig& config = {});
  ~NetClient() = default;
  NetClient(const NetClient&) = delete;
  NetClient& operator=(const NetClient&) = delete;
  NetClient(NetClient&&) = delete;
  NetClient& operator=(NetClient&&) = delete;

  // Starts a session (Hello + Ping) over `transport`, which must outlive the
  // session. A running session is discarded (a reconnect passes a new
  // transport and keeps the token); cumulative stats are kept.
  void connect(ITransport& transport, uint64_t nowNs);
  // Replaces the connection token the next Hello carries (a lobby hands the
  // token to the transport after the NetClient exists: WebRtcTransport's
  // lobby mode, M6a). A Hello already sent keeps the old one.
  void setToken(std::span<const std::byte> token);
  // Sends Disconnect{Quit} and closes the transport.
  void disconnect();
  void setConfig(const ClientConfig& config) { config_ = config; }

  void beginFrame();
  void pump(uint64_t nowNs);
  void reconcile();
  [[nodiscard]] uint32_t ticksDue(uint64_t nowNs);
  // One local tick: inputs[k] is the input of the k-th local slot in
  // ascending slot order (missing entries repeat that slot's last input). A
  // spectator steps only when the authoritative frame is held.
  void tick(std::span<const sim::PlayerInput> inputs = {});

  // Undelivered events of the ticks stepped so far, in tick order; the span
  // is valid until the next call.
  [[nodiscard]] std::span<const sim::SimEvent> drainEvents();

  [[nodiscard]] ClientState state() const { return state_; }
  [[nodiscard]] bool running() const { return state_ == ClientState::Running; }
  [[nodiscard]] RejectReason rejectReason() const { return rejectReason_; }
  [[nodiscard]] std::optional<DisconnectReason> disconnectReason() const {
    return disconnectReason_;
  }
  [[nodiscard]] bool spectator() const { return localSlots_ == 0; }
  [[nodiscard]] sim::PlayerMask localSlots() const { return localSlots_; }
  [[nodiscard]] const sim::MatchConfig& matchConfig() const { return matchConfig_; }
  // The config the local world runs with (its arena and mode select the
  // content every step uses); trails matchConfig() across a restart until
  // the new match's snapshot is loaded.
  [[nodiscard]] const sim::MatchConfig& simConfig() const { return simConfig_; }
  [[nodiscard]] sim::MatchContent matchContent() const {
    return sim::matchContent(*content_, simConfig_);
  }
  // Start tick of an announced match restart whose snapshot has not loaded yet.
  [[nodiscard]] std::optional<Tick> pendingRestart() const { return restartTick_; }

  // --- the room (v0.42) ---------------------------------------------------------
  // The newest RoomState (nullopt: none received, the server runs the M3 flow).
  [[nodiscard]] const std::optional<RoomStateMsg>& roomState() const { return room_; }
  // RoomStates received this session (a host's UI waits for the answer to
  // its RoomSettings before it shows the server's settings again).
  [[nodiscard]] uint32_t roomUpdates() const { return roomUpdates_; }
  // Whether the client is holding its timeline (see the header comment).
  [[nodiscard]] bool holding() const { return roomHeld() || resumePending_; }
  // The host's requests; the server ignores anyone else's (Running only).
  void sendRoomSettings(const RoomSettings& settings);
  void sendStart();

  // L: the tick world() will step next.
  [[nodiscard]] Tick localTick() const { return world_.tick; }
  // Newest tick T with every authoritative frame from the session base to T.
  [[nodiscard]] std::optional<Tick> confirmedTick() const;
  [[nodiscard]] const sim::WorldState& world() const { return world_; }
  // The state at L − 1 (interpolation); world() when there is none.
  [[nodiscard]] const sim::WorldState& prevWorld() const;
  [[nodiscard]] uint32_t renderPhaseQ16() const { return clock_.phaseQ16(); }
  // Input windows of the last ticksDue() batch (§5.4, §6.8): each tick spans
  // tickWallNs() at the rate that batch ran at, and the batch's last tick ends
  // at that call's nowNs, the ones before it counting back in whole ticks
  // (the same anchoring as the local client's fixed-step clock). i is the
  // tick's index in the batch, 0 = the first tick() after ticksDue().
  [[nodiscard]] uint64_t tickWallNs() const { return TickClock::tickWallNs(dueRatePpm_); }
  [[nodiscard]] uint64_t tickStartNs(uint32_t i) const;
  [[nodiscard]] const FrameReport& frameReport() const { return report_; }
  [[nodiscard]] const ClientNetStats& netStats() const { return stats_; }
  [[nodiscard]] const EventJournal& journal() const { return journal_; }
  [[nodiscard]] const RttEstimator& rtt() const { return rtt_; }
  [[nodiscard]] const LeadController& leadController() const { return lead_; }
  // Server tick estimated from the newest Pong (nullopt before one).
  [[nodiscard]] std::optional<Tick> estimatedServerTick(uint64_t nowNs) const;

  void setDesyncHook(std::function<void(const DesyncInfo&)> hook) { desyncHook_ = std::move(hook); }
  // Test and debug hook: applies f to the current world and every saved
  // state, so a corruption survives rollback (checksum resync tests).
  void debugCorrupt(const std::function<void(sim::WorldState&)>& f);

 private:
  struct Saved {
    sim::WorldState state{};  // state.tick == the tick stepped from it
    bool valid{false};
  };
  struct UsedFrame {
    sim::InputFrame frame{};
    bool valid{false};
    bool authoritative{false};
  };
  struct PendingChecksum {
    Tick tick{};
    uint32_t sum{0};
    bool valid{false};
  };
  static constexpr size_t kPendingChecksums = 32;
  static constexpr size_t kFrameEventCapacity = 2048;

  void resetSession();
  void sendHello(uint64_t nowNs);
  void advanceVerified();
  [[nodiscard]] uint32_t inputDelay() const;
  void onPacket(const Packet& packet);
  void onAuthInputs(const Packet& packet);
  void onSnapshot(const Packet& packet);
  void tryStart(uint64_t nowNs);
  void tryResume(uint64_t nowNs);
  [[nodiscard]] bool roomHeld() const { return room_ && room_->phase != RoomPhase::Playing; }
  void loadSnapshot(Tick runTo);
  void advanceConfirmed();
  void checkChecksums();
  void requestResync(Tick tick, uint32_t localSum);
  void sendInputs();
  void sendPing(uint64_t nowNs);
  void send(Lane lane, std::span<const std::byte> bytes);
  template <class M>
  void sendMsg(Lane lane, const M& m);
  void stepOne();  // builds the frame for L, saves, steps, journals
  void fastForward(int32_t ticks, bool recordLocal);
  [[nodiscard]] sim::InputFrame buildFrame(Tick t, bool* authoritative) const;
  [[nodiscard]] const sim::InputFrame* latestAuthBefore(Tick t) const;
  [[nodiscard]] Saved* savedAt(Tick t);
  [[nodiscard]] const Saved* savedAt(Tick t) const;
  [[nodiscard]] UsedFrame* usedAt(Tick t);
  [[nodiscard]] const sim::WorldState* stateAt(Tick t) const;
  void rollbackTo(Tick t);
  [[nodiscard]] uint32_t spectatorRatePpm() const;
  void recordBatch(uint64_t nowNs, uint32_t ticks, uint32_t ratePpm);

  const sim::ContentDB* content_;
  ClientConfig config_;
  uint32_t contentHash_{0};
  ITransport* transport_{nullptr};
  ClientState state_{ClientState::Idle};
  RejectReason rejectReason_{RejectReason::ProtocolMismatch};
  std::optional<DisconnectReason> disconnectReason_;

  // Session.
  sim::MatchConfig matchConfig_{};
  // The config the local world simulates with (arena, mode): the one of the
  // last snapshot loaded. matchConfig_ already holds an announced restart's.
  sim::MatchConfig simConfig_{};
  sim::PlayerMask localSlots_{0};
  bool haveWelcome_{false};
  bool haveSnapshot_{false};
  bool helloSent_{false};
  Tick base_{};      // first tick of the current timeline (snapshot tick)
  Tick verified_{};  // first tick whose used frame is not known authoritative
  std::optional<Tick> confirmed_;
  sim::PlayerMask basePresent_{0};
  Tick marginFrom_{};  // margins of inputs before this tick are ignored
  bool awaitingResync_{false};
  std::optional<Tick> restartTick_;  // announced, snapshot not loaded yet
  std::optional<RoomStateMsg> room_;
  uint32_t roomUpdates_{0};
  bool resumePending_{false};    // the room turned Playing; waiting to reload
  bool resumeSnapshot_{false};   // snapshot_ holds the snapshot to resume from
  bool resumeIsRestart_{false};  // ... and it starts an announced new match
  uint64_t resyncSentNs_{0};
  uint64_t lastRecvNs_{0};
  uint64_t nextPingNs_{0};
  uint64_t nowNs_{0};
  std::optional<Tick> firstAuthTick_;
  std::optional<Tick> newestAuthPacketTick_;
  uint64_t authPacketsSinceFirst_{0};
  struct LastPong {
    Tick tick{};
    uint64_t recvNs{0};
    bool valid{false};
  } pong_{};

  // Timeline.
  sim::WorldState world_{};
  // The state at verified_: every frame before it is authoritative. A
  // misprediction older than the ring re-simulates from here.
  sim::WorldState anchor_{};
  std::array<Saved, kRollbackWindow> ring_{};
  std::array<UsedFrame, kInputHistoryTicks> used_{};
  FrameHistory auth_{};
  InputHistory local_{};
  std::array<PendingChecksum, kPendingChecksums> checksums_{};
  sim::EventSink sink_{};
  sim::EventSink scratchSink_{};  // anchor steps: their events were journalled already
  EventJournal journal_{};
  std::array<sim::SimEvent, kFrameEventCapacity> frameEvents_{};
  SnapshotMsg snapshot_{};  // decode target

  // Clock.
  RttEstimator rtt_{};
  LeadController lead_;
  TickClock clock_{};
  // The last ticksDue() batch: its nowNs, tick count and clock rate.
  uint64_t dueNowNs_{0};
  uint32_t dueTicks_{0};
  uint32_t dueRatePpm_{kRateOnePpm};

  FrameReport report_{};
  ClientNetStats stats_{};
  std::function<void(const DesyncInfo&)> desyncHook_;
  // Encode scratch: a client only sends small messages (Input, control).
  std::array<std::byte, kMaxUnreliableBytes> buf_{};
};

}  // namespace kfm::net
