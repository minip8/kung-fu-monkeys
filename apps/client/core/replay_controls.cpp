#include "replay_controls.hpp"

#include "replay_session.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kfm::client {

namespace {

using platform::InputEventKind;
using platform::Key;
using platform::PadButton;

constexpr std::array<ReplayControl, 6> kControls{ReplayControl::Restart,  ReplayControl::PlayPause,
                                                 ReplayControl::Step,     ReplayControl::Speed,
                                                 ReplayControl::Timeline, ReplayControl::MainMenu};

[[nodiscard]] std::size_t indexOf(ReplayControl c) {
  return static_cast<std::size_t>(c);
}

[[nodiscard]] ReplayKey seek(bool forward, bool longJump) {
  if (forward) return longJump ? ReplayKey::SeekForwardLong : ReplayKey::SeekForward;
  return longJump ? ReplayKey::SeekBackLong : ReplayKey::SeekBack;
}

[[nodiscard]] ReplayCommand seekBy(const ReplayStatus& s, int64_t delta) {
  const uint32_t target = replaySeekTarget(s.tick, delta, s.tickCount);
  if (target == s.tick) return {};
  return {.kind = ReplayCommand::Kind::SeekTo, .tick = target};
}

}  // namespace

std::span<const ReplayControl> ReplayControls::controls() {
  return kControls;
}

ReplayKey ReplayControls::translate(const platform::InputEvent& event) {
  if (event.kind == InputEventKind::KeyUp) {
    if (event.key == Key::LeftShift || event.key == Key::RightShift) shift_ = false;
    return ReplayKey::None;
  }
  if (event.kind == InputEventKind::KeyDown) {
    switch (event.key) {
      case Key::LeftShift:
      case Key::RightShift:
        shift_ = true;
        return ReplayKey::None;
      case Key::Space:
        return ReplayKey::PlayPause;
      case Key::Period:
        return ReplayKey::Step;
      case Key::Left:
      case Key::A:
        return seek(false, shift_);
      case Key::Right:
      case Key::D:
        return seek(true, shift_);
      case Key::LeftBracket:
        return ReplayKey::SpeedDown;
      case Key::RightBracket:
        return ReplayKey::SpeedUp;
      case Key::Home:
        return ReplayKey::Restart;
      case Key::Escape:
        return ReplayKey::MainMenu;
      case Key::Up:
      case Key::W:
        return ReplayKey::FocusPrev;
      case Key::Down:
      case Key::S:
        return ReplayKey::FocusNext;
      case Key::Tab:
        return shift_ ? ReplayKey::FocusPrev : ReplayKey::FocusNext;
      case Key::Enter:
      case Key::KpEnter:
        return ReplayKey::Activate;
      default:
        return ReplayKey::None;
    }
  }
  if (event.kind != InputEventKind::PadButtonDown) return ReplayKey::None;
  switch (event.button) {
    case PadButton::South:
      return ReplayKey::Activate;
    case PadButton::East:
    case PadButton::Back:
      return ReplayKey::MainMenu;
    case PadButton::Start:
      return ReplayKey::PlayPause;
    case PadButton::West:
      return ReplayKey::Step;
    case PadButton::DpadLeft:
      return ReplayKey::SeekBack;
    case PadButton::DpadRight:
      return ReplayKey::SeekForward;
    case PadButton::DpadUp:
      return ReplayKey::FocusPrev;
    case PadButton::DpadDown:
      return ReplayKey::FocusNext;
    case PadButton::LeftShoulder:
      return ReplayKey::SpeedDown;
    case PadButton::RightShoulder:
      return ReplayKey::SpeedUp;
    default:
      return ReplayKey::None;
  }
}

