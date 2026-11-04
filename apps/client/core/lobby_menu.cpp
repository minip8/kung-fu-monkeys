#include "lobby_menu.hpp"

#include "kfm/sim/constants.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace kfm::client {

namespace {

using platform::InputEventKind;
using platform::Key;
using platform::PadAxis;
using platform::PadButton;

[[nodiscard]] MenuAction keyAction(Key k) {
  switch (k) {
    case Key::Up:
    case Key::W:
      return MenuAction::Up;
    case Key::Down:
    case Key::S:
      return MenuAction::Down;
    case Key::Left:
    case Key::A:
      return MenuAction::Left;
    case Key::Right:
    case Key::D:
      return MenuAction::Right;
    default:
      return MenuAction::None;
  }
}

[[nodiscard]] MenuAction padButtonAction(PadButton b) {
  switch (b) {
    case PadButton::DpadUp:
      return MenuAction::Up;
    case PadButton::DpadDown:
      return MenuAction::Down;
    case PadButton::DpadLeft:
      return MenuAction::Left;
    case PadButton::DpadRight:
      return MenuAction::Right;
    default:
      return MenuAction::None;
  }
}

// One axis's latch: a push past kStickPress fires once; the stick re-arms
// once it is back under kStickRelease.
[[nodiscard]] MenuAction stickAxis(int8_t& latch, float v, MenuAction negative,
                                   MenuAction positive) {
  if (latch != 0) {
    if (std::abs(v) < MenuInput::kStickRelease) latch = 0;
    return MenuAction::None;
  }
  if (v >= MenuInput::kStickPress) {
    latch = 1;
    return positive;
  }
  if (v <= -MenuInput::kStickPress) {
    latch = -1;
    return negative;
  }
  return MenuAction::None;
}

}  // namespace

// --- MenuInput -------------------------------------------------------------------

MenuInput::Stick* MenuInput::stickFor(uint32_t device) {
  for (Stick& s : sticks_) {
    if (s.used && s.device == device) return &s;
  }
  for (Stick& s : sticks_) {
    if (!s.used) {
      s = Stick{.device = device, .used = true};
      return &s;
    }
  }
  return nullptr;
}

MenuAction MenuInput::translate(const platform::InputEvent& event, bool deviceBound) {
  switch (event.kind) {
    case InputEventKind::KeyDown:
      return keyAction(event.key);
    case InputEventKind::PadButtonDown:
      return deviceBound ? padButtonAction(event.button) : MenuAction::None;
    case InputEventKind::PadAxis: {
      if (!deviceBound) return MenuAction::None;
      if (event.axis != PadAxis::LeftX && event.axis != PadAxis::LeftY) return MenuAction::None;
      Stick* s = stickFor(event.deviceId);
      if (s == nullptr) return MenuAction::None;
      // SDL's sign: +Y is down.
      return event.axis == PadAxis::LeftX
                 ? stickAxis(s->x, event.value, MenuAction::Left, MenuAction::Right)
                 : stickAxis(s->y, event.value, MenuAction::Up, MenuAction::Down);
    }
    case InputEventKind::PadRemoved:
      for (Stick& s : sticks_) {
        if (s.used && s.device == event.deviceId) s = Stick{};
      }
      return MenuAction::None;
    case InputEventKind::KeyUp:
    case InputEventKind::PadButtonUp:
    case InputEventKind::PadAdded:
      break;
  }
  return MenuAction::None;
}

bool eventDeviceBound(const platform::InputSampler& sampler, const platform::InputEvent& event) {
  if (event.deviceId == platform::kKeyboardDevice) return true;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    const platform::DeviceBinding b = sampler.binding(PlayerId{i});
    if (b.kind == platform::DeviceKind::Gamepad && b.padId == event.deviceId) return true;
  }
  return false;
}

// --- LobbyMenu -------------------------------------------------------------------

LobbyMenu::LobbyMenu() {
  setContent(sim::defaultContent());
}

const sim::ContentDB& LobbyMenu::content() const {
  return content_ != nullptr ? *content_ : sim::defaultContent();
}

