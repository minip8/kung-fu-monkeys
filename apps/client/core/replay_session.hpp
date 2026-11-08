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
// seek(tick). A seek is synchronous and deterministic. Every
// kReplayKeyframeTicks ticks the session keeps a keyframe, a copy of the
// world (WorldState is trivially copyable: the copy is the state straight
// play reaches), recorded the first time playback or a seek steps onto
// that tick. A seek starts from the newest keyframe at or before its
// target when the target is behind the current tick or that keyframe is
// ahead of it, else from the current tick (from the initial world when a
// backward seek has no keyframe); so once a span has been played or sought
// through, a seek into it steps fewer than kReplayKeyframeTicks ticks
// (lastSeekSteps()), which keeps a drag on the timeline smooth over a long
// replay. A seek shows the landed state as a still frame: it produces no
// frameEvents() and resets presentation(), so a jump never sprays effects
// or sounds.
//
// Drawing: the newest state alone (renderPrev() = curr(), alpha 1) while
// still (loaded or sought, nothing stepped since), at the end and while
// paused (a single step shows the tick it reached). Otherwise per the
// interpolation policy; Auto interpolates below 1x (the ticks come slower
// than the display: holding each would show a 0.25x replay at 15 fps) and
// from 1x up follows MatchSession (the newest tick when the display keeps
// up with the scaled tick rate).
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
#include <memory>
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

// A keyframe every 10 s of replay: a 16 MiB replay (~300 k ticks) keeps at
// most ~500 of them, a few MB (sizeof(sim::WorldState) each).
inline constexpr uint32_t kReplayKeyframeTicks = 10 * sim::kTickHz;

// The largest replay the viewer takes: readFileBytes refuses a bigger file,
// the browser's download is cut off there.
inline constexpr std::size_t kReplayMaxBytes = std::size_t{16} << 20;

// Why load() refused a replay.
enum class ReplayError : uint8_t {
  None = 0,
  BadHeader,  // not a KFMR file of this version (magic, version, short header)
  BadFrames,  // truncated (fewer bytes than tickCount frames need), malformed, or out of tick order
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
  // Jumps to `tick` (clamped to tickCount()) synchronously, from a keyframe
  // when one helps (header comment); pause and speed are kept, a pending
  // single step is dropped and the clock restarts at nowNs. No
  // frameEvents(), and presentation() is reset; prev() = curr().
  void seek(uint32_t tick, uint64_t nowNs);
  // The ticks the last seek() (or restart()) stepped.
  [[nodiscard]] uint32_t lastSeekSteps() const { return lastSeekSteps_; }
  // The keyframes recorded so far (tick 0, the initial world, is never one).
  [[nodiscard]] std::size_t keyframeCount() const;

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

  // What the builder draws this frame (the header comment's drawing rule):
  // the newest state alone while still, at the end or paused, else per the
  // interpolation policy.
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
  // The newest keyframe's tick at or before `tick` (0 when none is).
  [[nodiscard]] uint32_t keyframeAtOrBefore(uint32_t tick) const;
  // curr() at a keyframe's tick (resetWorld() for 0).
  void restoreKeyframe(uint32_t tick);
  void stepOnce(sim::EventSink& sink);
  void appendEvents(const sim::EventSink& sink);

  bool loaded_{false};
  ReplayError errorKind_{ReplayError::None};
  std::string error_;
  sim::ReplayHeader header_{};
  std::vector<sim::InputFrame> frames_;
  // Slot k is the world at tick k * kReplayKeyframeTicks once recorded
  // (slot 0 stays empty: initialWorld() rebuilds tick 0).
  std::vector<std::unique_ptr<sim::WorldState>> keyframes_;
  uint32_t lastSeekSteps_{0};

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
// main.cpp instead) of at most `maxBytes`. False with `error` set ("cannot
// open PATH", "PATH is larger than 16 MiB", ...) when it cannot be read or
// is too large (refused from its size, before reading).
[[nodiscard]] bool readFileBytes(const std::string& path, std::vector<std::byte>& out,
                                 std::string& error, std::size_t maxBytes = kReplayMaxBytes);

}  // namespace kfm::client
