// NetSession: the playable client in network mode (architecture §6.1, §6.2,
// §6.8, §6.9, §7, §11 M3). SDL-free and unit-tested headlessly against an
// in-process NetServer over the loopback emulator.
//
// A separate class from MatchSession on purpose: the local path (lobby,
// bots, pause/step/time scale, live content tuning, the fixed-step clock)
// stays exactly as M2 shipped and tested it, and nothing here can change it.
// The two meet only at the lobby: the host builds a NetSession from the
// MatchSession's InputSampler when the player connects, and hands the
// sampler back (lobbySampler()) when the session ends.
//
// It owns the NetClient (heap: ~360 KB), the transport the caller injects
// (the app passes a WebRtcTransport, tests a LoopbackTransport they keep),
// a copy of the InputSampler, the PresentationState and the per-frame event
// accumulator. Per frame, push() every InputEvent, then advance(nowNs):
//   1. client.beginFrame(); client.pump(now); client.reconcile();
//   2. if the timeline changed: presentation::onRollback() against the frame
//      that was drawn last (same tick, same alpha), so the offset is the
//      correction alone and never the motion of this frame's new ticks;
//   3. for each of client.ticksDue(now) ticks: sample one PlayerInput per
//      local slot over that tick's wall-clock window (tickStartNs(i),
//      tickWallNs(); the batch's last tick takes everything still pending)
//      and client.tick(inputs);
//   4. a timeline change during ticksDue() (a hard clock jump) gets its own
//      onRollback() against the new states;
//   5. frameEvents() = client.drainEvents() (§5.9: deduplicated, abandoned
//      events dropped), never the raw per-tick sink.
// Then build(dt, out) draws renderPrev()/curr() with renderAlpha().
//
// Local devices: the lobby's bound devices, in ascending lobby slot order,
// become local device 0, 1, ...; the server assigns the slots and local
// device i feeds the i-th set bit of localSlots(). requestedSlots is the
// number of devices unless overridden (slots beyond the devices idle); a
// spectator requests none. A pad removed in a match repeats its slot's last
// input until a free device presses a button (as in M2).
//
// Pause, single step, time scale and content tuning do not exist here: the
// server's clock and content rule a networked match.
//
// Rooms (M6a, docs/lobby-protocol.md §3): with NetSessionConfig::lobby set
// the transport asks the lobby to create or join a room before it connects
// (WebRtcTransport's lobby mode) and the session gets an ILobbyJoin view of
// it (attachLobby()). The NetClient starts without a token; on the first
// advance() that sees the room, before pump(), the lobby's token goes into
// the NetClient (setToken), which is before the Hello: the transport only
// reports Connected after the lobby's `joined`. statusText() then names the
// room, and a refusal or a lobby close becomes plain words
// (lobbyErrorText()).
//
// The online room (v0.42, protocol 6, §6.6): NetSessionConfig::nickname
// rides in the Hello; roomState() is the server's newest RoomState (none
// from a server on the M3 flow); phase() is Waiting or Over while the room
// is (the NetClient holds its timeline, nothing is sampled or sent), so the
// app draws the waiting screen or the match-over screen over the last
// world; isHost(), sendSettings() and start() are the host's controls
// (anyone may call them: the server ignores a non-host). slotName() is a
// human slot's nickname, else its label.
#pragma once

#include "kfm/net/net_client.hpp"
#include "kfm/net/transport.hpp"
#include "kfm/platform/input_event.hpp"
#include "kfm/platform/input_sampler.hpp"
#include "kfm/presentation/builder.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/world_state.hpp"
#include "match_session.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kfm::client {

// What the network screen shows.
enum class NetPhase : uint8_t {
  Connecting = 0,  // transport and handshake
  Syncing,         // Welcome received: snapshot and RTT samples
  Running,         // predicting; the match is drawn
  Waiting,         // v0.42: synced, the room waits for its host's Start
  Over,            // v0.42: synced, the room's match ended (the host plays again)
  Rejected,        // the server refused the Hello (rejectReason())
  Disconnected,    // transport closed, timed out, kicked or quit
  Failed,          // the transport could not be created (failure())
};

[[nodiscard]] const char* netPhaseName(NetPhase phase);
[[nodiscard]] const char* rejectReasonText(net::RejectReason reason);
[[nodiscard]] const char* disconnectReasonText(net::DisconnectReason reason);

// A room to create or join through the lobby.
struct LobbyTarget {
  bool create{true};   // create a room in `region`; false: join `code`
  std::string region;  // create: "eu", "us", ...
  std::string code;    // join: upper case, 5 characters
  bool operator==(const LobbyTarget&) const = default;
};

// The room the lobby put the session in.
struct LobbyRoomInfo {
  std::string code;  // "ABCDE"
  std::string region;
  bool operator==(const LobbyRoomInfo&) const = default;
};

