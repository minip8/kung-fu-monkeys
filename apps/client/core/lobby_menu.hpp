// The game-setup panel as a headless state machine (architecture §7, v0.24;
// v0.97 rebuilt it): the local lobby's and the online room host's, the same
// rows in the same places. SDL- and ImGui-free; hud.cpp draws it and main.cpp
// applies the settings to the MatchSession or sends them as RoomSettings.
//
// v0.97: a fixed list of seven rows, Game (the preset, presets.hpp, or
// "Custom"), Mode, Arena, Kit, Power-ups, Bots and Bot difficulty, that never
// changes with a value: a row that does not apply stays in its place, greyed,
// with a short reason as its value (the Kit under a mode that hands kits out
// by role, Power-ups on an arena without power-up spawners, Bot difficulty
// in an online room, where the server runs the bots), and Up/Down step over
// it. Player preferences (volume, sound, the arena theme) are Settings'
// (menus.hpp), not the panel's. The power-up set is one row (All / None /
// "3 of 5"); its Custom opens the PowerUpPicker, a fixed grid of the arena's
// power-ups over the panel. When one choice forces another (a mode that
// brings its own arena or needs an arena with zones, team sides or a grid),
// the Arena row only ever offers arenas that suit the mode, and notice()
// says in a line what changed and why ("Arena set to Standoff: Team Rounds
// needs a starting side for each team.").
//
// Navigation is generic over devices and buttons: MenuInput turns raw
// platform::InputEvents into Up/Down/Left/Right (keyboard arrows and WASD,
// a pad's d-pad and left stick) and never looks at game bindings, so the
// lobby's join presses (Throw/Dash or any pad button) and any button a
// future ability adds stay free. Up/Down move the focus (wrapping, over the
// enabled rows); Left and Right change the focused value: Game, Mode, Arena,
// Kit and Bot difficulty cycle (wrapping; Mode past the bot contests,
// isBotMode(), Kit past the kits only a mode's roles hand out,
// isRoleLoadout()), Power-ups cycles All, None and Custom (which opens the
// picker), Bots steps and clamps. Enter / pad Start still starts the match
// (MatchSession). v0.42: the online room's host edits the same rows
// (setRoomRows()), v8 its Bots row stops under the server's player cap
// (setPlayerCap()).
#pragma once

#include "kfm/platform/input_event.hpp"
#include "kfm/platform/input_sampler.hpp"
#include "kfm/sim/content.hpp"
#include "lobby_settings.hpp"
#include "presets.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kfm::client {

enum class MenuAction : uint8_t { None = 0, Up, Down, Left, Right };

// v0.97: the panel's rows, always these, in this order.
enum class LobbyRowKind : uint8_t {
  Game = 0,  // the preset (presets.hpp), or Custom
  Mode,
  Arena,
  Loadout,   // "Kit" on screen
  PowerUps,  // All / None / Custom (the picker)
  Bots,
  BotDifficulty,
};
inline constexpr std::array<LobbyRowKind, 7> kLobbyRows{
    LobbyRowKind::Game,     LobbyRowKind::Mode, LobbyRowKind::Arena,        LobbyRowKind::Loadout,
    LobbyRowKind::PowerUps, LobbyRowKind::Bots, LobbyRowKind::BotDifficulty};

// The Power-ups row's value: every power-up the arena can draw, none of
// them, or some (the picker's).
enum class PowerUpChoice : uint8_t { All = 0, None, Custom };

// Raw input -> menu actions. A stick counts once per push past
// kStickPress and re-arms when it falls back under kStickRelease.
class MenuInput {
 public:
  static constexpr float kStickPress = 0.6f;
  static constexpr float kStickRelease = 0.35f;
  static constexpr std::size_t kMaxPads = 8;

  // `deviceBound`: whether the event's device is bound to a lobby slot. An
  // unbound pad's press is its join, so only bound pads navigate; the
  // keyboard always navigates (its navigation keys are not join keys).
  [[nodiscard]] MenuAction translate(const platform::InputEvent& event, bool deviceBound);

 private:
  struct Stick {
    uint32_t device{0};
    bool used{false};
    int8_t x{0};  // -1, 0, +1: the direction latched while held
    int8_t y{0};
  };
  Stick* stickFor(uint32_t device);
  std::array<Stick, kMaxPads> sticks_{};
};

// Enter, keypad Enter or a bound pad's Start: the press that starts the
// match (MatchSession) or, on the online room's waiting screen, the host's
// Start.
[[nodiscard]] bool isActivateEvent(const platform::InputEvent& event, bool deviceBound);

// The `deviceBound` argument of MenuInput::translate(): the keyboard always,
// a pad when the sampler has it bound to a slot.
[[nodiscard]] bool eventDeviceBound(const platform::InputSampler& sampler,
                                    const platform::InputEvent& event);

