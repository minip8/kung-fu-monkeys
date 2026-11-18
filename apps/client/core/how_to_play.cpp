#include "how_to_play.hpp"

#include <array>
#include <cstddef>

namespace kfm::client {

using platform::Action;
using platform::Key;
using platform::PadButton;

std::string keyName(Key key) {
  const auto k = static_cast<uint16_t>(key);
  if (key >= Key::A && key <= Key::Z) {
    const std::array<char, 2> c{static_cast<char>('A' + (k - static_cast<uint16_t>(Key::A))), '\0'};
    return c.data();
  }
  if (key >= Key::Num0 && key <= Key::Num9) {
    const std::array<char, 2> c{static_cast<char>('0' + (k - static_cast<uint16_t>(Key::Num0))),
                                '\0'};
    return c.data();
  }
  if (key >= Key::Kp0 && key <= Key::Kp9) {
    return "Keypad " +
           std::string(1, static_cast<char>('0' + (k - static_cast<uint16_t>(Key::Kp0))));
  }
  if (key >= Key::F1 && key <= Key::F12) {
    return "F" + std::to_string(1 + (k - static_cast<uint16_t>(Key::F1)));
  }
  switch (key) {
    case Key::Up:
      return "Up";
    case Key::Down:
      return "Down";
    case Key::Left:
      return "Left";
    case Key::Right:
      return "Right";
    case Key::Space:
      return "Space";
    case Key::Enter:
      return "Enter";
    case Key::Escape:
      return "Esc";
    case Key::Tab:
      return "Tab";
    case Key::Backspace:
      return "Backspace";
    case Key::LeftShift:
      return "Left Shift";
    case Key::RightShift:
      return "Right Shift";
    case Key::LeftCtrl:
      return "Left Ctrl";
    case Key::RightCtrl:
      return "Right Ctrl";
    case Key::LeftAlt:
      return "Left Alt";
    case Key::RightAlt:
      return "Right Alt";
    case Key::Comma:
      return ",";
    case Key::Period:
      return ".";
    case Key::Slash:
      return "/";
    case Key::Semicolon:
      return ";";
    case Key::Apostrophe:
      return "'";
    case Key::Minus:
      return "-";
    case Key::Equals:
      return "=";
    case Key::LeftBracket:
      return "[";
    case Key::RightBracket:
      return "]";
    case Key::Backslash:
      return "\\";
    case Key::Grave:
      return "`";
    case Key::Insert:
      return "Insert";
    case Key::Delete:
      return "Delete";
    case Key::Home:
      return "Home";
    case Key::End:
      return "End";
    case Key::PageUp:
      return "Page Up";
    case Key::PageDown:
      return "Page Down";
    case Key::KpEnter:
      return "Keypad Enter";
    case Key::MouseLeft:
      return "Left click";
    case Key::MouseMiddle:
      return "Middle click";
    case Key::MouseRight:
      return "Right click";
    default:
      break;
  }
  return "?";
}

std::string_view padButtonName(PadButton button) {
  switch (button) {
    case PadButton::South:
      return "A";
    case PadButton::East:
      return "B";
    case PadButton::West:
      return "X";
    case PadButton::North:
      return "Y";
    case PadButton::Back:
      return "Back";
    case PadButton::Guide:
      return "Guide";
    case PadButton::Start:
      return "Start";
    case PadButton::LeftStick:
      return "Left stick press";
    case PadButton::RightStick:
      return "Right stick press";
    case PadButton::LeftShoulder:
      return "LB";
    case PadButton::RightShoulder:
      return "RB";
    case PadButton::DpadUp:
      return "D-pad up";
    case PadButton::DpadDown:
      return "D-pad down";
    case PadButton::DpadLeft:
      return "D-pad left";
    case PadButton::DpadRight:
      return "D-pad right";
    case PadButton::kCount:
      break;
  }
  return "?";
}

namespace {

// "J, Space or Left click": every key bound to the action.
[[nodiscard]] std::string keysOf(const platform::KeyboardProfile& p, Action a) {
  std::vector<std::string> names;
  for (const Key k : p[a]) {
    if (k != Key::Unknown && k < Key::kCount) names.push_back(keyName(k));
  }
  std::string out;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i > 0) out += i + 1 == names.size() ? " or " : ", ";
    out += names[i];
  }
  return out.empty() ? std::string("-") : out;
}