// What a lobby-mode transport tells the session: the room once the lobby
// said `joined`, its token, and why it ended (WebRtcTransport's lobbyRoom(),
// connectionToken() and lastError() in the app; a fake in the tests).
class ILobbyJoin {
 public:
  ILobbyJoin() = default;
  ILobbyJoin(const ILobbyJoin&) = delete;
  ILobbyJoin& operator=(const ILobbyJoin&) = delete;
  ILobbyJoin(ILobbyJoin&&) = delete;
  ILobbyJoin& operator=(ILobbyJoin&&) = delete;
  virtual ~ILobbyJoin() = default;
  [[nodiscard]] virtual std::optional<LobbyRoomInfo> room() const = 0;
  [[nodiscard]] virtual std::vector<std::byte> token() const = 0;
  // "lobby refused: <reason>", "lobby closed: <reason>", "lobby connection
  // closed", another transport error, or empty.
  [[nodiscard]] virtual std::string error() const = 0;
};

// A lobby error (ILobbyJoin::error()) in plain words for the status screen:
// "Room ABCDE not found", "Room ABCDE is full", "No server available in eu",
// "Version mismatch: reload the page or update the client", ... Empty when
// `error` is not a lobby error.
[[nodiscard]] std::string lobbyErrorText(std::string_view error, const LobbyTarget& target);

struct NetSessionConfig {
  std::string serverLabel;           // shown in the status ("ws://host:port", the lobby URL)
  std::optional<LobbyTarget> lobby;  // set: a room through the lobby at serverLabel
  uint32_t buildId{0};               // must match the server's (ClientConfig::buildId)
  std::vector<std::byte> token;      // connection token (§6.7), may be empty
  bool spectate{false};              // request no slots
  std::string nickname;              // v0.42: sent in Hello (Nickname::from)
  uint8_t requestedSlots{0};         // 0: one per bound lobby device (at least 1)
  uint8_t inputDelayTicks{0};        // §6.2 fixed input delay
  // The client's loaded content (its hash must match the server's);
  // nullptr = sim::defaultContent(). Copied. The arena and mode come from
  // the server (Welcome / Snapshot / MatchConfig).
  const sim::ContentDB* content{nullptr};
};

// Per-second rates of the cumulative ClientNetStats, over the last full
// second (NetSession::rates()).
struct NetRates {
  double mispredictedTicksPerSec{0.0};
  double rollbacksPerSec{0.0};
  double substitutionsPerSec{0.0};
  double bytesUpPerSec{0.0};
  double bytesDownPerSec{0.0};
  double ticksPerSec{0.0};
};

class NetSession {
 public:
  // Per-frame event capacity (the journal hands out at most this many).
  static constexpr std::size_t kMaxFrameEvents = 2048;

  // `transport` must outlive the session; `owned` (optional) is destroyed
  // with it. A null transport puts the session in Failed with `failure`.
  NetSession(NetSessionConfig config, platform::InputSampler lobbySampler,
             net::ITransport* transport, uint64_t nowNs,
             std::unique_ptr<net::ITransport> owned = nullptr, std::string failure = {});
  ~NetSession();
  NetSession(const NetSession&) = delete;
  NetSession& operator=(const NetSession&) = delete;
  NetSession(NetSession&&) = delete;
  NetSession& operator=(NetSession&&) = delete;

  // --- per frame --------------------------------------------------------------
  void push(const platform::InputEvent& event);
  void advance(uint64_t nowNs);
  // buildRenderState over renderPrev()/curr()/renderAlpha()/frameEvents();
  // remembers what it drew for the next onRollback().
  void build(float dtSeconds, presentation::RenderState& out);

  // Sends Disconnect{Quit}; the phase becomes Disconnected.
  void disconnect();

  // Lobby mode: the transport's view of the lobby (see the header comment).
  // Call before the first advance(); destroyed before the owned transport.
  void attachLobby(std::unique_ptr<ILobbyJoin> lobby) { lobby_ = std::move(lobby); }
  // The room once the lobby placed the session in one (kept after a
  // disconnect, so Start can rejoin it).
  [[nodiscard]] const std::optional<LobbyRoomInfo>& room() const { return room_; }
  // The lobby's refusal or close in plain words, or empty.
  [[nodiscard]] std::string lobbyErrorText() const;

  // --- status -----------------------------------------------------------------
  [[nodiscard]] NetPhase phase() const;
  [[nodiscard]] bool running() const { return phase() == NetPhase::Running; }
  // Synced with a server: Running, Waiting or Over.
  [[nodiscard]] bool synced() const { return client_->running(); }
  // One line for the HUD and the status screen ("Connecting to ws://...").
  [[nodiscard]] std::string statusText() const;
  [[nodiscard]] const std::string& failure() const { return failure_; }
  [[nodiscard]] const NetSessionConfig& config() const { return config_; }
  [[nodiscard]] uint8_t requestedSlots() const { return requested_; }
  // Whether the session ever reached Running (a disconnect then means "lost").
  [[nodiscard]] bool everRan() const { return everRan_; }
  // Whether the server's Welcome arrived, so client().matchConfig() is the
  // server's (arena, mode, power-up set, roster): the online lobby shows it.
  [[nodiscard]] bool configKnown() const { return configKnown_; }

