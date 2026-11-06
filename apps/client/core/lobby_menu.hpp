// The local lobby's settings panel as a headless state machine (architecture
// §7, v0.24): a list of rows (arena, mode, teams, loadout, one toggle per
// power-up, bots, bot difficulty, volume, mute) with a focus, driven by
// MenuActions. SDL- and ImGui-free;
// hud.cpp draws it and main.cpp applies the settings to the MatchSession and
// the audio.
//
// Navigation is generic over devices and buttons: MenuInput turns raw
// platform::InputEvents into Up/Down/Left/Right (keyboard arrows and WASD,
// a pad's d-pad and left stick) and never looks at game bindings, so the
// lobby's join presses (Throw/Dash or any pad button) and any button a
// future ability adds stay free. Up/Down move the focus (wrapping); Left
// and Right change the focused value: arena, mode, loadout and bot difficulty cycle
// (wrapping), a power-up toggles, bots and volume step and clamp, mute toggles. Enter /
// pad Start still starts the match (MatchSession). v0.32: the Teams row only
// shows what the mode gives (ModeDef::teams), Left/Right leave it alone; the
// arena row skips arenas that do not suit the mode
// (ContentDB::compatible), and a mode the arena does not suit moves the
// arena to the first one that does; v0.34: a mode that names its own arena
// (ModeDef::arena) brings it, and a mode's baseline power-up gets no row.
// v0.42: the online room's host edits the same rows; v0.43 gives that menu
// only the rows a RoomSettings carries (setRoomRows()). v0.43 moved the M6a
// Online section (Create room, the region, Join) to the Play online screen
// (online_menu.hpp).
#pragma once

#include "kfm/platform/input_event.hpp"
#include "kfm/platform/input_sampler.hpp"
#include "kfm/sim/content.hpp"
#include "lobby_settings.hpp"

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

enum class LobbyRowKind : uint8_t {
  Arena = 0,
  Mode,
  Teams,    // v0.32: read-only, from the mode
  Loadout,  // v0.31
  PowerUp,
  Bots,
  BotDifficulty,
  Volume,
  Mute,
  Theme,  // v0.52: the arena theme (cosmetic, local play's panel only)
};

struct LobbyRow {
  LobbyRowKind kind{LobbyRowKind::Arena};
  uint8_t powerUp{0};  // PowerUp rows: the index into ContentDB::powerUps
  constexpr bool operator==(const LobbyRow&) const = default;
};

inline constexpr uint8_t kVolumeStep = 10;  // percent per Left/Right

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

class LobbyMenu {
 public:
  static constexpr std::size_t kMaxRows = 9 + sim::kMaxPowerUps;

  LobbyMenu();

  // Rebuilds the rows for this content and clamps the settings to it (an
  // arena, mode or loadout index past the content falls back to 0; power-up bits past
  // it are kept, they are harmless). Keeps the focused row when it still
  // exists. The menu keeps a pointer: `content` must outlive it or the next
  // setContent(). Before the first call the menu uses sim::defaultContent().
  void setContent(const sim::ContentDB& content);
  // v0.43: only the rows a RoomSettings carries (arena, mode, teams,
  // loadout, power-ups, bots): the online room host's menu. Off: every row.
  void setRoomRows(bool room);

  [[nodiscard]] std::span<const LobbyRow> rows() const { return {rows_.data(), rowCount_}; }
  [[nodiscard]] std::size_t focus() const { return focus_; }
  [[nodiscard]] const LobbyRow& focusedRow() const { return rows_[focus_]; }
  void setFocus(std::size_t row);

  [[nodiscard]] const LobbySettings& settings() const { return settings_; }
  [[nodiscard]] LobbySettings& settings() { return settings_; }

  // Applies one action. Returns true when a setting changed (a focus move
  // alone returns false).
  bool apply(MenuAction action);
  // Left/Right on a given row (the panel's mouse buttons).
  bool change(std::size_t row, int direction);

  // --- text for the panel (and the tests) -----------------------------------
  [[nodiscard]] std::string rowLabel(const LobbyRow& row) const;
  [[nodiscard]] std::string rowValue(const LobbyRow& row) const;

 private:
  [[nodiscard]] const sim::ContentDB& content() const;
  void rebuildRows();

  const sim::ContentDB* content_{nullptr};
  bool roomRows_{false};
  std::array<LobbyRow, kMaxRows> rows_{};
  std::size_t rowCount_{0};
  std::size_t focus_{0};
  LobbySettings settings_{};
};

// The key numbers of a mode, one line: "first to 3 rounds",
// "first to 10 eliminations, 3:00, respawn 2.0 s (1.5 s protected)",
// "first to 3 rounds of 1:00".
[[nodiscard]] std::string modeSummary(const sim::ModeDef& mode);

// The abilities of a loadout by slot, one line: "dash, throw, slam" (an
// empty third slot is left out).
[[nodiscard]] std::string loadoutSummary(const sim::ContentDB& db, uint8_t loadout);

// "m:ss" of a tick count.
[[nodiscard]] std::string ticksClock(uint32_t ticks);

}  // namespace kfm::client
