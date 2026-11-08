// The replay viewer's control bar (M7 step 6), headless: the key and pad
// mapping, the focus, what a click asks for, the scrub and the labels.
#include "core/replay_controls.hpp"

#include "core/replay_session.hpp"
#include "kfm/platform/input_event.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

using namespace kfm;
using client::ReplayCommand;
using client::ReplayControl;
using client::ReplayControls;
using client::ReplayKey;
using client::ReplayStatus;
using platform::InputEvent;
using platform::InputEventKind;
using platform::Key;
using platform::PadButton;
using Kind = ReplayCommand::Kind;

namespace {

InputEvent keyDown(Key k) {
  return InputEvent{.kind = InputEventKind::KeyDown, .key = k};
}
InputEvent keyUp(Key k) {
  return InputEvent{.kind = InputEventKind::KeyUp, .key = k};
}
InputEvent pad(PadButton b) {
  return InputEvent{.kind = InputEventKind::PadButtonDown, .deviceId = 7, .button = b};
}

// 30 s of replay, 10 s in, playing at 1x.
constexpr ReplayStatus kPlaying{
    .tick = 600, .tickCount = 1800, .paused = false, .atEnd = false, .speedIndex = 2};

}  // namespace

TEST_CASE("replay controls: keys", "[client][replay]") {
  ReplayControls c;
  CHECK(c.translate(keyDown(Key::Space)) == ReplayKey::PlayPause);
  CHECK(c.translate(keyDown(Key::Period)) == ReplayKey::Step);
  CHECK(c.translate(keyDown(Key::Left)) == ReplayKey::SeekBack);
  CHECK(c.translate(keyDown(Key::Right)) == ReplayKey::SeekForward);
  CHECK(c.translate(keyDown(Key::A)) == ReplayKey::SeekBack);
  CHECK(c.translate(keyDown(Key::D)) == ReplayKey::SeekForward);
  CHECK(c.translate(keyDown(Key::LeftBracket)) == ReplayKey::SpeedDown);
  CHECK(c.translate(keyDown(Key::RightBracket)) == ReplayKey::SpeedUp);
  CHECK(c.translate(keyDown(Key::Home)) == ReplayKey::Restart);
  CHECK(c.translate(keyDown(Key::Escape)) == ReplayKey::MainMenu);
  CHECK(c.translate(keyDown(Key::Up)) == ReplayKey::FocusPrev);
  CHECK(c.translate(keyDown(Key::Down)) == ReplayKey::FocusNext);
  CHECK(c.translate(keyDown(Key::Tab)) == ReplayKey::FocusNext);
  CHECK(c.translate(keyDown(Key::Enter)) == ReplayKey::Activate);
  CHECK(c.translate(keyDown(Key::KpEnter)) == ReplayKey::Activate);
  CHECK(c.translate(keyDown(Key::J)) == ReplayKey::None);  // game keys do nothing
  CHECK(c.translate(keyUp(Key::Space)) == ReplayKey::None);

  SECTION("Shift makes the seek 30 s and Tab go back, until it is released") {
    CHECK(c.translate(keyDown(Key::LeftShift)) == ReplayKey::None);
    CHECK(c.translate(keyDown(Key::Left)) == ReplayKey::SeekBackLong);
    CHECK(c.translate(keyDown(Key::Right)) == ReplayKey::SeekForwardLong);
    CHECK(c.translate(keyDown(Key::Tab)) == ReplayKey::FocusPrev);
    CHECK(c.translate(keyUp(Key::LeftShift)) == ReplayKey::None);
    CHECK(c.translate(keyDown(Key::Right)) == ReplayKey::SeekForward);
    (void)c.translate(keyDown(Key::RightShift));
    CHECK(c.translate(keyDown(Key::Right)) == ReplayKey::SeekForwardLong);
  }
}

