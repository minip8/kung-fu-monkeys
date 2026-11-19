#include "lobby_menu.hpp"

#include "content_labels.hpp"
#include "kfm/sim/constants.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <utility>

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
    // v0.45: the mouse never navigates (the UI takes its clicks itself).
    case InputEventKind::MouseMotion:
    case InputEventKind::MouseButtonDown:
    case InputEventKind::MouseButtonUp:
    case InputEventKind::MouseLeave:
      break;
  }
  return MenuAction::None;
}

bool isActivateEvent(const platform::InputEvent& event, bool deviceBound) {
  if (event.kind == InputEventKind::KeyDown) {
    return event.key == Key::Enter || event.key == Key::KpEnter;
  }
  return deviceBound && event.kind == InputEventKind::PadButtonDown &&
         event.button == PadButton::Start;
}

bool eventDeviceBound(const platform::InputSampler& sampler, const platform::InputEvent& event) {
  if (event.deviceId == platform::kKeyboardDevice) return true;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    const platform::DeviceBinding b = sampler.binding(PlayerId{i});
    if (b.kind == platform::DeviceKind::Gamepad && b.padId == event.deviceId) return true;
  }
  return false;
}

bool uiCapturesEvent(const platform::InputEvent& event, bool uiWantsKeyboard, bool uiWantsMouse) {
  if (event.kind == InputEventKind::KeyDown) return uiWantsKeyboard;
  if (event.kind == InputEventKind::MouseButtonDown) return uiWantsMouse;
  return false;
}

// --- PowerUpPicker ---------------------------------------------------------------

void PowerUpPicker::open(const sim::ContentDB& content, uint8_t arena) {
  open_ = true;
  arena_ = arena;
  count_ = 0;
  const uint16_t drawn = arenaPowerUps(content, content.arenaAt(arena));
  for (uint8_t i = 0; i < content.powerUpCount && i < sim::kMaxPowerUps; ++i) {
    if ((drawn & (1u << i)) != 0) items_[count_++] = Item{ItemKind::Toggle, i};
  }
  toggles_ = count_;
  items_[count_++] = Item{ItemKind::All};
  items_[count_++] = Item{ItemKind::None};
  items_[count_++] = Item{ItemKind::Done};
  focus_ = 0;
}

void PowerUpPicker::setFocus(std::size_t index) {
  if (index < count_) focus_ = index;
}

std::size_t PowerUpPicker::rowCount() const {
  return ((toggles_ + kColumns - 1) / kColumns) + 1;  // the toggles, then the buttons
}

std::size_t PowerUpPicker::rowOf(std::size_t index) const {
  return index < toggles_ ? index / kColumns : rowCount() - 1;
}

std::size_t PowerUpPicker::columnOf(std::size_t index) const {
  return index < toggles_ ? index % kColumns : index - toggles_;
}

std::size_t PowerUpPicker::rowStart(std::size_t row) const {
  return row + 1 < rowCount() ? row * kColumns : toggles_;
}

std::size_t PowerUpPicker::rowLength(std::size_t row) const {
  if (row + 1 >= rowCount()) return count_ - toggles_;
  return std::min(kColumns, toggles_ - (row * kColumns));
}

void PowerUpPicker::apply(MenuAction action) {
  if (count_ == 0) return;
  const std::size_t rows = rowCount();
  const std::size_t row = rowOf(focus_);
  const std::size_t column = columnOf(focus_);
  switch (action) {
    case MenuAction::Up:
    case MenuAction::Down: {
      const std::size_t to =
          action == MenuAction::Down ? (row + 1) % rows : (row + rows - 1) % rows;
      focus_ = rowStart(to) + std::min(column, rowLength(to) - 1);
      break;
    }
    case MenuAction::Left:
    case MenuAction::Right: {
      const std::size_t n = rowLength(row);
      const std::size_t to = action == MenuAction::Right ? (column + 1) % n : (column + n - 1) % n;
      focus_ = rowStart(row) + to;
      break;
    }
    case MenuAction::None:
      break;
  }
}

bool PowerUpPicker::activate(std::size_t index, LobbySettings& settings) {
  if (!open_ || index >= count_) return false;
  focus_ = index;
  const uint16_t before = settings.powerUps;
  const Item& item = items_[index];
  switch (item.kind) {
    case ItemKind::Toggle:
      settings.powerUps = static_cast<uint16_t>(settings.powerUps ^ (1u << item.powerUp));
      break;
    case ItemKind::All:
      settings.powerUps = sim::kAllPowerUps;
      break;
    case ItemKind::None:
      settings.powerUps = 0;
      break;
    case ItemKind::Done:
      open_ = false;
      break;
  }
  return settings.powerUps != before;
}