void LobbyMenu::setContent(const sim::ContentDB& content) {
  const LobbyRow focused = rowCount_ > 0 ? rows_[focus_] : LobbyRow{};
  content_ = &content;
  rowCount_ = 0;
  rows_[rowCount_++] = LobbyRow{LobbyRowKind::Arena};
  rows_[rowCount_++] = LobbyRow{LobbyRowKind::Mode};
  rows_[rowCount_++] = LobbyRow{LobbyRowKind::Teams};
  rows_[rowCount_++] = LobbyRow{LobbyRowKind::Loadout};
  const uint8_t powerUps = std::min<uint8_t>(content.powerUpCount, sim::kMaxPowerUps);
  for (uint8_t i = 0; i < powerUps; ++i)
    rows_[rowCount_++] = LobbyRow{LobbyRowKind::PowerUp, i};
  rows_[rowCount_++] = LobbyRow{LobbyRowKind::Bots};
  rows_[rowCount_++] = LobbyRow{LobbyRowKind::BotDifficulty};
  rows_[rowCount_++] = LobbyRow{LobbyRowKind::Volume};
  rows_[rowCount_++] = LobbyRow{LobbyRowKind::Mute};
  if (settings_.arena >= content.arenaCount) settings_.arena = 0;
  if (settings_.mode >= content.modeCount) settings_.mode = 0;
  if (settings_.loadout >= content.loadoutCount) settings_.loadout = 0;
  if (!content.compatible(settings_.arena, settings_.mode)) {
    settings_.arena = content.firstCompatibleArena(settings_.mode).value_or(0);
  }
  focus_ = 0;
  for (std::size_t i = 0; i < rowCount_; ++i) {
    if (rows_[i] == focused) focus_ = i;
  }
}

void LobbyMenu::setFocus(std::size_t row) {
  if (row < rowCount_) focus_ = row;
}

bool LobbyMenu::apply(MenuAction action) {
  switch (action) {
    case MenuAction::Up:
      focus_ = focus_ == 0 ? rowCount_ - 1 : focus_ - 1;
      return false;
    case MenuAction::Down:
      focus_ = focus_ + 1 >= rowCount_ ? 0 : focus_ + 1;
      return false;
    case MenuAction::Left:
      return change(focus_, -1);
    case MenuAction::Right:
      return change(focus_, +1);
    case MenuAction::None:
      break;
  }
  return false;
}

bool LobbyMenu::change(std::size_t row, int direction) {
  if (row >= rowCount_ || direction == 0) return false;
  const LobbySettings before = settings_;
  const sim::ContentDB& db = content();
  const auto cycle = [&](uint8_t value, uint8_t count) {
    if (count == 0) return uint8_t{0};
    const int next = (static_cast<int>(value) + (direction > 0 ? 1 : count - 1)) % count;
    return static_cast<uint8_t>(next);
  };
  const LobbyRow& r = rows_[row];
  switch (r.kind) {
    case LobbyRowKind::Arena:
      // The next arena that suits the mode (v0.32); the current one when
      // no other does.
      for (uint8_t step = 0; step < db.arenaCount; ++step) {
        settings_.arena = cycle(settings_.arena, db.arenaCount);
        if (db.compatible(settings_.arena, settings_.mode)) break;
      }
      break;
    case LobbyRowKind::Mode:
      settings_.mode = cycle(settings_.mode, db.modeCount);
      if (!db.compatible(settings_.arena, settings_.mode)) {
        settings_.arena = db.firstCompatibleArena(settings_.mode).value_or(settings_.arena);
      }
      break;
    case LobbyRowKind::Teams:
      break;  // derived from the mode
    case LobbyRowKind::Loadout:
      settings_.loadout = cycle(settings_.loadout, db.loadoutCount);
      break;
    case LobbyRowKind::PowerUp:
      settings_.powerUps = static_cast<uint16_t>(settings_.powerUps ^ (1u << r.powerUp));
      break;
    case LobbyRowKind::Bots: {
      const int bots = std::clamp(static_cast<int>(settings_.bots) + (direction > 0 ? 1 : -1), 0,
                                  static_cast<int>(sim::kMaxPlayers) - 1);
      settings_.bots = static_cast<uint8_t>(bots);
      break;
    }
    case LobbyRowKind::BotDifficulty: {
      constexpr uint8_t kTiers = 3;  // easy, medium, hard
      settings_.botTier =
          static_cast<ai::ReferenceTier>(cycle(static_cast<uint8_t>(settings_.botTier), kTiers));
      break;
    }
    case LobbyRowKind::Volume: {
      const int volume = std::clamp(
          static_cast<int>(settings_.volume) + (direction > 0 ? kVolumeStep : -kVolumeStep), 0,
          100);
      settings_.volume = static_cast<uint8_t>(volume);
      break;
    }
    case LobbyRowKind::Mute:
      settings_.mute = !settings_.mute;
      break;
  }
  return !(settings_ == before);
}

std::string LobbyMenu::rowLabel(const LobbyRow& row) const {
  switch (row.kind) {
    case LobbyRowKind::Arena:
      return "Arena";
    case LobbyRowKind::Mode:
      return "Mode";
    case LobbyRowKind::Teams:
      return "Teams";
    case LobbyRowKind::Loadout:
      return "Loadout";
    case LobbyRowKind::PowerUp:
      return std::string(content().powerUpAt(row.powerUp).name.view());
    case LobbyRowKind::Bots:
      return "Bots";
    case LobbyRowKind::BotDifficulty:
      return "Bot difficulty";
    case LobbyRowKind::Volume:
      return "Volume";
    case LobbyRowKind::Mute:
      return "Sound";
  }
  return {};
}