[[nodiscard]] std::string buttonsOf(const platform::GamepadProfile& p, Action a) {
  std::string out;
  for (const PadButton b : p[a]) {
    if (b >= PadButton::kCount) continue;
    if (!out.empty()) out += " or ";
    out += padButtonName(b);
  }
  return out.empty() ? std::string("-") : out;
}

// "WASD" when the four move keys are W, A, S, D; "arrows" for the arrow
// keys; else the four names.
[[nodiscard]] std::string moveKeys(const platform::KeyboardProfile& p) {
  const Key up = p[Action::Up][0];
  const Key left = p[Action::Left][0];
  const Key down = p[Action::Down][0];
  const Key right = p[Action::Right][0];
  if (up == Key::W && left == Key::A && down == Key::S && right == Key::D) return "WASD";
  if (up == Key::Up && left == Key::Left && down == Key::Down && right == Key::Right) {
    return "the arrow keys";
  }
  return keyName(up) + keyName(left) + keyName(down) + keyName(right);
}

}  // namespace

HowToPlay howToPlay(const platform::KeyboardProfile& keys,
                    const platform::KeyboardProfile& keysRight, const platform::GamepadProfile& pad,
                    bool mouseAim) {
  HowToPlay h;
  const bool cursor = keys.mouseAim && mouseAim;
  h.rows.push_back({.action = "Move",
                    .what = "Run around the arena.",
                    .keyboard = moveKeys(keys),
                    .gamepad = "Left stick or the d-pad"});
  h.rows.push_back({.action = "Aim",
                    .what = "Point where your banana goes.",
                    .keyboard = cursor ? std::string("The mouse") : std::string("The way you move"),
                    .gamepad = "Right stick"});
  h.rows.push_back({.action = "Throw",
                    .what = "Hold to charge (further and faster), let go to throw.",
                    .keyboard = keysOf(keys, Action::Throw),
                    .gamepad = buttonsOf(pad, Action::Throw)});
  h.rows.push_back({.action = "Recall",
                    .what = "Empty-handed, hold Throw: your dropped banana flies back to you.",
                    .keyboard = "Hold " + keysOf(keys, Action::Throw),
                    .gamepad = "Hold " + buttonsOf(pad, Action::Throw)});
  h.rows.push_back({.action = "Dash",
                    .what = "A quick dodge. Dash just as a banana reaches you to catch it.",
                    .keyboard = keysOf(keys, Action::Dash),
                    .gamepad = buttonsOf(pad, Action::Dash)});
  h.rows.push_back({.action = "Ability",
                    .what = "Your kit's extra move (Slam, Blink, Parry ...), when it has one.",
                    .keyboard = keysOf(keys, Action::Ability),
                    .gamepad = buttonsOf(pad, Action::Ability)});
  h.secondKeyboard = "A second player on the same keyboard: " + moveKeys(keysRight) +
                     " move (and aim), " + keysOf(keysRight, Action::Throw) + " throw, " +
                     keysOf(keysRight, Action::Dash) + " dash, " +
                     keysOf(keysRight, Action::Ability) + " ability.";
  h.tips = {
      "Bananas fly out and curve back like boomerangs: one hit knocks a monkey out.",
      "Missed? Your banana comes home, or lies where it fell until you pick it up or recall it.",
      "Join local play by pressing Throw or Dash on a free keyboard side, or any button on a pad.",
  };
  return h;
}

namespace {
// The first key bound to the action, and a mouse button when one is too:
// "J / Left click" (the hint has one line).
[[nodiscard]] std::string shortKeys(const platform::KeyboardProfile& p, Action a) {
  std::string first;
  std::string mouse;
  for (const Key k : p[a]) {
    if (k == Key::Unknown || k >= Key::kCount) continue;
    const bool isMouse = k >= Key::MouseLeft && k <= Key::MouseRight;
    if (isMouse && mouse.empty()) mouse = keyName(k);
    if (!isMouse && first.empty()) first = keyName(k);
  }
  if (first.empty()) return mouse.empty() ? std::string("-") : mouse;
  return mouse.empty() ? first : first + " / " + mouse;
}
}  // namespace

std::string firstRoundHint(const platform::KeyboardProfile& keys, bool mouseAim) {
  const bool cursor = keys.mouseAim && mouseAim;
  return moveKeys(keys) + " move  \xC2\xB7  " + (cursor ? "mouse aims" : "aim where you move") +
         "  \xC2\xB7  hold " + shortKeys(keys, Action::Throw) + ": throw  \xC2\xB7  " +
         shortKeys(keys, Action::Dash) + ": dash (time it to catch)";
}

}  // namespace kfm::client
