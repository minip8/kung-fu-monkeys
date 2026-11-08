// ReplaySession: the client's replay viewer (M7 step 6). SDL-free and
// unit-tested headlessly against the replay corpus.
//
// A replay (.kfmr, kfm/sim/replay.hpp) is the seed, the MatchConfig, the
// content and build hashes and one InputFrame per tick. Playing it back is
// deterministic: initialWorld(header.matchConfig()) stepped over the
// recorded frames with the content whose hash the header names. So the
// viewer needs no bot runtime and no network: the bots' and the humans'
// inputs are all in the frames, and the browser plays a replay like the
// native client.
//
// load() decodes the header and every frame up front (heap at load is fine
// here: the no-heap rule is the sim's) and refuses, with a reason
// (errorKind(), error()), a file that is not a replay of this version, has
// truncated or malformed frames, was recorded with other content (the
// viewer is the client of the replay's own release: a hash mismatch is
// refused, never played approximately), names an arena, mode or loadout the
// content does not have, or has no ticks. The build hash is not checked
// (every build writes 0 today); header() shows it.
//
// The playback surface mirrors MatchSession and NetSession so the app's
// render path draws it unchanged: per frame advance(nowNs), then build the
// RenderState from renderPrev(), curr(), renderAlpha(), frameEvents() with
// presentation() and presentationSeconds(dt).
//
// Controls: pause and single step (setPaused, requestSingleStep), a speed
// from kReplaySpeeds (setSpeed, the clock's time scale), restart() and
// seek(tick). A seek is synchronous: forward it steps from the current
// tick, backward it re-creates the initial world and steps from tick 0
// (deterministic, and cheap at tens of thousands of ticks a second). A seek
// shows the landed state as a still frame: it produces no frameEvents() and
// resets presentation(), so a jump never sprays effects or sounds.
//
// At the end (atEnd(): every recorded frame stepped) the session holds the
// final state and advance() steps nothing; paused() does not change.
// restart() plays it again.
//
// Replays carry no nicknames or team names: the UI labels slots itself
// (matchConfig().teams is the team of every slot as recorded).
#pragma once

#include "fixed_step_clock.hpp"
#include "kfm/presentation/builder.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/replay.hpp"
#include "kfm/sim/world_state.hpp"
#include "match_session.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace kfm::client {

// The playback speeds, slowest first (setSpeed snaps to the nearest).
inline constexpr std::array<double, 5> kReplaySpeeds{0.25, 0.5, 1.0, 2.0, 4.0};
inline constexpr double kDefaultReplaySpeed = 1.0;

// Ticks one advance() may step: 4x on a 30 Hz display. (Local play caps at
// kDefaultMaxTicksPerFrame; a replay at 4x needs more headroom.)
inline constexpr uint32_t kReplayMaxTicksPerFrame = 8;

// Why load() refused a replay.
enum class ReplayError : uint8_t {
  None = 0,
  BadHeader,        // not a KFMR file of this version (magic, version, short header)
  BadFrames,        // a frame is truncated or malformed, or out of tick order
  ContentMismatch,  // recorded with other content (header contentHash != this content's)
  BadMatchConfig,   // its arena, mode or loadout index is not in the content
  NoTicks,          // a header with tickCount 0: nothing to play
};

class ReplaySession {
 public:
  // Per-frame event accumulator capacity (kReplayMaxTicksPerFrame ticks of a
  // full EventSink); further events are counted and dropped.
  static constexpr std::size_t kMaxFrameEvents =
      static_cast<std::size_t>(sim::kMaxEventsPerTick) * kReplayMaxTicksPerFrame;

  ReplaySession();

  // --- loading ----------------------------------------------------------------
  // Decodes `bytes` (a whole .kfmr) against `content` (copied: the session
  // owns its content). On success the session plays from tick 0 at speed
  // 1, unpaused, with the clock started at nowNs, and error() is empty. On
  // failure it returns false, holds no replay (loaded() false, prev()/curr()
  // default states) and error() says why in plain words (a content mismatch
  // names both hashes in hex).
  bool load(std::span<const std::byte> bytes, const sim::ContentDB& content, uint64_t nowNs);
  [[nodiscard]] bool loaded() const { return loaded_; }
  [[nodiscard]] ReplayError errorKind() const { return errorKind_; }
  [[nodiscard]] const std::string& error() const { return error_; }
  // The decoded header (seed, hashes, players, arena, mode, power-ups,
  // loadout, teams, tickCount); default before a successful load.
  [[nodiscard]] const sim::ReplayHeader& header() const { return header_; }

  // --- per frame --------------------------------------------------------------
  // Steps the ticks the clock hands out (0..kReplayMaxTicksPerFrame; one
  // after requestSingleStep() while paused), never past the last recorded
  // frame. Clears frameEvents() first.
  void advance(uint64_t nowNs);