  // --- the room (v0.42) ---------------------------------------------------------
  [[nodiscard]] const std::optional<net::RoomStateMsg>& roomState() const {
    return client_->roomState();
  }
  // Whether one of the local slots is the room's host.
  [[nodiscard]] bool isHost() const;
  // The host's controls: new settings (Waiting), Start (Waiting: start the
  // match; Over: back to Waiting).
  void sendSettings(const net::RoomSettings& settings);
  void start();
  // A human slot's nickname, else "P<n>" ("BOT<n>" for a bot slot).
  [[nodiscard]] std::string slotName(PlayerId slot) const;

  // --- slots ------------------------------------------------------------------
  [[nodiscard]] sim::PlayerMask localSlots() const { return client_->localSlots(); }
  // The server slot local device `device` feeds, or invalid.
  [[nodiscard]] PlayerId slotOfDevice(uint8_t device) const;
  [[nodiscard]] uint8_t deviceCount() const { return static_cast<uint8_t>(devices_.size()); }
  // Local for the local slots, Dropped for a local slot whose pad is gone,
  // Remote for every other present slot, None otherwise.
  [[nodiscard]] SlotOwner owner(PlayerId slot) const;
  [[nodiscard]] sim::PlayerMask droppedMask() const;

  // --- state ------------------------------------------------------------------
  // The world drawn: the predicted world once synced, an empty arena before.
  [[nodiscard]] const sim::WorldState& curr() const;
  [[nodiscard]] const sim::WorldState& renderPrev() const;
  [[nodiscard]] float renderAlpha() const;
  [[nodiscard]] bool rendersNewest() const;
  void setInterpolationPolicy(InterpolationPolicy p) { policy_ = p; }
  [[nodiscard]] InterpolationPolicy interpolationPolicy() const { return policy_; }
  [[nodiscard]] double displayHz() const;

  [[nodiscard]] std::span<const sim::SimEvent> frameEvents() const {
    return {frameEvents_.data(), frameEventCount_};
  }
  [[nodiscard]] uint32_t ticksThisFrame() const { return ticksThisFrame_; }
  // Frames whose timeline changed (onRollback() applied) since the start.
  [[nodiscard]] uint64_t rollbackFrames() const { return rollbackFrames_; }

  [[nodiscard]] presentation::PresentationState& presentation() { return presentation_; }
  [[nodiscard]] const presentation::PresentationState& presentation() const {
    return presentation_;
  }
  [[nodiscard]] const sim::ContentDB& content() const { return content_; }
  // The content with the arena and mode of the match the server runs.
  [[nodiscard]] sim::MatchContent matchContent() const { return client_->matchContent(); }
  [[nodiscard]] uint32_t checksum() const;

  [[nodiscard]] const net::NetClient& client() const { return *client_; }
  [[nodiscard]] net::NetClient& client() { return *client_; }
  [[nodiscard]] const NetRates& rates() const { return rates_; }
  [[nodiscard]] uint64_t nowNs() const { return nowNs_; }

  [[nodiscard]] platform::InputSampler& sampler() { return sampler_; }
  [[nodiscard]] const platform::InputSampler& sampler() const { return sampler_; }
  // The sampler to hand back to the local lobby: current key/pad state with
  // the lobby's bindings restored and every held button consumed.
  [[nodiscard]] platform::InputSampler lobbySampler() const;

 private:
  struct LobbyDevice {
    PlayerId lobbySlot{};
    platform::DeviceBinding device{};
  };

  void bindLocalSlots();
  void handleRebinds();
  void stepTicks(uint32_t ticks);
  void updateRates(uint64_t nowNs);
  void updateDisplayRate(uint64_t nowNs);

  NetSessionConfig config_;
  std::unique_ptr<ILobbyJoin> lobby_;
  std::optional<LobbyRoomInfo> room_;
  uint8_t requested_{1};
  sim::ContentDB content_{};
  std::unique_ptr<net::ITransport> ownedTransport_;
  std::unique_ptr<net::NetClient> client_;
  std::string failure_;
  bool everRan_{false};
  bool configKnown_{false};
  bool slotsBound_{false};
  uint64_t nowNs_{0};

  std::vector<LobbyDevice> devices_;
  platform::InputSampler sampler_{};
  sim::PlayerMask boundLocal_{0};  // server slots bound to a device
  std::array<sim::PlayerInput, sim::kMaxPlayers> lastInputs_{};

  sim::WorldState idleWorld_{};  // drawn until the first snapshot
  presentation::PresentationState presentation_{};
  InterpolationPolicy policy_{InterpolationPolicy::Auto};
  float lastAlpha_{1.0f};
  bool lastNewest_{true};
  uint64_t lastFrameNs_{0};
  double frameNsEma_{0.0};

  std::vector<sim::SimEvent> frameEvents_;
  std::size_t frameEventCount_{0};
  uint32_t ticksThisFrame_{0};
  uint64_t rollbackFrames_{0};

  NetRates rates_{};
  uint64_t rateWindowStartNs_{0};
  net::ClientNetStats rateBase_{};
};

}  // namespace kfm::client