// Whether the UI keeps `event` from the game: a key press while a text field
// has the keyboard, or (v0.45) a mouse click while the pointer is over a
// window (ImGui's WantCaptureKeyboard / WantCaptureMouse). Releases, motion
// and pads always go through, so nothing is left held.
[[nodiscard]] bool uiCapturesEvent(const platform::InputEvent& event, bool uiWantsKeyboard,
                                   bool uiWantsMouse);

// v0.97: the power-up picker, a fixed grid over the setup panel: one toggle
// per power-up the arena's spawners can draw (content order, two to a row),
// then a row of All, None and Done. Its items do not change while it is
// open (the arena cannot change under it). Up/Down move between rows (the
// column kept where the row has it), Left/Right along a row, both wrapping.
class PowerUpPicker {
 public:
  enum class ItemKind : uint8_t { Toggle = 0, All, None, Done };
  struct Item {
    ItemKind kind{ItemKind::Done};
    uint8_t powerUp{0};  // Toggle: the index into ContentDB::powerUps
    constexpr bool operator==(const Item&) const = default;
  };
  static constexpr std::size_t kColumns = 2;  // toggles per row
  static constexpr std::size_t kMaxItems = sim::kMaxPowerUps + 3;

  // Opens on the arena's power-ups, the focus on the first item.
  void open(const sim::ContentDB& content, uint8_t arena);
  void close() { open_ = false; }
  [[nodiscard]] bool isOpen() const { return open_; }
  [[nodiscard]] uint8_t arena() const { return arena_; }

  [[nodiscard]] std::span<const Item> items() const { return {items_.data(), count_}; }
  [[nodiscard]] std::size_t toggleCount() const { return toggles_; }
  [[nodiscard]] std::size_t focus() const { return focus_; }
  [[nodiscard]] const Item& focusedItem() const { return items_[focus_]; }
  void setFocus(std::size_t index);
  // The grid: the row and column of item `index`, and how many rows.
  [[nodiscard]] std::size_t rowOf(std::size_t index) const;
  [[nodiscard]] std::size_t columnOf(std::size_t index) const;
  [[nodiscard]] std::size_t rowCount() const;

  void apply(MenuAction action);
  // Item `index` pressed: a toggle flips its power-up, All and None set the
  // whole set (sim::kAllPowerUps, 0), Done closes. True when the set changed.
  bool activate(std::size_t index, LobbySettings& settings);
  bool activate(LobbySettings& settings) { return activate(focus_, settings); }

  [[nodiscard]] static const char* label(ItemKind kind);

 private:
  [[nodiscard]] std::size_t rowStart(std::size_t row) const;
  [[nodiscard]] std::size_t rowLength(std::size_t row) const;

  bool open_{false};
  uint8_t arena_{0};
  std::array<Item, kMaxItems> items_{};
  std::size_t count_{0};
  std::size_t toggles_{0};
  std::size_t focus_{0};
};

class LobbyMenu {
 public:
  LobbyMenu();

  // Takes the content: resolves the presets, clamps the settings to it (an
  // arena, mode or loadout index past the content falls back to 0; power-up
  // bits past it are kept, they are harmless; an arena that does not suit
  // the mode gives way to the first that does) and closes the picker. The
  // menu keeps a pointer: `content` must outlive it or the next
  // setContent(). Before the first call the menu uses sim::defaultContent().
  void setContent(const sim::ContentDB& content);
  // v0.43: the online room host's menu: the same rows, Bot difficulty greyed
  // (the server runs the bots). Off: local play.
  void setRoomRows(bool room);
  [[nodiscard]] bool roomRows() const { return roomRows_; }
  // v8 (issue #83): the player cap the Bots row stops under, the server's
  // rule (apps/server validRoomSettings(): at most maxPlayers − 1 bots, one
  // slot stays a human's). The online room sets it from the RoomState's
  // maxPlayers; local play keeps the default, kMaxPlayers. Clamped to
  // 1..kMaxPlayers; a bot count above the new limit drops to it.
  void setPlayerCap(uint8_t maxPlayers);
  // The Bots row's highest value: playerCap − 1.
  [[nodiscard]] uint8_t maxBots() const { return static_cast<uint8_t>(playerCap_ - 1); }
  // v0.97: the humans who play, so a preset's players can be made up with
  // bots (GamePreset::players).
  void setHumans(uint8_t humans) { humans_ = humans; }