  // --- controls ---------------------------------------------------------------
  void setPaused(bool paused) { clock_.setPaused(paused); }
  [[nodiscard]] bool paused() const { return clock_.paused(); }
  // While paused, the next advance() steps exactly one tick (none at the end).
  void requestSingleStep() { clock_.requestStep(); }
  // Snaps to the nearest of kReplaySpeeds and sets the clock's time scale.
  void setSpeed(double speed);
  [[nodiscard]] double speed() const { return speed_; }
  // The index of speed() in kReplaySpeeds.
  [[nodiscard]] std::size_t speedIndex() const;
  // Back to tick 0 and playing (unpaused); speed is kept. Like seek(0).
  void restart(uint64_t nowNs);
  // Jumps to `tick` (clamped to tickCount()) synchronously; pause and speed
  // are kept, a pending single step is dropped and the clock restarts at
  // nowNs. No frameEvents(), and presentation() is reset; prev() = curr().
  void seek(uint32_t tick, uint64_t nowNs);

  // Ticks stepped so far: the index of curr() (0 = the initial world).
  [[nodiscard]] uint32_t tick() const { return tick_; }
  // The recorded ticks (header().tickCount); curr() at tickCount() is the
  // final state.
  [[nodiscard]] uint32_t tickCount() const { return static_cast<uint32_t>(frames_.size()); }
  [[nodiscard]] bool atEnd() const { return tick_ >= tickCount(); }

  // --- state ------------------------------------------------------------------
  [[nodiscard]] const sim::WorldState& prev() const { return prev_; }
  [[nodiscard]] const sim::WorldState& curr() const { return curr_; }
  // sim::checksum(curr()): the replay's per-tick checksum at tick().
  [[nodiscard]] uint32_t checksum() const;
  // The frame the last step ran (default before the first).
  [[nodiscard]] const sim::InputFrame& lastFrame() const { return lastFrame_; }

  // What the builder draws this frame, per the interpolation policy (as
  // MatchSession). At the end, and after a seek until the next step, the
  // newest state alone (a still frame).
  [[nodiscard]] const sim::WorldState& renderPrev() const;
  [[nodiscard]] float renderAlpha() const;
  [[nodiscard]] bool rendersNewest() const;
  void setInterpolationPolicy(InterpolationPolicy p) { policy_ = p; }
  [[nodiscard]] InterpolationPolicy interpolationPolicy() const { return policy_; }

  // Events of every tick stepped by the last advance(), each exactly once.
  [[nodiscard]] std::span<const sim::SimEvent> frameEvents() const {
    return {frameEvents_.data(), frameEventCount_};
  }
  [[nodiscard]] uint64_t droppedEvents() const { return droppedEvents_; }
  [[nodiscard]] uint32_t ticksThisFrame() const { return ticksThisFrame_; }
  // As MatchSession: all of the frame's wall time while playing, and while
  // paused only the ticks a single step ran, so a paused replay draws a
  // still frame.
  [[nodiscard]] double presentationSeconds(double wallSeconds) const;

  [[nodiscard]] presentation::PresentationState& presentation() { return presentation_; }

  // The config the replay was recorded with (header().matchConfig()), its
  // content, and the content step() runs it with.
  [[nodiscard]] const sim::MatchConfig& matchConfig() const { return matchConfig_; }
  [[nodiscard]] const sim::ContentDB& content() const { return content_; }
  [[nodiscard]] sim::MatchContent matchContent() const {
    return sim::matchContent(content_, matchConfig_);
  }

  [[nodiscard]] FixedStepClock& clock() { return clock_; }
  [[nodiscard]] const FixedStepClock& clock() const { return clock_; }

 private:
  void fail(ReplayError kind, std::string message);
  void resetWorld();
  void stepOnce(sim::EventSink& sink);
  void appendEvents(const sim::EventSink& sink);

  bool loaded_{false};
  ReplayError errorKind_{ReplayError::None};
  std::string error_;
  sim::ReplayHeader header_{};
  std::vector<sim::InputFrame> frames_;

  sim::ContentDB content_{};
  sim::MatchConfig matchConfig_{};
  sim::WorldState prev_{};
  sim::WorldState curr_{};
  sim::EventSink sink_{};
  sim::InputFrame lastFrame_{};
  uint32_t tick_{0};
  bool still_{true};  // draw curr() alone (loaded or seeked, nothing stepped since)
  double speed_{kDefaultReplaySpeed};

  std::array<sim::SimEvent, kMaxFrameEvents> frameEvents_{};
  std::size_t frameEventCount_{0};
  uint64_t droppedEvents_{0};
  uint32_t ticksThisFrame_{0};

  presentation::PresentationState presentation_{};
  FixedStepClock clock_{};
  InterpolationPolicy policy_{InterpolationPolicy::Auto};
};

// Reads a whole file (a native .kfmr path; the browser fetches its URL in
// main.cpp instead). False with `error` set ("cannot open PATH", ...) when
// it cannot be read.
[[nodiscard]] bool readFileBytes(const std::string& path, std::vector<std::byte>& out,
                                 std::string& error);

}  // namespace kfm::client
