// Platform-neutral input events (architecture §7, §13). The SDL glue in
// kfm_platform translates SDL_Events into InputEvent; InputSampler consumes
// them. Nothing in this header knows about SDL: the key names are ours and the
// SDL scancode mapping lives in the glue.
#pragma once

#include <cstdint>

namespace kfm::platform {

// Physical keys (positions, not layout-dependent characters: the glue maps
// SDL scancodes, so WASD stays WASD on AZERTY).
// uint16_t leaves room past 256 keys (pinned with the SDL glue).
// clang-format off
enum class Key : uint16_t {  // NOLINT(performance-enum-size)
  Unknown,
  A, B, C, D, E, F, G, H, I, J, K, L, M,
  N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
  Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
  Up, Down, Left, Right,
  Space, Enter, Escape, Tab, Backspace,
  LeftShift, RightShift, LeftCtrl, RightCtrl, LeftAlt, RightAlt,
  Comma, Period, Slash, Semicolon, Apostrophe,
  Minus, Equals, LeftBracket, RightBracket, Backslash, Grave,
  Insert, Delete, Home, End, PageUp, PageDown,
  Kp0, Kp1, Kp2, Kp3, Kp4, Kp5, Kp6, Kp7, Kp8, Kp9, KpEnter,
  F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
  kCount,  // NOLINT(readability-identifier-naming): count sentinel
};
// clang-format on

// Same order as SDL3's SDL_GamepadButton (positional names: South is A on an
// Xbox pad, Cross on a PlayStation pad), so the glue may cast directly for
// values below kCount.
enum class PadButton : uint8_t {
  South,
  East,
  West,
  North,
  Back,
  Guide,
  Start,
  LeftStick,
  RightStick,
  LeftShoulder,
  RightShoulder,
  DpadUp,
  DpadDown,
  DpadLeft,
  DpadRight,
  kCount,  // NOLINT(readability-identifier-naming): count sentinel
};

// Same order as SDL3's SDL_GamepadAxis.
enum class PadAxis : uint8_t {
  LeftX,
  LeftY,
  RightX,
  RightY,
  LeftTrigger,
  RightTrigger,
  kCount,  // NOLINT(readability-identifier-naming): count sentinel
};

enum class InputEventKind : uint8_t {
  KeyDown,
  KeyUp,
  PadButtonDown,
  PadButtonUp,
  PadAxis,
  PadAdded,
  PadRemoved,
};

// Keyboard device id. Pads use their SDL joystick instance id, which SDL
// guarantees is never 0.
inline constexpr uint32_t kKeyboardDevice = 0;

// One input event. Only the fields relevant to `kind` are read:
//   KeyDown/KeyUp            key
//   PadButtonDown/Up         deviceId, button
//   PadAxis                  deviceId, axis, value
//   PadAdded/PadRemoved      deviceId
// Key repeats (SDL's `repeat` flag) may be forwarded or dropped: a KeyDown for
// a key that is already down is ignored.
struct InputEvent {
  InputEventKind kind{InputEventKind::KeyDown};
  uint32_t deviceId{kKeyboardDevice};  // 0 = keyboard; SDL joystick instance id for pads
  Key key{Key::Unknown};
  PadButton button{PadButton::kCount};
  PadAxis axis{PadAxis::kCount};
  float value{0.0f};        // axis in [-1, 1], SDL's sign: +Y is down; triggers 0..1
  uint64_t timestampNs{0};  // SDL_GetTicksNS clock (SDL_Event::common.timestamp)
};

}  // namespace kfm::platform