  [[nodiscard]] static std::span<const LobbyRowKind> rows() { return kLobbyRows; }
  [[nodiscard]] std::size_t focus() const { return focus_; }
  [[nodiscard]] LobbyRowKind focusedRow() const { return kLobbyRows[focus_]; }
  void setFocus(std::size_t row);
  // v0.97: whether a row applies (a greyed row keeps its place; Up/Down
  // skip it) and, when it does not, why, in a sentence.
  [[nodiscard]] bool rowEnabled(LobbyRowKind row) const;
  [[nodiscard]] std::string rowReason(LobbyRowKind row) const;

  [[nodiscard]] const LobbySettings& settings() const { return settings_; }
  [[nodiscard]] LobbySettings& settings() { return settings_; }

  // Applies one action. Returns true when a setting changed (a focus move
  // alone returns false).
  bool apply(MenuAction action);
  // Left/Right on a given row (the panel's mouse buttons). A greyed row
  // does nothing.
  bool change(std::size_t row, int direction);

  // --- presets (v0.97, presets.hpp) ------------------------------------------
  [[nodiscard]] std::span<const ResolvedPreset> presets() const { return presets_.view(); }
  // The preset the settings play, if any (else the Game row says Custom).
  [[nodiscard]] std::optional<std::size_t> activePreset() const;
  // Applies preset `index` (and the bots it wants). True when a setting changed.
  bool applyPreset(std::size_t index);

  // --- what a change did besides itself (v0.97) -------------------------------
  // One line: "Arena set to Standoff: Team Rounds needs a starting side for
  // each team.", "Added 3 bots: Classic is for 4 monkeys."; empty when the
  // last change did only what was asked. Cleared by the next change.
  [[nodiscard]] const std::string& notice() const { return notice_; }
  void clearNotice() { notice_.clear(); }

  // --- the power-up picker (v0.97) ---------------------------------------------
  [[nodiscard]] PowerUpChoice powerUpChoice() const;
  [[nodiscard]] PowerUpPicker& picker() { return picker_; }
  [[nodiscard]] const PowerUpPicker& picker() const { return picker_; }
  [[nodiscard]] bool pickerOpen() const { return picker_.isOpen(); }
  // Opens it on the arena's power-ups (not when the Power-ups row is greyed).
  void openPicker();

  // --- text for the panel (and the tests) -----------------------------------
  [[nodiscard]] static const char* rowLabel(LobbyRowKind row);
  // The value, or the short reason of a greyed row ("By role", "None in
  // this arena", "Server's choice").
  [[nodiscard]] std::string rowValue(LobbyRowKind row) const;

 private:
  [[nodiscard]] const sim::ContentDB& content() const;
  void clampSettings();
  void changeMode(int direction);

  const sim::ContentDB* content_{nullptr};
  bool roomRows_{false};
  uint8_t playerCap_{sim::kMaxPlayers};
  uint8_t humans_{1};
  std::size_t focus_{0};
  LobbySettings settings_{};
  PresetList presets_{};
  // The preset last picked or played: the Game row's place while it says
  // Custom (none yet: before the first).
  std::optional<std::size_t> lastPreset_;
  std::string notice_;
  PowerUpPicker picker_;
};

// v0.97: what a mode needs of an arena, for the notice when the Arena row
// had to move ("a starting side for each team", "a grid arena"); empty when
// any arena suits it.
[[nodiscard]] std::string_view arenaNeed(const sim::ModeDef& mode);

// The key numbers of a mode, one line: "first to 3 rounds",
// "first to 10 eliminations, 3:00, respawn 2.0 s (1.5 s protected)" (v0.57:
// "points" when the mode has a catch or suicide score),
// "first to 3 rounds of 1:00", v0.82 harvest: "first team to 30 bananas
// banked, one per 0.33 s at home, a lone raider steals one per 0.50 s,
// respawn empty-handed, 1:00" ("no stealing" without a steal beat).
[[nodiscard]] std::string modeSummary(const sim::ModeDef& mode);

// The abilities of a loadout by slot, one line: "dash, throw, slam" (an
// empty third slot is left out).
[[nodiscard]] std::string loadoutSummary(const sim::ContentDB& db, uint8_t loadout);

// v0.82: an arena's spawners by kind: power-up spawners and trees (which
// bear bananas whatever the match's power-up set), and the line the match
// preview gives the trees ("1 tree bears bananas", "4 trees bear bananas").
struct ArenaSpawners {
  uint8_t powerUps{0};
  uint8_t trees{0};
  bool operator==(const ArenaSpawners&) const = default;
};
[[nodiscard]] ArenaSpawners arenaSpawners(const sim::ArenaDef& arena);
[[nodiscard]] std::string treesText(uint8_t trees);

// "m:ss" of a tick count.
[[nodiscard]] std::string ticksClock(uint32_t ticks);
// v0.82: a beat in seconds, "0.03 s" under a second, "1.5 s" from one.
[[nodiscard]] std::string beatText(uint32_t ticks);

}  // namespace kfm::client
