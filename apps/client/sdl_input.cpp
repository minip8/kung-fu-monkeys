#include "sdl_input.hpp"

#include <algorithm>
#include <cstdint>

namespace kfm::client {

namespace {

// SDL_TOUCH_MOUSEID and SDL_PEN_MOUSEID spelled without the headers' C-style
// casts (-Werror=old-style-cast): the mouse events SDL synthesises from touch
// and pen input.
constexpr SDL_MouseID kTouchMouseId = static_cast<SDL_MouseID>(-1);
constexpr SDL_MouseID kPenMouseId = static_cast<SDL_MouseID>(-2);

using platform::InputEvent;
using platform::InputEventKind;
using platform::Key;

[[nodiscard]] Key offset(Key first, int delta) {
  return static_cast<Key>(static_cast<int>(first) + delta);
}

}  // namespace

Key keyFromScancode(SDL_Scancode sc) {
  if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return offset(Key::A, sc - SDL_SCANCODE_A);
  // SDL orders 1..9 then 0; ours is 0..9.
  if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return offset(Key::Num1, sc - SDL_SCANCODE_1);
  if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) {
    return offset(Key::Kp1, sc - SDL_SCANCODE_KP_1);
  }
  if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return offset(Key::F1, sc - SDL_SCANCODE_F1);
  switch (sc) {
    case SDL_SCANCODE_0:
      return Key::Num0;
    case SDL_SCANCODE_KP_0:
      return Key::Kp0;
    case SDL_SCANCODE_KP_ENTER:
      return Key::KpEnter;
    case SDL_SCANCODE_UP:
      return Key::Up;
    case SDL_SCANCODE_DOWN:
      return Key::Down;
    case SDL_SCANCODE_LEFT:
      return Key::Left;
    case SDL_SCANCODE_RIGHT:
      return Key::Right;
    case SDL_SCANCODE_SPACE:
      return Key::Space;
    case SDL_SCANCODE_RETURN:
      return Key::Enter;
    case SDL_SCANCODE_ESCAPE:
      return Key::Escape;
    case SDL_SCANCODE_TAB:
      return Key::Tab;
    case SDL_SCANCODE_BACKSPACE:
      return Key::Backspace;
    case SDL_SCANCODE_LSHIFT:
      return Key::LeftShift;
    case SDL_SCANCODE_RSHIFT:
      return Key::RightShift;
    case SDL_SCANCODE_LCTRL:
      return Key::LeftCtrl;
    case SDL_SCANCODE_RCTRL:
      return Key::RightCtrl;
    case SDL_SCANCODE_LALT:
      return Key::LeftAlt;
    case SDL_SCANCODE_RALT:
      return Key::RightAlt;
    case SDL_SCANCODE_COMMA:
      return Key::Comma;
    case SDL_SCANCODE_PERIOD:
      return Key::Period;
    case SDL_SCANCODE_SLASH:
      return Key::Slash;
    case SDL_SCANCODE_SEMICOLON:
      return Key::Semicolon;
    case SDL_SCANCODE_APOSTROPHE:
      return Key::Apostrophe;
    case SDL_SCANCODE_MINUS:
      return Key::Minus;
    case SDL_SCANCODE_EQUALS:
      return Key::Equals;
    case SDL_SCANCODE_LEFTBRACKET:
      return Key::LeftBracket;
    case SDL_SCANCODE_RIGHTBRACKET:
      return Key::RightBracket;
    case SDL_SCANCODE_BACKSLASH:
      return Key::Backslash;
    case SDL_SCANCODE_GRAVE:
      return Key::Grave;
    case SDL_SCANCODE_INSERT:
      return Key::Insert;
    case SDL_SCANCODE_DELETE:
      return Key::Delete;
    case SDL_SCANCODE_HOME:
      return Key::Home;
    case SDL_SCANCODE_END:
      return Key::End;
    case SDL_SCANCODE_PAGEUP:
      return Key::PageUp;
    case SDL_SCANCODE_PAGEDOWN:
      return Key::PageDown;
    default:
      return Key::Unknown;
  }
}