ReplayCommand ReplayControls::apply(ReplayKey key, const ReplayStatus& status) {
  using Kind = ReplayCommand::Kind;
  const auto shortTicks = static_cast<int64_t>(kReplaySeekShortTicks);
  const auto longTicks = static_cast<int64_t>(kReplaySeekLongTicks);
  switch (key) {
    case ReplayKey::None:
      return {};
    case ReplayKey::PlayPause:
      return activate(ReplayControl::PlayPause, status);
    case ReplayKey::Step:
      // The key also pauses a playing replay (the button is for a paused one).
      if (status.atEnd) return {};
      return {.kind = Kind::Step};
    case ReplayKey::SeekBack:
      return seekBy(status, -shortTicks);
    case ReplayKey::SeekForward:
      return seekBy(status, shortTicks);
    case ReplayKey::SeekBackLong:
      return seekBy(status, -longTicks);
    case ReplayKey::SeekForwardLong:
      return seekBy(status, longTicks);
    case ReplayKey::SpeedDown:
      if (status.speedIndex == 0) return {};
      return {.kind = Kind::SetSpeed, .speedIndex = status.speedIndex - 1};
    case ReplayKey::SpeedUp:
      if (status.speedIndex + 1 >= kReplaySpeeds.size()) return {};
      return {.kind = Kind::SetSpeed, .speedIndex = status.speedIndex + 1};
    case ReplayKey::Restart:
      return {.kind = Kind::Restart};
    case ReplayKey::MainMenu:
      return {.kind = Kind::MainMenu};
    case ReplayKey::FocusPrev:
    case ReplayKey::FocusNext: {
      const std::size_t n = kControls.size();
      const std::size_t step = key == ReplayKey::FocusNext ? 1 : n - 1;
      std::size_t i = indexOf(focus_);
      for (std::size_t tries = 0; tries < n; ++tries) {
        i = (i + step) % n;
        if (enabled(kControls[i], status)) break;
      }
      focus_ = kControls[i];
      return {};
    }
    case ReplayKey::Activate:
      return activate(focus_, status);
  }
  return {};
}

ReplayCommand ReplayControls::activate(ReplayControl control, const ReplayStatus& status) {
  using Kind = ReplayCommand::Kind;
  if (!enabled(control, status)) return {};
  switch (control) {
    case ReplayControl::Restart:
      return {.kind = Kind::Restart};
    case ReplayControl::PlayPause:
      // At the end the button is "Watch again".
      return {.kind = status.atEnd ? Kind::Restart : Kind::TogglePause};
    case ReplayControl::Step:
      return {.kind = Kind::Step};
    case ReplayControl::Speed:
      return {.kind = Kind::SetSpeed, .speedIndex = (status.speedIndex + 1) % kReplaySpeeds.size()};
    case ReplayControl::Timeline:
      return {};  // the pointer seeks (scrubTo); Left / Right from the keys
    case ReplayControl::MainMenu:
      return {.kind = Kind::MainMenu};
  }
  return {};
}

bool ReplayControls::enabled(ReplayControl control, const ReplayStatus& status) {
  if (control == ReplayControl::Step) return status.paused && !status.atEnd;
  return true;
}

std::optional<ReplayCommand> ReplayControls::scrubTo(uint32_t tick) {
  if (scrub_ && *scrub_ == tick) return std::nullopt;
  scrub_ = tick;
  return ReplayCommand{.kind = ReplayCommand::Kind::SeekTo, .tick = tick};
}

std::optional<ReplayCommand> ReplayControls::endScrub() {
  if (!scrub_) return std::nullopt;
  const uint32_t tick = *scrub_;
  scrub_.reset();
  return ReplayCommand{.kind = ReplayCommand::Kind::SeekTo, .tick = tick};
}

const char* ReplayControls::label(ReplayControl control, const ReplayStatus& status) {
  switch (control) {
    case ReplayControl::Restart:
      return "Restart";
    case ReplayControl::PlayPause:
      if (status.atEnd) return "Watch again";
      return status.paused ? "Play" : "Pause";
    case ReplayControl::Step:
      return "Step";
    case ReplayControl::Speed: {
      static constexpr std::array<const char*, kReplaySpeeds.size()> kLabels{"0.25x", "0.5x", "1x",
                                                                             "2x", "4x"};
      return kLabels[std::min(status.speedIndex, kLabels.size() - 1)];
    }
    case ReplayControl::Timeline:
      return "";
    case ReplayControl::MainMenu:
      return "Main menu";
  }
  return "";
}

std::string replayClock(uint32_t ticks) {
  const uint32_t seconds = ticks / sim::kTickHz;
  std::array<char, 24> text{};
  (void)std::snprintf(text.data(), text.size(), "%u:%02u", seconds / 60U, seconds % 60U);
  return text.data();
}

std::string replayTimeLabel(uint32_t tick, uint32_t tickCount) {
  return replayClock(tick) + " / " + replayClock(tickCount);
}

std::string replaySpeedLabel(std::size_t index) {
  return ReplayControls::label(ReplayControl::Speed, ReplayStatus{.speedIndex = index});
}

uint32_t replaySeekTarget(uint32_t tick, int64_t delta, uint32_t tickCount) {
  const int64_t target =
      std::clamp<int64_t>(static_cast<int64_t>(tick) + delta, 0, static_cast<int64_t>(tickCount));
  return static_cast<uint32_t>(target);
}

uint32_t replayTickAt(float fraction, uint32_t tickCount) {
  if (std::isnan(fraction)) return 0;
  const double f = std::clamp(static_cast<double>(fraction), 0.0, 1.0);
  return static_cast<uint32_t>(std::lround(f * static_cast<double>(tickCount)));
}

}  // namespace kfm::client