std::string LobbyMenu::rowValue(const LobbyRow& row) const {
  const sim::ContentDB& db = content();
  switch (row.kind) {
    case LobbyRowKind::Arena:
      return std::string(db.arenaAt(settings_.arena).name.view());
    case LobbyRowKind::Mode:
      return std::string(db.modeAt(settings_.mode).name.view());
    case LobbyRowKind::Teams: {
      const uint8_t teams = db.modeAt(settings_.mode).teams;
      return teams == 0 ? std::string("free-for-all") : std::to_string(teams) + " (by mode)";
    }
    case LobbyRowKind::Loadout:
      return std::string(db.loadoutAt(settings_.loadout).name.view());
    case LobbyRowKind::PowerUp:
      return (settings_.powerUps & (1u << row.powerUp)) != 0 ? "on" : "off";
    case LobbyRowKind::Bots:
      return std::to_string(settings_.bots);
    case LobbyRowKind::BotDifficulty:
      return std::string(ai::toString(settings_.botTier));
    case LobbyRowKind::Volume:
      return std::to_string(settings_.volume) + "%";
    case LobbyRowKind::Mute:
      return settings_.mute ? "muted" : "on";
  }
  return {};
}

std::string loadoutSummary(const sim::ContentDB& db, uint8_t loadout) {
  const std::array<uint8_t, sim::kAbilitySlots>& slots = db.loadoutAbilities(loadout);
  std::string s;
  for (uint8_t slot = 0; slot < sim::kAbilitySlots; ++slot) {
    if (slots[slot] == 0) continue;  // an empty slot (the base kit's ability button)
    if (!s.empty()) s += ", ";
    s += db.abilityAt(slots[slot]).name.view();
  }
  return s;
}

std::string ticksClock(uint32_t ticks) {
  const uint32_t seconds = (ticks + sim::kTickHz - 1) / sim::kTickHz;
  char buf[16];
  std::snprintf(buf, sizeof buf, "%u:%02u", seconds / 60, seconds % 60);
  return buf;
}

std::string modeSummary(const sim::ModeDef& mode) {
  char buf[128];
  const auto secs = [](uint32_t ticks) {
    return static_cast<double>(ticks) / static_cast<double>(sim::kTickHz);
  };
  // v0.32: the team kinds say so ("teams of 2: ...").
  const char* team = mode.teams > 0 ? "team " : "";
  switch (mode.kind) {
    case sim::ModeKind::Deathmatch:
    case sim::ModeKind::TeamDeathmatch: {
      const std::string clock =
          mode.timeLimitTicks == 0 ? std::string("no clock") : ticksClock(mode.timeLimitTicks);
      if (mode.respawnInvulnTicks > 0) {
        std::snprintf(buf, sizeof buf,
                      "first %sto %u eliminations, %s, respawn %.1f s (%.1f s protected)", team,
                      static_cast<unsigned>(mode.scoreToWin), clock.c_str(),
                      secs(mode.respawnTicks), secs(mode.respawnInvulnTicks));
      } else {
        std::snprintf(buf, sizeof buf, "first %sto %u eliminations, %s, respawn %.1f s", team,
                      static_cast<unsigned>(mode.scoreToWin), clock.c_str(),
                      secs(mode.respawnTicks));
      }
      return buf;
    }
    case sim::ModeKind::Hill: {
      const std::string clock =
          mode.timeLimitTicks == 0 ? std::string("no clock") : ticksClock(mode.timeLimitTicks);
      std::snprintf(buf, sizeof buf,
                    "first team to %u points, a point per %.1f s alone on the hill, %s",
                    static_cast<unsigned>(mode.scoreToWin), secs(mode.hillTicks), clock.c_str());
      return buf;
    }
    case sim::ModeKind::TeamRounds:
      std::snprintf(buf, sizeof buf, "first team to %u rounds, last team standing",
                    static_cast<unsigned>(mode.roundsToWin));
      return buf;
    case sim::ModeKind::Survival:
      if (mode.timeLimitTicks != 0) {
        std::snprintf(buf, sizeof buf, "first to %u rounds of %s, last monkey standing",
                      static_cast<unsigned>(mode.roundsToWin),
                      ticksClock(mode.timeLimitTicks).c_str());
        return buf;
      }
      [[fallthrough]];
    case sim::ModeKind::Rounds:
    case sim::ModeKind::kCount:
      break;
  }
  std::snprintf(buf, sizeof buf, "first to %u rounds, last monkey standing",
                static_cast<unsigned>(mode.roundsToWin));
  return buf;
}

}  // namespace kfm::client
