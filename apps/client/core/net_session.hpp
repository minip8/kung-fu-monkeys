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
#include <span>
#include <string>
#include <vector>

namespace kfm::client {

// What the network screen shows.
enum class NetPhase : uint8_t {
  Connecting = 0,  // transport and handshake
  Syncing,         // Welcome received: snapshot and RTT samples
  Running,         // predicting; the match is drawn
  Rejected,        // the server refused the Hello (rejectReason())
  Disconnected,    // transport closed, timed out, kicked or quit
  Failed,          // the transport could not be created (failure())
};

[[nodiscard]] const char* netPhaseName(NetPhase phase);
[[nodiscard]] const char* rejectReasonText(net::RejectReason reason);
[[nodiscard]] const char* disconnectReasonText(net::DisconnectReason reason);

struct NetSessionConfig {
  std::string serverLabel;       // shown in the status ("ws://host:port")
  uint32_t buildId{0};           // must match the server's (ClientConfig::buildId)
  std::vector<std::byte> token;  // connection token (§6.7), may be empty
  bool spectate{false};          // request no slots
  uint8_t requestedSlots{0};     // 0: one per bound lobby device (at least 1)
  uint8_t inputDelayTicks{0};    // §6.2 fixed input delay
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

  // --- status -----------------------------------------------------------------
  [[nodiscard]] NetPhase phase() const;
  [[nodiscard]] bool running() const { return phase() == NetPhase::Running; }
  // One line for the HUD and the status screen ("Connecting to ws://...").
  [[nodiscard]] std::string statusText() const;
  [[nodiscard]] const std::string& failure() const { return failure_; }
  [[nodiscard]] const NetSessionConfig& config() const { return config_; }
  [[nodiscard]] uint8_t requestedSlots() const { return requested_; }
  // Whether the session ever reached Running (a disconnect then means "lost").
  [[nodiscard]] bool everRan() const { return everRan_; }

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
  uint8_t requested_{1};
  sim::ContentDB content_{};
  std::unique_ptr<net::ITransport> ownedTransport_;
  std::unique_ptr<net::NetClient> client_;
  std::string failure_;
  bool everRan_{false};
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
