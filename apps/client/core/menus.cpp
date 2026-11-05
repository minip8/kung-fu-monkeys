#include "menus.hpp"

#include "kfm/net/messages.hpp"

#include <algorithm>
#include <array>

namespace kfm::client {

using platform::InputEventKind;
using platform::Key;
using platform::PadButton;

bool isMenuActivate(const platform::InputEvent& event) {
  if (event.kind == InputEventKind::KeyDown) {
    return event.key == Key::Enter || event.key == Key::KpEnter || event.key == Key::Space;
  }
  return event.kind == InputEventKind::PadButtonDown &&
         (event.button == PadButton::South || event.button == PadButton::Start);
}

bool isMenuBack(const platform::InputEvent& event) {
  return event.kind == InputEventKind::PadButtonDown &&
         (event.button == PadButton::East || event.button == PadButton::Back);
}

// --- MainMenu ---------------------------------------------------------------------------

MainMenu::MainMenu(bool quitItem) {
  items_[count_++] = MainMenuItem::PlayOnline;
  items_[count_++] = MainMenuItem::PlayLocal;
  items_[count_++] = MainMenuItem::Settings;
  if (quitItem) items_[count_++] = MainMenuItem::Quit;
}

void MainMenu::setOnlineAvailable(bool available) {
  online_ = available;
  if (!enabled(focusedItem())) apply(MenuAction::Down);
}

bool MainMenu::enabled(MainMenuItem item) const {
  return item != MainMenuItem::PlayOnline || online_;
}

void MainMenu::setFocus(std::size_t index) {
  if (index < count_ && enabled(items_[index])) focus_ = index;
}

void MainMenu::apply(MenuAction action) {
  if (action != MenuAction::Up && action != MenuAction::Down) return;
  for (std::size_t step = 0; step < count_; ++step) {
    focus_ = action == MenuAction::Down ? (focus_ + 1) % count_ : (focus_ + count_ - 1) % count_;
    if (enabled(items_[focus_])) return;
  }
}

std::optional<MainMenuItem> MainMenu::activate() const {
  if (!enabled(focusedItem())) return std::nullopt;
  return focusedItem();
}

const char* MainMenu::label(MainMenuItem item) {
  switch (item) {
    case MainMenuItem::PlayOnline:
      return "Play online";
    case MainMenuItem::PlayLocal:
      return "Play local";
    case MainMenuItem::Settings:
      return "Settings";
    case MainMenuItem::Quit:
      return "Quit";
  }
  return "";
}

const char* MainMenu::hint(MainMenuItem item) const {
  switch (item) {
    case MainMenuItem::PlayOnline:
      return online_ ? "Make a room and invite friends, or join theirs with a code"
                     : "No lobby configured: start with --lobby URL (or ?lobby=)";
    case MainMenuItem::PlayLocal:
      return "Up to ten monkeys on one screen: keyboards, pads and bots";
    case MainMenuItem::Settings:
      return "Your name, sound and the bots' difficulty";
    case MainMenuItem::Quit:
      return "Back to your desktop";
  }
  return "";
}

// --- SettingsMenu -----------------------------------------------------------------------

namespace {
constexpr std::array<SettingsRow, 5> kSettingsRows{SettingsRow::Name, SettingsRow::Volume,
                                                   SettingsRow::Sound, SettingsRow::BotDifficulty,
                                                   SettingsRow::Back};
}  // namespace

std::span<const SettingsRow> SettingsMenu::rows() {
  return kSettingsRows;
}

void SettingsMenu::setFocus(std::size_t row) {
  if (row < kSettingsRows.size()) focus_ = row;
}

bool SettingsMenu::apply(MenuAction action, LobbySettings& settings) {
  const std::size_t n = kSettingsRows.size();
  switch (action) {
    case MenuAction::Up:
      focus_ = (focus_ + n - 1) % n;
      return false;
    case MenuAction::Down:
      focus_ = (focus_ + 1) % n;
      return false;
    case MenuAction::Left:
      return change(focus_, -1, settings);
    case MenuAction::Right:
      return change(focus_, +1, settings);
    case MenuAction::None:
      break;
  }
  return false;
}

bool SettingsMenu::change(std::size_t row, int direction, LobbySettings& settings) {
  if (row >= kSettingsRows.size() || direction == 0) return false;
  const LobbySettings before = settings;
  switch (kSettingsRows[row]) {
    case SettingsRow::Volume: {
      const int v = std::clamp(
          static_cast<int>(settings.volume) + (direction > 0 ? kVolumeStep : -kVolumeStep), 0, 100);
      settings.volume = static_cast<uint8_t>(v);
      break;
    }
    case SettingsRow::Sound:
      settings.mute = !settings.mute;
      break;
    case SettingsRow::BotDifficulty: {
      constexpr int kTiers = 3;  // easy, medium, hard
      const int t =
          (static_cast<int>(settings.botTier) + (direction > 0 ? 1 : kTiers - 1)) % kTiers;
      settings.botTier = static_cast<ai::ReferenceTier>(t);
      break;
    }
    case SettingsRow::Name:
    case SettingsRow::Back:
      break;
  }
  return !(settings == before);
}

bool SettingsMenu::setNick(std::string& nick, std::string_view text) {
  const net::Nickname n = net::Nickname::from(text);
  if (n.empty() || n.view() == nick) return false;
  nick = n.view();
  return true;
}

const char* SettingsMenu::rowLabel(SettingsRow row) {
  switch (row) {
    case SettingsRow::Name:
      return "Name";
    case SettingsRow::Volume:
      return "Volume";
    case SettingsRow::Sound:
      return "Sound";
    case SettingsRow::BotDifficulty:
      return "Bot difficulty";
    case SettingsRow::Back:
      return "Back";
  }
  return "";
}

std::string SettingsMenu::rowValue(SettingsRow row, const LobbySettings& settings,
                                   const std::string& nick) {
  switch (row) {
    case SettingsRow::Name:
      return nick;
    case SettingsRow::Volume:
      return std::to_string(settings.volume) + "%";
    case SettingsRow::Sound:
      return settings.mute ? "muted" : "on";
    case SettingsRow::BotDifficulty:
      return std::string(ai::toString(settings.botTier));
    case SettingsRow::Back:
      break;
  }
  return {};
}

// --- ButtonMenu -------------------------------------------------------------------------

void ButtonMenu::setCount(std::size_t count) {
  count_ = std::max<std::size_t>(count, 1);
  focus_ = std::min(focus_, count_ - 1);
}

void ButtonMenu::setFocus(std::size_t index) {
  if (index < count_) focus_ = index;
}

void ButtonMenu::apply(MenuAction action) {
  switch (action) {
    case MenuAction::Up:
    case MenuAction::Left:
      focus_ = (focus_ + count_ - 1) % count_;
      break;
    case MenuAction::Down:
    case MenuAction::Right:
      focus_ = (focus_ + 1) % count_;
      break;
    case MenuAction::None:
      break;
  }
}

}  // namespace kfm::client
