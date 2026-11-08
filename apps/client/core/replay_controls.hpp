// The replay viewer's control bar as a headless model (M7 step 6): which
// control has the keyboard / pad focus, what a key or pad press asks for,
// what a click on a control asks for, the timeline's scrub and the labels.
// SDL- and ImGui-free; hud.cpp draws the bar and main.cpp applies the
// ReplayCommands to its ReplaySession.
//
// The bar, left to right: Restart, Play / Pause (at the end "Watch again",
// which restarts), Step (one tick, only while paused and before the end),
// Speed ("1x"; a click cycles kReplaySpeeds, wrapping), the timeline
// (0..tickCount; a click or a drag seeks), the time "m:ss / m:ss" (60 ticks
// a second), Main menu.
//
// Keys (translate()):
//   Space            play / pause (at the end: watch again)
//   .                one tick (pauses first while playing)
//   Left / A         back 5 s, Right / D forward 5 s; with Shift 30 s
//   [ / ]            slower / faster (clamped at 0.25x and 4x)
//   Home             restart
//   Escape           main menu
//   Up / W, Down / S, Tab (Shift+Tab back)   move the focus over the bar
//   Enter            the focused control, as a click
// Pads: South the focused control, East / Back main menu, Start play /
// pause, West one tick, d-pad Left / Right seek 5 s, d-pad Up / Down the
// focus, the shoulders slower / faster.
//
// The menu-highlight rule (menu_highlight.hpp) covers the focus keys and
// Enter / South only (isMenuPress()): the shortcuts act at once.
#pragma once

#include "kfm/platform/input_event.hpp"
#include "kfm/sim/constants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace kfm::client {

inline constexpr uint32_t kReplaySeekShortTicks = 5 * sim::kTickHz;  // Left / Right
inline constexpr uint32_t kReplaySeekLongTicks = 30 * sim::kTickHz;  // with Shift

enum class ReplayControl : uint8_t { Restart = 0, PlayPause, Step, Speed, Timeline, MainMenu };

// What the bar shows of the session (copied from ReplaySession each frame).
struct ReplayStatus {
  uint32_t tick{0};
  uint32_t tickCount{0};
  bool paused{false};
  bool atEnd{false};
  std::size_t speedIndex{2};  // into kReplaySpeeds (1x)
};

// What the bar asks of the session.
struct ReplayCommand {
  enum class Kind : uint8_t {
    None = 0,
    TogglePause,  // ReplaySession::setPaused(!paused())
    Step,         // setPaused(true), then requestSingleStep()
    SeekTo,       // seek(tick)
    SetSpeed,     // setSpeed(kReplaySpeeds[speedIndex])
    Restart,      // restart()
    MainMenu,     // leave the viewer for the main menu
  };
  Kind kind{Kind::None};
  uint32_t tick{0};           // SeekTo: within 0..tickCount
  std::size_t speedIndex{0};  // SetSpeed
  bool operator==(const ReplayCommand&) const = default;
};

// A press as the bar reads it (ReplayControls::translate()).
enum class ReplayKey : uint8_t {
  None = 0,
  PlayPause,
  Step,
  SeekBack,
  SeekForward,
  SeekBackLong,
  SeekForwardLong,
  SpeedDown,
  SpeedUp,
  Restart,
  MainMenu,
  FocusPrev,
  FocusNext,
  Activate,  // the focused control
};

class ReplayControls {
 public:
  [[nodiscard]] static std::span<const ReplayControl> controls();

  // The key or pad press an event is (None for anything else); Shift's
  // state is tracked from its own key events.
  [[nodiscard]] ReplayKey translate(const platform::InputEvent& event);
  // FocusPrev / FocusNext move the focus over the enabled controls
  // (wrapping) and ask for nothing; Activate is activate(focused).
  [[nodiscard]] ReplayCommand apply(ReplayKey key, const ReplayStatus& status);
  // A click on `control` (or Enter on it); a disabled control asks nothing.
  [[nodiscard]] static ReplayCommand activate(ReplayControl control, const ReplayStatus& status);
  [[nodiscard]] static bool enabled(ReplayControl control, const ReplayStatus& status);
  // The focus keys and Enter / South: the presses the highlight's reveal
  // rule may drop (MenuHighlight::reveal).
  [[nodiscard]] static bool isMenuPress(ReplayKey key) {
    return key == ReplayKey::FocusPrev || key == ReplayKey::FocusNext || key == ReplayKey::Activate;
  }

  [[nodiscard]] ReplayControl focus() const { return focus_; }
  void setFocus(ReplayControl control) { focus_ = control; }

  // The timeline under the pointer: scrubTo() returns the seek a drag asks
  // for (none while the tick under the pointer is the one already sought),
  // endScrub() the final seek at the release (the session's clock restarts
  // there). While scrubbing() the app holds the playback.
  [[nodiscard]] std::optional<ReplayCommand> scrubTo(uint32_t tick);
  [[nodiscard]] std::optional<ReplayCommand> endScrub();
  [[nodiscard]] bool scrubbing() const { return scrub_.has_value(); }

  // The button's text: "Restart", "Pause" / "Play" / "Watch again",
  // "Step", the speed ("1x"), "" for the timeline, "Main menu".
  [[nodiscard]] static const char* label(ReplayControl control, const ReplayStatus& status);

 private:
  ReplayControl focus_{ReplayControl::PlayPause};
  bool shift_{false};
  std::optional<uint32_t> scrub_;
};

// "m:ss" of `ticks` at 60 a second (whole seconds, rounded down; minutes
// past 59 keep counting: "75:00").
[[nodiscard]] std::string replayClock(uint32_t ticks);
// The bar's time: "0:12 / 0:30".
[[nodiscard]] std::string replayTimeLabel(uint32_t tick, uint32_t tickCount);
// kReplaySpeeds[index] as "0.25x", "0.5x", "1x", "2x", "4x".
[[nodiscard]] std::string replaySpeedLabel(std::size_t index);
// `tick` moved by `delta` ticks, within 0..tickCount.
[[nodiscard]] uint32_t replaySeekTarget(uint32_t tick, int64_t delta, uint32_t tickCount);
// The tick at `fraction` (0..1, clamped) along the timeline, rounded to the
// nearest.
[[nodiscard]] uint32_t replayTickAt(float fraction, uint32_t tickCount);

}  // namespace kfm::client
