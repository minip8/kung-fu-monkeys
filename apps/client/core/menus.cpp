#include "menus.hpp"

#include "kfm/net/messages.hpp"
#include "match_session.hpp"
#include "net_session.hpp"

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

bool isSettingsEvent(const platform::InputEvent& event, bool deviceBound) {
  if (event.kind == InputEventKind::KeyDown) return event.key == Key::Tab;
  return deviceBound && event.kind == InputEventKind::PadButtonDown &&
         event.button == PadButton::Back;
}

// --- MainMenu ---------------------------------------------------------------------------

MainMenu::MainMenu(bool quitItem) {
  items_[count_++] = MainMenuItem::PlayOnline;
  items_[count_++] = MainMenuItem::PlayLocal;
  items_[count_++] = MainMenuItem::HowToPlay;
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
    case MainMenuItem::HowToPlay:
      return "How to play";
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
    case MainMenuItem::HowToPlay:
      return "The controls, and how to catch a banana";
    case MainMenuItem::Settings:
      return "Your name, the volume, the arena's look and mouse aim";
    case MainMenuItem::Quit:
      return "Back to your desktop";
  }
  return "";
}

// --- SettingsMenu -----------------------------------------------------------------------

namespace {
constexpr std::array<SettingsRow, 6> kSettingsRows{SettingsRow::Name,     SettingsRow::Volume,
                                                   SettingsRow::Sound,    SettingsRow::Theme,
                                                   SettingsRow::MouseAim, SettingsRow::Back};
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
    case SettingsRow::Theme:
      settings.theme = presentation::cycleThemeChoice(settings.theme, direction);
      break;
    case SettingsRow::MouseAim:
      settings.mouseAim = !settings.mouseAim;
      break;
    case SettingsRow::Name:
    case SettingsRow::Back:
      break;
  }
  return !(settings == before);
}

SettingsMenu::Press SettingsMenu::press(const platform::InputEvent& event) {
  if (event.kind == InputEventKind::KeyDown && event.key == Key::Escape) return Press::Close;
  if (isMenuBack(event)) return Press::Close;
  if (isMenuActivate(event)) return Press::Activate;
  return Press::None;
}