TEST_CASE("replay controls: pads", "[client][replay]") {
  ReplayControls c;
  CHECK(c.translate(pad(PadButton::South)) == ReplayKey::Activate);
  CHECK(c.translate(pad(PadButton::East)) == ReplayKey::MainMenu);
  CHECK(c.translate(pad(PadButton::Back)) == ReplayKey::MainMenu);
  CHECK(c.translate(pad(PadButton::Start)) == ReplayKey::PlayPause);
  CHECK(c.translate(pad(PadButton::West)) == ReplayKey::Step);
  CHECK(c.translate(pad(PadButton::DpadLeft)) == ReplayKey::SeekBack);
  CHECK(c.translate(pad(PadButton::DpadRight)) == ReplayKey::SeekForward);
  CHECK(c.translate(pad(PadButton::DpadUp)) == ReplayKey::FocusPrev);
  CHECK(c.translate(pad(PadButton::DpadDown)) == ReplayKey::FocusNext);
  CHECK(c.translate(pad(PadButton::LeftShoulder)) == ReplayKey::SpeedDown);
  CHECK(c.translate(pad(PadButton::RightShoulder)) == ReplayKey::SpeedUp);
  CHECK(c.translate(pad(PadButton::North)) == ReplayKey::None);
  InputEvent axis{.kind = InputEventKind::PadAxis, .deviceId = 7};
  CHECK(c.translate(axis) == ReplayKey::None);
}

TEST_CASE("replay controls: what a press asks for", "[client][replay]") {
  ReplayControls c;
  CHECK(c.apply(ReplayKey::PlayPause, kPlaying) == ReplayCommand{.kind = Kind::TogglePause});
  CHECK(c.apply(ReplayKey::Step, kPlaying) == ReplayCommand{.kind = Kind::Step});
  CHECK(c.apply(ReplayKey::SeekBack, kPlaying) == ReplayCommand{.kind = Kind::SeekTo, .tick = 300});
  CHECK(c.apply(ReplayKey::SeekForward, kPlaying) ==
        ReplayCommand{.kind = Kind::SeekTo, .tick = 900});
  // 30 s jumps stop at the ends; a jump that lands where it is asks nothing.
  CHECK(c.apply(ReplayKey::SeekBackLong, kPlaying) ==
        ReplayCommand{.kind = Kind::SeekTo, .tick = 0});
  CHECK(c.apply(ReplayKey::SeekForwardLong, kPlaying) ==
        ReplayCommand{.kind = Kind::SeekTo, .tick = 1800});
  ReplayStatus start = kPlaying;
  start.tick = 0;
  CHECK(c.apply(ReplayKey::SeekBack, start) == ReplayCommand{});
  CHECK(c.apply(ReplayKey::SpeedDown, kPlaying) ==
        ReplayCommand{.kind = Kind::SetSpeed, .speedIndex = 1});
  CHECK(c.apply(ReplayKey::SpeedUp, kPlaying) ==
        ReplayCommand{.kind = Kind::SetSpeed, .speedIndex = 3});
  ReplayStatus slowest = kPlaying;
  slowest.speedIndex = 0;
  CHECK(c.apply(ReplayKey::SpeedDown, slowest) == ReplayCommand{});
  ReplayStatus fastest = kPlaying;
  fastest.speedIndex = client::kReplaySpeeds.size() - 1;
  CHECK(c.apply(ReplayKey::SpeedUp, fastest) == ReplayCommand{});
  CHECK(c.apply(ReplayKey::Restart, kPlaying) == ReplayCommand{.kind = Kind::Restart});
  CHECK(c.apply(ReplayKey::MainMenu, kPlaying) == ReplayCommand{.kind = Kind::MainMenu});
  CHECK(c.apply(ReplayKey::None, kPlaying) == ReplayCommand{});

  SECTION("at the end, play is watch again and step does nothing") {
    ReplayStatus end = kPlaying;
    end.tick = end.tickCount;
    end.atEnd = true;
    CHECK(c.apply(ReplayKey::PlayPause, end) == ReplayCommand{.kind = Kind::Restart});
    CHECK(c.apply(ReplayKey::Step, end) == ReplayCommand{});
    CHECK(ReplayControls::label(ReplayControl::PlayPause, end) == std::string("Watch again"));
    CHECK(c.apply(ReplayKey::SeekBack, end) == ReplayCommand{.kind = Kind::SeekTo, .tick = 1500});
  }
}