const char* PowerUpPicker::label(ItemKind kind) {
  switch (kind) {
    case ItemKind::All:
      return "All";
    case ItemKind::None:
      return "None";
    case ItemKind::Done:
      return "Done";
    case ItemKind::Toggle:
      break;
  }
  return "";
}

// --- LobbyMenu -------------------------------------------------------------------

LobbyMenu::LobbyMenu() {
  setContent(sim::defaultContent());
}

const sim::ContentDB& LobbyMenu::content() const {
  return content_ != nullptr ? *content_ : sim::defaultContent();
}

void LobbyMenu::setContent(const sim::ContentDB& content) {
  content_ = &content;
  presets_ = resolvePresets(content);
  lastPreset_.reset();
  picker_.close();
  notice_.clear();
  clampSettings();
  if (const std::optional<std::size_t> p = activePreset()) lastPreset_ = p;
  if (!rowEnabled(focusedRow())) focus_ = 0;
}

void LobbyMenu::clampSettings() {
  const sim::ContentDB& db = content();
  if (settings_.arena >= db.arenaCount) settings_.arena = 0;
  if (settings_.mode >= db.modeCount) settings_.mode = 0;
  if (settings_.loadout >= db.loadoutCount) settings_.loadout = 0;
  if (!db.compatible(settings_.arena, settings_.mode)) {
    settings_.arena = db.firstCompatibleArena(settings_.mode).value_or(0);
  }
}

void LobbyMenu::setRoomRows(bool room) {
  roomRows_ = room;
  if (!rowEnabled(focusedRow())) focus_ = 0;
}

void LobbyMenu::setPlayerCap(uint8_t maxPlayers) {
  playerCap_ = std::clamp<uint8_t>(maxPlayers, 1, static_cast<uint8_t>(sim::kMaxPlayers));
  settings_.bots = std::min(settings_.bots, maxBots());
}

void LobbyMenu::setFocus(std::size_t row) {
  if (row < kLobbyRows.size()) focus_ = row;
}

bool LobbyMenu::rowEnabled(LobbyRowKind row) const {
  const sim::ContentDB& db = content();
  switch (row) {
    case LobbyRowKind::Loadout:
      return db.modeAt(settings_.mode).roleCount == 0;
    case LobbyRowKind::PowerUps:
      return arenaPowerUps(db, db.arenaAt(settings_.arena)) != 0;
    case LobbyRowKind::BotDifficulty:
      return !roomRows_;
    case LobbyRowKind::Game:
    case LobbyRowKind::Mode:
    case LobbyRowKind::Arena:
    case LobbyRowKind::Bots:
      break;
  }
  return true;
}

std::string LobbyMenu::rowReason(LobbyRowKind row) const {
  if (rowEnabled(row)) return {};
  const sim::ContentDB& db = content();
  switch (row) {
    case LobbyRowKind::Loadout:
      return modeLabel(db.modeAt(settings_.mode).name.view()).title +
             " gives each monkey its kit by its place in the team.";
    case LobbyRowKind::PowerUps:
      return arenaTitle(db.arenaAt(settings_.arena).name.view()) +
             " has no power-up spawners: pick an arena with them for power-ups.";
    case LobbyRowKind::BotDifficulty:
      return "Online, the room's server runs the bots and sets how they play.";
    case LobbyRowKind::Game:
    case LobbyRowKind::Mode:
    case LobbyRowKind::Arena:
    case LobbyRowKind::Bots:
      break;
  }
  return {};
}

std::optional<std::size_t> LobbyMenu::activePreset() const {
  return findPreset(content(), presets(), settings_);
}

bool LobbyMenu::applyPreset(std::size_t index) {
  if (index >= presets_.count) return false;
  const LobbySettings before = settings_;
  const ResolvedPreset& p = presets_.items[index];
  const uint8_t freeSlots = static_cast<uint8_t>(playerCap_ > humans_ ? playerCap_ - humans_ : 0);
  const uint8_t oldBots = settings_.bots;
  if (client::applyPreset(p, humans_, std::min(maxBots(), freeSlots), settings_)) {
    const unsigned added = static_cast<unsigned>(settings_.bots - oldBots);
    notice_ = "Added " + std::to_string(added) + (added == 1 ? " bot: " : " bots: ") +
              std::string(p.def->title) + " is for " +
              std::to_string(static_cast<unsigned>(p.def->players)) + " monkeys.";
  }
  lastPreset_ = index;
  return !(settings_ == before);
}