SettingsMenu::Activation SettingsMenu::activate(LobbySettings& settings) {
  switch (focusedRow()) {
    case SettingsRow::Name:
      return Activation::EditName;
    case SettingsRow::Back:
      close();
      return Activation::Closed;
    case SettingsRow::Volume:
    case SettingsRow::Sound:
    case SettingsRow::Theme:
    case SettingsRow::MouseAim:
      break;
  }
  return change(focus_, +1, settings) ? Activation::Changed : Activation::None;
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
    case SettingsRow::Theme:
      return "Arena theme";
    case SettingsRow::MouseAim:
      return "Mouse aim";
    case SettingsRow::Back:
      return "Done";
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
    case SettingsRow::Theme:
      // v0.97: Auto says what it means.
      if (settings.theme == presentation::ThemeChoice::Auto) return "auto (per arena)";
      return std::string(presentation::themeChoiceName(settings.theme));
    case SettingsRow::MouseAim:
      return settings.mouseAim ? "on" : "off";
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

// --- PauseMenu --------------------------------------------------------------------------

PauseItems pauseItems(PauseContext context) {
  PauseItems out;
  const auto add = [&](PauseItem item) { out.items[out.count++] = item; };
  add(PauseItem::Resume);
  add(PauseItem::Settings);
  add(PauseItem::HowToPlay);
  if (context.online) {
    // v0.97: the host's controls of the running match (v0.96).
    if (context.canRestart) add(PauseItem::Restart);
    if (context.canEnd) add(PauseItem::EndMatch);
    add(PauseItem::LeaveRoom);
  } else {
    add(PauseItem::Restart);
    add(PauseItem::BackToLobby);
    add(PauseItem::MainMenu);
  }
  return out;
}

void PauseMenu::open(PauseContext context) {
  open_ = true;
  help_ = false;
  armed_.reset();
  context_ = context;
  items_ = pauseItems(context);
  buttons_.setCount(items_.count);
  buttons_.setFocus(0);
}

void PauseMenu::setContext(PauseContext context) {
  if (context == context_) return;
  const PauseItem focused = focusedItem();
  context_ = context;
  items_ = pauseItems(context);
  buttons_.setCount(items_.count);
  const auto items = items_.view();
  if (const auto it = std::ranges::find(items, focused); it != items.end()) {
    buttons_.setFocus(static_cast<std::size_t>(it - items.begin()));
  }
  if (armed_ && std::ranges::find(items, *armed_) == items.end()) armed_.reset();
}

void PauseMenu::setFocus(std::size_t index) {
  if (index != buttons_.focus()) armed_.reset();
  buttons_.setFocus(index);
}

void PauseMenu::apply(MenuAction action) {
  if (action != MenuAction::None) armed_.reset();
  buttons_.apply(action);
}

bool PauseMenu::needsConfirm(PauseItem item) const {
  return context_.online && (item == PauseItem::Restart || item == PauseItem::EndMatch);
}

std::optional<PauseItem> PauseMenu::pick(PauseItem item) {
  if (!needsConfirm(item) || armed_ == item) {
    armed_.reset();
    return item;
  }
  armed_ = item;
  return std::nullopt;
}

const char* PauseMenu::buttonLabel(PauseItem item) const {
  if (armed_ == item) {
    if (item == PauseItem::EndMatch) return "Yes, end for everyone";
    if (item == PauseItem::Restart) return "Yes, restart for all";
  }
  return label(item);
}

PauseContext pauseContext(const NetSession& session) {
  return PauseContext{
      .online = true, .canRestart = session.canRestartMatch(), .canEnd = session.canEndMatch()};
}

std::optional<PauseItem> PauseMenu::press(const platform::InputEvent& event) const {
  if (!open_) return std::nullopt;
  if (help_) {
    const bool back = (event.kind == InputEventKind::KeyDown && event.key == Key::Escape) ||
                      (event.kind == InputEventKind::PadButtonDown &&
                       (event.button == PadButton::East || event.button == PadButton::Back ||
                        event.button == PadButton::Start));
    if (back || isMenuActivate(event)) return PauseItem::HowToPlay;
    return std::nullopt;
  }
  if (event.kind == InputEventKind::KeyDown && event.key == Key::Escape) return PauseItem::Resume;
  if (event.kind == InputEventKind::PadButtonDown &&
      (event.button == PadButton::East || event.button == PadButton::Back ||
       event.button == PadButton::Start)) {
    return PauseItem::Resume;
  }
  if (isMenuActivate(event)) return focusedItem();
  return std::nullopt;
}

const char* PauseMenu::label(PauseItem item) {
  switch (item) {
    case PauseItem::Resume:
      return "Resume";
    case PauseItem::Settings:
      return "Settings";
    case PauseItem::Restart:
      return "Restart match";
    case PauseItem::BackToLobby:
      return "Back to lobby";
    case PauseItem::MainMenu:
      return "Main menu";
    case PauseItem::LeaveRoom:
      return "Leave room";
    case PauseItem::EndMatch:
      return "End match";
    case PauseItem::HowToPlay:
      return "How to play";
  }
  return "";
}

bool isPauseEvent(const platform::InputEvent& event, bool deviceBound) {
  if (event.kind == InputEventKind::KeyDown) return event.key == Key::Escape;
  return deviceBound && event.kind == InputEventKind::PadButtonDown &&
         event.button == PadButton::Start;
}

void openPauseMenu(PauseMenu& menu, MatchSession& session) {
  menu.open(PauseContext{.online = false, .canRestart = true, .canEnd = false});
  session.setPaused(true);
}

void closePauseMenu(PauseMenu& menu, MatchSession& session) {
  menu.close();
  // Resumed inside a frame, before its advance(): what is pending or held
  // now must not fire in the first tick.
  session.sampler().endTick();
  session.sampler().consumeAllButtons();
  session.setPaused(false);
}

void openPauseMenu(PauseMenu& menu, NetSession& session) {
  menu.open(pauseContext(session));
  session.setInputSuspended(true);
}

void closePauseMenu(PauseMenu& menu, NetSession& session) {
  menu.close();
  session.setInputSuspended(false);
}

}  // namespace kfm::client