TEST_CASE("replay controls: buttons", "[client][replay]") {
  ReplayStatus paused = kPlaying;
  paused.paused = true;
  CHECK(ReplayControls::label(ReplayControl::PlayPause, kPlaying) == std::string("Pause"));
  CHECK(ReplayControls::label(ReplayControl::PlayPause, paused) == std::string("Play"));
  CHECK(ReplayControls::label(ReplayControl::Speed, kPlaying) == std::string("1x"));
  CHECK(ReplayControls::label(ReplayControl::Restart, kPlaying) == std::string("Restart"));
  CHECK(ReplayControls::label(ReplayControl::MainMenu, kPlaying) == std::string("Main menu"));
  // Step is a paused replay's.
  CHECK_FALSE(ReplayControls::enabled(ReplayControl::Step, kPlaying));
  CHECK(ReplayControls::enabled(ReplayControl::Step, paused));
  CHECK(ReplayControls::activate(ReplayControl::Step, kPlaying) == ReplayCommand{});
  CHECK(ReplayControls::activate(ReplayControl::Step, paused) == ReplayCommand{.kind = Kind::Step});
  CHECK(ReplayControls::activate(ReplayControl::PlayPause, paused) ==
        ReplayCommand{.kind = Kind::TogglePause});
  CHECK(ReplayControls::activate(ReplayControl::Restart, paused) ==
        ReplayCommand{.kind = Kind::Restart});
  CHECK(ReplayControls::activate(ReplayControl::MainMenu, paused) ==
        ReplayCommand{.kind = Kind::MainMenu});
  CHECK(ReplayControls::activate(ReplayControl::Timeline, paused) == ReplayCommand{});
  // The speed button cycles, wrapping from 4x to 0.25x.
  CHECK(ReplayControls::activate(ReplayControl::Speed, kPlaying) ==
        ReplayCommand{.kind = Kind::SetSpeed, .speedIndex = 3});
  ReplayStatus fastest = kPlaying;
  fastest.speedIndex = 4;
  CHECK(ReplayControls::activate(ReplayControl::Speed, fastest) ==
        ReplayCommand{.kind = Kind::SetSpeed, .speedIndex = 0});
}

TEST_CASE("replay controls: the focus skips a disabled step and Enter activates it",
          "[client][replay]") {
  ReplayControls c;
  CHECK(c.focus() == ReplayControl::PlayPause);
  CHECK(c.apply(ReplayKey::FocusNext, kPlaying) == ReplayCommand{});
  CHECK(c.focus() == ReplayControl::Speed);  // Step is disabled while playing
  CHECK(c.apply(ReplayKey::Activate, kPlaying) ==
        ReplayCommand{.kind = Kind::SetSpeed, .speedIndex = 3});
  (void)c.apply(ReplayKey::FocusNext, kPlaying);
  CHECK(c.focus() == ReplayControl::Timeline);
  (void)c.apply(ReplayKey::FocusNext, kPlaying);
  CHECK(c.focus() == ReplayControl::MainMenu);
  (void)c.apply(ReplayKey::FocusNext, kPlaying);
  CHECK(c.focus() == ReplayControl::Restart);  // wraps
  (void)c.apply(ReplayKey::FocusPrev, kPlaying);
  CHECK(c.focus() == ReplayControl::MainMenu);
  CHECK(c.apply(ReplayKey::Activate, kPlaying) == ReplayCommand{.kind = Kind::MainMenu});

  ReplayStatus paused = kPlaying;
  paused.paused = true;
  c.setFocus(ReplayControl::PlayPause);
  (void)c.apply(ReplayKey::FocusNext, paused);
  CHECK(c.focus() == ReplayControl::Step);
  CHECK(c.apply(ReplayKey::Activate, paused) == ReplayCommand{.kind = Kind::Step});

  CHECK(ReplayControls::isMenuPress(ReplayKey::FocusNext));
  CHECK(ReplayControls::isMenuPress(ReplayKey::Activate));
  CHECK_FALSE(ReplayControls::isMenuPress(ReplayKey::PlayPause));
  CHECK_FALSE(ReplayControls::isMenuPress(ReplayKey::SeekBack));
}

TEST_CASE("replay controls: the timeline scrub seeks on change and once more at release",
          "[client][replay]") {
  ReplayControls c;
  CHECK_FALSE(c.scrubbing());
  CHECK_FALSE(c.endScrub().has_value());
  CHECK(c.scrubTo(100) == ReplayCommand{.kind = Kind::SeekTo, .tick = 100});
  CHECK(c.scrubbing());
  CHECK_FALSE(c.scrubTo(100).has_value());  // the pointer rests: no re-seek
  CHECK(c.scrubTo(40) == ReplayCommand{.kind = Kind::SeekTo, .tick = 40});
  CHECK(c.endScrub() == ReplayCommand{.kind = Kind::SeekTo, .tick = 40});
  CHECK_FALSE(c.scrubbing());
}