std::optional<InputEvent> translateSdlEvent(const SDL_Event& ev) {
  InputEvent out{};
  out.timestampNs = ev.common.timestamp;
  switch (ev.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      if (ev.key.repeat) return std::nullopt;
      out.key = keyFromScancode(ev.key.scancode);
      if (out.key == Key::Unknown) return std::nullopt;
      out.kind = ev.type == SDL_EVENT_KEY_DOWN ? InputEventKind::KeyDown : InputEventKind::KeyUp;
      out.deviceId = platform::kKeyboardDevice;
      return out;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
      if (ev.gbutton.button >= static_cast<uint8_t>(platform::PadButton::kCount)) {
        return std::nullopt;
      }
      out.kind = ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? InputEventKind::PadButtonDown
                                                          : InputEventKind::PadButtonUp;
      out.deviceId = ev.gbutton.which;
      out.button = static_cast<platform::PadButton>(ev.gbutton.button);
      return out;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
      if (ev.gaxis.axis >= static_cast<uint8_t>(platform::PadAxis::kCount)) return std::nullopt;
      out.kind = InputEventKind::PadAxis;
      out.deviceId = ev.gaxis.which;
      out.axis = static_cast<platform::PadAxis>(ev.gaxis.axis);
      out.value = std::clamp(static_cast<float>(ev.gaxis.value) / 32767.0f, -1.0f, 1.0f);
      return out;
    }
    case SDL_EVENT_MOUSE_MOTION: {
      if (ev.motion.which == kTouchMouseId || ev.motion.which == kPenMouseId) {
        return std::nullopt;  // touch and pen stay out of mouse aim
      }
      out.kind = InputEventKind::MouseMotion;
      out.deviceId = platform::kKeyboardDevice;
      out.x = ev.motion.x;
      out.y = ev.motion.y;
      return out;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      if (ev.button.which == kTouchMouseId || ev.button.which == kPenMouseId) {
        return std::nullopt;
      }
      switch (ev.button.button) {
        case SDL_BUTTON_LEFT:
          out.key = Key::MouseLeft;
          break;
        case SDL_BUTTON_MIDDLE:
          out.key = Key::MouseMiddle;
          break;
        case SDL_BUTTON_RIGHT:
          out.key = Key::MouseRight;
          break;
        default:
          return std::nullopt;
      }
      out.kind = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? InputEventKind::MouseButtonDown
                                                        : InputEventKind::MouseButtonUp;
      out.deviceId = platform::kKeyboardDevice;
      out.x = ev.button.x;
      out.y = ev.button.y;
      return out;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      out.kind = InputEventKind::MouseLeave;
      out.deviceId = platform::kKeyboardDevice;
      return out;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED: {
      out.kind = ev.type == SDL_EVENT_GAMEPAD_ADDED ? InputEventKind::PadAdded
                                                    : InputEventKind::PadRemoved;
      out.deviceId = ev.gdevice.which;
      return out;
    }
    default:
      return std::nullopt;
  }
}

void GamepadSet::onEvent(const SDL_Event& ev) {
  if (ev.type == SDL_EVENT_GAMEPAD_ADDED) {
    if (SDL_GetGamepadFromID(ev.gdevice.which) != nullptr) return;  // already open
    for (SDL_Gamepad*& slot : pads_) {
      if (slot != nullptr) continue;
      slot = SDL_OpenGamepad(ev.gdevice.which);
      return;
    }
  } else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
    SDL_Gamepad* pad = SDL_GetGamepadFromID(ev.gdevice.which);
    if (pad == nullptr) return;
    for (SDL_Gamepad*& slot : pads_) {
      if (slot == pad) slot = nullptr;
    }
    SDL_CloseGamepad(pad);
  }
}

void GamepadSet::closeAll() {
  for (SDL_Gamepad*& pad : pads_) {
    if (pad != nullptr) SDL_CloseGamepad(pad);
    pad = nullptr;
  }
}

std::size_t GamepadSet::openCount() const {
  return static_cast<std::size_t>(
      std::ranges::count_if(pads_, [](SDL_Gamepad* p) { return p != nullptr; }));
}

}  // namespace kfm::client
