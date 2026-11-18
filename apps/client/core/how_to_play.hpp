// The How to play card (architecture §7, v0.94) as a headless model: what
// each control does and which keys and buttons do it, read from the input
// profiles the sampler plays with (platform::defaultKeyboardProfile1/2,
// defaultGamepadProfile), so the card never drifts from the bindings.
// SDL- and ImGui-free; screens.cpp draws it from the main menu (How to
// play) and from the pause menu.
#pragma once

#include "kfm/platform/input_event.hpp"
#include "kfm/platform/input_sampler.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace kfm::client {

// "W", "Space", "Left click", "Right Shift", "." ...
[[nodiscard]] std::string keyName(platform::Key key);
// The Xbox-layout names of the positional buttons: South "A", East "B",
// West "X", North "Y", the shoulders "LB" / "RB", "Start", "D-pad up" ...
[[nodiscard]] std::string_view padButtonName(platform::PadButton button);

struct ControlRow {
  std::string action;    // "Throw"
  std::string what;      // "Hold to charge (further, faster), let go to throw."
  std::string keyboard;  // the first keyboard profile (keyboard + mouse)
  std::string gamepad;
};

struct HowToPlay {
  std::vector<ControlRow> rows;   // move, aim, throw, recall, dash, ability
  std::string secondKeyboard;     // the second player on the same keyboard, one line
  std::vector<std::string> tips;  // the game in three lines
};

// The card for these profiles. `mouseAim`: Settings > Mouse aim (on, the
// cursor aims for the keyboard + mouse player; off, the move direction).
[[nodiscard]] HowToPlay howToPlay(const platform::KeyboardProfile& keys,
                                  const platform::KeyboardProfile& keysRight,
                                  const platform::GamepadProfile& pad, bool mouseAim);

// v0.94: the short line over the first round's countdown of a local match
// for the keyboard + mouse player ("WASD move · mouse aims · hold J / Left
// click: throw · K / Right click: dash (time it to catch)").
[[nodiscard]] std::string firstRoundHint(const platform::KeyboardProfile& keys, bool mouseAim);

}  // namespace kfm::client