PowerUpChoice LobbyMenu::powerUpChoice() const {
  const sim::ContentDB& db = content();
  const uint16_t drawn = arenaPowerUps(db, db.arenaAt(settings_.arena));
  const auto on = static_cast<uint16_t>(drawn & settings_.powerUps);
  if (on == 0) return PowerUpChoice::None;
  return on == drawn ? PowerUpChoice::All : PowerUpChoice::Custom;
}

void LobbyMenu::openPicker() {
  if (!rowEnabled(LobbyRowKind::PowerUps)) return;
  picker_.open(content(), settings_.arena);
}

bool LobbyMenu::apply(MenuAction action) {
  // Up/Down step over the greyed rows.
  const auto step = [&](bool down) {
    const std::size_t n = kLobbyRows.size();
    for (std::size_t k = 0; k < n; ++k) {
      focus_ = down ? (focus_ + 1) % n : (focus_ + n - 1) % n;
      if (rowEnabled(kLobbyRows[focus_])) return;
    }
  };
  switch (action) {
    case MenuAction::Up:
      step(false);
      return false;
    case MenuAction::Down:
      step(true);
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

void LobbyMenu::changeMode(int direction) {
  const sim::ContentDB& db = content();
  const auto count = db.modeCount;
  const uint8_t oldArena = settings_.arena;
  // v0.94: past the bot-contest modes (a --mode flag may still pick one).
  for (uint8_t n = 0; n < count; ++n) {
    settings_.mode =
        static_cast<uint8_t>((settings_.mode + (direction > 0 ? 1 : count - 1)) % count);
    if (!isBotMode(db.modeAt(settings_.mode))) break;
  }
  // v0.34: a mode that names its arena (ModeDef::arena) brings it; v0.82: a
  // mode with a pool brings its first member unless the arena already is
  // one; an arena that does not suit the mode gives way (v0.32).
  const sim::ModeDef& m = db.modeAt(settings_.mode);
  const bool fits = db.compatible(settings_.arena, settings_.mode);
  if (m.hasArena() || (m.hasArenaSet() && !m.inArenaSet(settings_.arena)) || !fits) {
    settings_.arena = db.firstCompatibleArena(settings_.mode).value_or(settings_.arena);
  }
  if (settings_.arena == oldArena) return;
  // v0.97: say so, and why.
  const std::string arena = arenaTitle(db.arenaAt(settings_.arena).name.view());
  const std::string mode = modeLabel(m.name.view()).title;
  if (!fits && !arenaNeed(m).empty()) {
    notice_ = "Arena set to " + arena + ": " + mode + " needs " + std::string(arenaNeed(m)) + ".";
  } else if (m.hasArena()) {
    notice_ = "Arena set to " + arena + ", " + mode + "'s own arena.";
  } else {
    notice_ = "Arena set to " + arena + ", the first of " + mode + "'s arenas.";
  }
}

bool LobbyMenu::change(std::size_t row, int direction) {
  if (row >= kLobbyRows.size() || direction == 0) return false;
  const LobbyRowKind kind = kLobbyRows[row];
  if (!rowEnabled(kind)) return false;
  const LobbySettings before = settings_;
  const sim::ContentDB& db = content();
  const auto cycle = [&](uint8_t value, uint8_t count) {
    if (count == 0) return uint8_t{0};
    const int next = (static_cast<int>(value) + (direction > 0 ? 1 : count - 1)) % count;
    return static_cast<uint8_t>(next);
  };
  notice_.clear();
  switch (kind) {
    case LobbyRowKind::Game: {
      const std::size_t n = presets_.count;
      if (n == 0) break;
      std::size_t next = 0;
      if (const std::optional<std::size_t> at = activePreset()) {
        next = direction > 0 ? (*at + 1) % n : (*at + n - 1) % n;
      } else if (lastPreset_) {
        // Custom sits right after the preset it was edited from: Left goes
        // back to that one, Right on to the next.
        const std::size_t from = std::min(*lastPreset_, n - 1);
        next = direction > 0 ? (from + 1) % n : from;
      } else {
        next = direction > 0 ? 0 : n - 1;  // before any preset: the ends of the list
      }
      (void)applyPreset(next);
      break;
    }
    case LobbyRowKind::Arena:
      // The next arena that suits the mode (v0.32); the current one when
      // no other does.
      for (uint8_t step = 0; step < db.arenaCount; ++step) {
        settings_.arena = cycle(settings_.arena, db.arenaCount);
        if (db.compatible(settings_.arena, settings_.mode)) break;
      }
      break;
    case LobbyRowKind::Mode:
      changeMode(direction);
      break;
    case LobbyRowKind::Loadout:
      // v0.97: past the kits a mode hands out by role (and so the bot
      // contests', v0.94).
      for (uint8_t n = 0; n < db.loadoutCount; ++n) {
        settings_.loadout = cycle(settings_.loadout, db.loadoutCount);
        if (!isRoleLoadout(db, settings_.loadout)) break;
      }
      break;
    case LobbyRowKind::PowerUps: {
      // All -> None -> Custom (the picker) -> All.
      const auto at = static_cast<uint8_t>(powerUpChoice());
      const auto next = static_cast<PowerUpChoice>(cycle(at, 3));
      if (next == PowerUpChoice::All) {
        settings_.powerUps = sim::kAllPowerUps;
      } else if (next == PowerUpChoice::None) {
        settings_.powerUps = 0;
      } else {
        openPicker();
      }
      break;
    }
    case LobbyRowKind::Bots: {
      const int bots = std::clamp(static_cast<int>(settings_.bots) + (direction > 0 ? 1 : -1), 0,
                                  static_cast<int>(maxBots()));
      settings_.bots = static_cast<uint8_t>(bots);
      break;
    }
    case LobbyRowKind::BotDifficulty: {
      constexpr uint8_t kTiers = 3;  // easy, medium, hard
      settings_.botTier =
          static_cast<ai::ReferenceTier>(cycle(static_cast<uint8_t>(settings_.botTier), kTiers));
      break;
    }
  }
  if (const std::optional<std::size_t> p = activePreset()) lastPreset_ = p;
  return !(settings_ == before);
}

const char* LobbyMenu::rowLabel(LobbyRowKind row) {
  switch (row) {
    case LobbyRowKind::Game:
      return "Game";
    case LobbyRowKind::Mode:
      return "Mode";
    case LobbyRowKind::Arena:
      return "Arena";
    case LobbyRowKind::Loadout:
      return "Kit";
    case LobbyRowKind::PowerUps:
      return "Power-ups";
    case LobbyRowKind::Bots:
      return "Bots";
    case LobbyRowKind::BotDifficulty:
      return "Bot difficulty";
  }
  return "";
}

std::string LobbyMenu::rowValue(LobbyRowKind row) const {
  const sim::ContentDB& db = content();
  switch (row) {
    case LobbyRowKind::Game: {
      const std::optional<std::size_t> p = activePreset();
      return p ? std::string(presets_.items[*p].def->title) : std::string("Custom");
    }
    case LobbyRowKind::Mode:
      return modeLabel(db.modeAt(settings_.mode).name.view()).title;
    case LobbyRowKind::Arena:
      return arenaTitle(db.arenaAt(settings_.arena).name.view());
    case LobbyRowKind::Loadout:
      if (!rowEnabled(row)) return "By role";
      return loadoutTitle(db.loadoutAt(settings_.loadout).name.view());
    case LobbyRowKind::PowerUps: {
      if (!rowEnabled(row)) return "None in this arena";
      const uint16_t drawn = arenaPowerUps(db, db.arenaAt(settings_.arena));
      switch (powerUpChoice()) {
        case PowerUpChoice::All:
          return "All";
        case PowerUpChoice::None:
          return "None";
        case PowerUpChoice::Custom:
          break;
      }
      const auto on = std::popcount(static_cast<unsigned>(drawn & settings_.powerUps));
      return std::to_string(on) + " of " + std::to_string(std::popcount(unsigned{drawn}));
    }
    case LobbyRowKind::Bots:
      return std::to_string(settings_.bots);
    case LobbyRowKind::BotDifficulty:
      if (!rowEnabled(row)) return "Server's choice";
      return std::string(ai::toString(settings_.botTier));
  }
  return {};
}

std::string_view arenaNeed(const sim::ModeDef& mode) {
  if (mode.space == sim::Space::Grid || mode.vision.fog()) return "a grid arena";
  switch (mode.kind) {
    case sim::ModeKind::Hill:
    case sim::ModeKind::Control:
      return "an arena with a hill zone";
    case sim::ModeKind::Capture:
      return "an arena with a base for each team";
    case sim::ModeKind::Harvest:
      return "an arena with a home for each team and a tree";
    case sim::ModeKind::Rounds:
    case sim::ModeKind::Deathmatch:
    case sim::ModeKind::Survival:
    case sim::ModeKind::TeamRounds:
    case sim::ModeKind::TeamDeathmatch:
    case sim::ModeKind::KillRounds:
    case sim::ModeKind::kCount:
      break;
  }
  if (mode.teams > 0) return "a starting side for each team";
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

ArenaSpawners arenaSpawners(const sim::ArenaDef& arena) {
  ArenaSpawners n;
  for (uint8_t i = 0; i < arena.pickupSpawnerCount && i < sim::kMaxPickupSpawners; ++i) {
    if (arena.pickupSpawners[i].isTree()) {
      ++n.trees;
    } else {
      ++n.powerUps;
    }
  }
  return n;
}

std::string treesText(uint8_t trees) {
  return trees == 1 ? std::string("1 tree bears bananas")
                    : std::to_string(static_cast<unsigned>(trees)) + " trees bear bananas";
}

std::string beatText(uint32_t ticks) {
  char buf[24];
  const double s = static_cast<double>(ticks) / static_cast<double>(sim::kTickHz);
  std::snprintf(buf, sizeof buf, s >= 1.0 ? "%.1f s" : "%.2f s", s);
  return buf;
}

std::string modeSummary(const sim::ModeDef& mode) {
  char buf[160];
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
      // v0.57: with catch or suicide points the target counts points.
      const char* unit = mode.catchScore != 0 || mode.suicideScore != 0 ? "points" : "eliminations";
      if (mode.respawnInvulnTicks > 0) {
        std::snprintf(buf, sizeof buf, "first %sto %u %s, %s, respawn %.1f s (%.1f s protected)",
                      team, static_cast<unsigned>(mode.scoreToWin), unit, clock.c_str(),
                      secs(mode.respawnTicks), secs(mode.respawnInvulnTicks));
      } else {
        std::snprintf(buf, sizeof buf, "first %sto %u %s, %s, respawn %.1f s", team,
                      static_cast<unsigned>(mode.scoreToWin), unit, clock.c_str(),
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
    case sim::ModeKind::Capture: {
      const std::string clock =
          mode.timeLimitTicks == 0 ? std::string("no clock") : ticksClock(mode.timeLimitTicks);
      std::snprintf(
          buf, sizeof buf, "first team to %u captures, a dropped flag home after %.1f s, %s",
          static_cast<unsigned>(mode.scoreToWin), secs(mode.flagReturnTicks), clock.c_str());
      return buf;
    }
    case sim::ModeKind::Control: {
      // v0.80: capture zones and keep them.
      const std::string clock =
          mode.timeLimitTicks == 0 ? std::string("no clock") : ticksClock(mode.timeLimitTicks);
      std::snprintf(buf, sizeof buf,
                    "first team to %u points, a zone taken in %.1f s pays a point per %.1f s, %s",
                    static_cast<unsigned>(mode.scoreToWin), secs(mode.captureTicks),
                    secs(mode.hillTicks), clock.c_str());
      return buf;
    }
    case sim::ModeKind::Harvest: {
      // v0.82: bananas carried home and banked; a lone raider takes them back.
      const std::string clock =
          mode.timeLimitTicks == 0 ? std::string("no clock") : ticksClock(mode.timeLimitTicks);
      char steal[64];
      if (mode.stealTicks == 0) {
        std::snprintf(steal, sizeof steal, "no stealing");
      } else {
        std::snprintf(steal, sizeof steal, "a lone raider steals one per %s",
                      beatText(mode.stealTicks).c_str());
      }
      const char* respawn = mode.respawnAmmo == 0 ? ", respawn empty-handed" : "";
      std::snprintf(buf, sizeof buf,
                    "first team to %u bananas banked, one per %s at home, %s%s, %s",
                    static_cast<unsigned>(mode.scoreToWin), beatText(mode.bankTicks).c_str(), steal,
                    respawn, clock.c_str());
      return buf;
    }
    case sim::ModeKind::TeamRounds:
      std::snprintf(buf, sizeof buf, "first team to %u rounds, last team standing",
                    static_cast<unsigned>(mode.roundsToWin));
      return buf;
    case sim::ModeKind::KillRounds:  // v0.90
      std::snprintf(buf, sizeof buf,
                    "first to %u kills, judged at a round's end, last monkey standing",
                    static_cast<unsigned>(mode.scoreToWin));
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