TEST_CASE("replay controls: the time and speed labels", "[client][replay]") {
  CHECK(client::replayClock(0) == "0:00");
  CHECK(client::replayClock(59) == "0:00");  // whole seconds, rounded down
  CHECK(client::replayClock(60) == "0:01");
  CHECK(client::replayClock(1800) == "0:30");
  CHECK(client::replayClock(60U * 60U * 75U) == "75:00");
  CHECK(client::replayTimeLabel(720, 1800) == "0:12 / 0:30");
  CHECK(client::replaySpeedLabel(0) == "0.25x");
  CHECK(client::replaySpeedLabel(1) == "0.5x");
  CHECK(client::replaySpeedLabel(2) == "1x");
  CHECK(client::replaySpeedLabel(4) == "4x");
  CHECK(client::replaySeekTarget(10, -300, 1800) == 0);
  CHECK(client::replaySeekTarget(1700, 300, 1800) == 1800);
  CHECK(client::replaySeekTarget(600, 300, 1800) == 900);
  CHECK(client::replayTickAt(0.0f, 1800) == 0);
  CHECK(client::replayTickAt(0.5f, 1800) == 900);
  CHECK(client::replayTickAt(1.0f, 1800) == 1800);
  CHECK(client::replayTickAt(-1.0f, 1800) == 0);
  CHECK(client::replayTickAt(2.0f, 1800) == 1800);
}

TEST_CASE("replay controls: a focused control that turns disabled hands the focus to Play",
          "[client][replay]") {
  ReplayStatus paused = kPlaying;
  paused.paused = true;
  ReplayControls c;
  c.setFocus(ReplayControl::Step);
  c.refocus(paused);
  CHECK(c.focus() == ReplayControl::Step);  // enabled: kept
  SECTION("the replay plays on") {
    c.refocus(kPlaying);
    CHECK(c.focus() == ReplayControl::PlayPause);
  }
  SECTION("the replay reaches the end, still paused") {
    ReplayStatus end = paused;
    end.tick = end.tickCount;
    end.atEnd = true;
    c.refocus(end);
    CHECK(c.focus() == ReplayControl::PlayPause);
    CHECK(c.apply(ReplayKey::Activate, end) == ReplayCommand{.kind = Kind::Restart});
  }
  SECTION("a key refocuses first: Enter on a disabled Step is Pause") {
    CHECK(c.apply(ReplayKey::Activate, kPlaying) == ReplayCommand{.kind = Kind::TogglePause});
    CHECK(c.focus() == ReplayControl::PlayPause);
  }
  SECTION("other controls are never disabled") {
    for (const ReplayControl control : ReplayControls::controls()) {
      if (control == ReplayControl::Step) continue;
      c.setFocus(control);
      c.refocus(kPlaying);
      CHECK(c.focus() == control);
    }
  }
}

TEST_CASE("replay controls: the bar takes two rows when one leaves the timeline short",
          "[client][replay]") {
  // The pieces as hud.cpp measures them in the game font at 22 px (Restart
  // 95, Play / Pause 137, Step 71, 0.25x 82, Main menu 127, the time 116,
  // 10 px gaps), the bar 16 px in from each side with 14 px of padding.
  const auto rows = [](float display) {
    constexpr float kGap = 10.0f;
    return client::replayBarRows(
        client::ReplayBarPieces{.inner = display - (2.0f * 16.0f) - (2.0f * 14.0f),
                                .transport = 95.1f + 136.5f + 71.3f + 81.6f + (3.0f * kGap),
                                .time = 115.7f,
                                .menu = 126.5f,
                                .gap = kGap});
  };
  for (const float display : {800.0f, 852.0f, 1024.0f, 1280.0f}) {
    INFO("display " << display);
    // Never a timeline under the minimum, one row or two.
    CHECK(rows(display).timeline >= client::kReplayMinTimelineWidth);
  }
  CHECK(rows(800.0f).twoRows);
  CHECK(rows(852.0f).twoRows);  // one row would leave ~105 px: two
  CHECK_FALSE(rows(1024.0f).twoRows);
  CHECK_FALSE(rows(1280.0f).twoRows);
  // Two rows: the timeline beside the time takes the rest of its row.
  CHECK(rows(800.0f).timeline == 800.0f - 60.0f - 115.7f - 10.0f);
  // The switch sits where the one-row timeline reaches the minimum (the
  // pieces, gaps and padding take 746.7 px).
  constexpr float kSwitch = 746.7f + client::kReplayMinTimelineWidth;
  CHECK_FALSE(rows(kSwitch + 0.5f).twoRows);
  CHECK(rows(kSwitch - 0.5f).twoRows);
  // A display too narrow for anything still gives the timeline a width.
  CHECK(rows(10.0f).twoRows);
  CHECK(rows(10.0f).timeline == 1.0f);
}
