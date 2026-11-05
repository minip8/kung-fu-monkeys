#include "sdl_input.hpp"

#include <catch2/catch_test_macros.hpp>

using kfm::client::keyFromScancode;
using kfm::client::translateSdlEvent;
using kfm::platform::InputEventKind;
using kfm::platform::Key;
using kfm::platform::PadAxis;
using kfm::platform::PadButton;

namespace {

// The translated event; fails the test when there is none.
kfm::platform::InputEvent must(const SDL_Event& ev) {
  const auto e = translateSdlEvent(ev);
  REQUIRE(e.has_value());
  return e.value_or(kfm::platform::InputEvent{});
}

}  // namespace

TEST_CASE("sdl input: scancodes map to physical keys", "[client][sdl]") {
  CHECK(keyFromScancode(SDL_SCANCODE_A) == Key::A);
  CHECK(keyFromScancode(SDL_SCANCODE_W) == Key::W);
  CHECK(keyFromScancode(SDL_SCANCODE_Z) == Key::Z);
  CHECK(keyFromScancode(SDL_SCANCODE_1) == Key::Num1);
  CHECK(keyFromScancode(SDL_SCANCODE_9) == Key::Num9);
  CHECK(keyFromScancode(SDL_SCANCODE_0) == Key::Num0);
  CHECK(keyFromScancode(SDL_SCANCODE_KP_0) == Key::Kp0);
  CHECK(keyFromScancode(SDL_SCANCODE_KP_7) == Key::Kp7);
  CHECK(keyFromScancode(SDL_SCANCODE_KP_ENTER) == Key::KpEnter);
  CHECK(keyFromScancode(SDL_SCANCODE_F1) == Key::F1);
  CHECK(keyFromScancode(SDL_SCANCODE_F12) == Key::F12);
  CHECK(keyFromScancode(SDL_SCANCODE_UP) == Key::Up);
  CHECK(keyFromScancode(SDL_SCANCODE_RIGHT) == Key::Right);
  CHECK(keyFromScancode(SDL_SCANCODE_SPACE) == Key::Space);
  CHECK(keyFromScancode(SDL_SCANCODE_RETURN) == Key::Enter);
  CHECK(keyFromScancode(SDL_SCANCODE_LSHIFT) == Key::LeftShift);
  CHECK(keyFromScancode(SDL_SCANCODE_RSHIFT) == Key::RightShift);
  CHECK(keyFromScancode(SDL_SCANCODE_RCTRL) == Key::RightCtrl);
  CHECK(keyFromScancode(SDL_SCANCODE_PERIOD) == Key::Period);
  CHECK(keyFromScancode(SDL_SCANCODE_SLASH) == Key::Slash);
  CHECK(keyFromScancode(SDL_SCANCODE_GRAVE) == Key::Grave);
  CHECK(keyFromScancode(SDL_SCANCODE_PAGEDOWN) == Key::PageDown);
  CHECK(keyFromScancode(SDL_SCANCODE_F13) == Key::Unknown);
  CHECK(keyFromScancode(SDL_SCANCODE_CAPSLOCK) == Key::Unknown);
}

TEST_CASE("sdl input: keyboard events, repeats dropped", "[client][sdl]") {
  SDL_Event ev{};
  ev.type = SDL_EVENT_KEY_DOWN;
  ev.key.scancode = SDL_SCANCODE_J;
  ev.common.timestamp = 123456789;
  auto e = must(ev);
  CHECK(e.kind == InputEventKind::KeyDown);
  CHECK(e.key == Key::J);
  CHECK(e.deviceId == kfm::platform::kKeyboardDevice);
  CHECK(e.timestampNs == 123456789);

  ev.key.repeat = true;
  CHECK_FALSE(translateSdlEvent(ev));

  ev.key.repeat = false;
  ev.type = SDL_EVENT_KEY_UP;
  e = must(ev);
  CHECK(e.kind == InputEventKind::KeyUp);

  ev.key.scancode = SDL_SCANCODE_CAPSLOCK;
  CHECK_FALSE(translateSdlEvent(ev));
}

TEST_CASE("sdl input: gamepad events", "[client][sdl]") {
  SDL_Event ev{};
  ev.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
  ev.gbutton.which = 17;
  ev.gbutton.button = SDL_GAMEPAD_BUTTON_WEST;
  auto e = must(ev);
  CHECK(e.kind == InputEventKind::PadButtonDown);
  CHECK(e.deviceId == 17);
  CHECK(e.button == PadButton::West);

  ev.gbutton.button = SDL_GAMEPAD_BUTTON_MISC1;  // beyond the buttons we name
  CHECK_FALSE(translateSdlEvent(ev));

  ev = SDL_Event{};
  ev.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
  ev.gaxis.which = 17;
  ev.gaxis.axis = SDL_GAMEPAD_AXIS_RIGHTY;
  ev.gaxis.value = -32768;
  e = must(ev);
  CHECK(e.kind == InputEventKind::PadAxis);
  CHECK(e.axis == PadAxis::RightY);
  CHECK(e.value == -1.0f);
  ev.gaxis.value = 32767;
  CHECK(must(ev).value == 1.0f);

  ev = SDL_Event{};
  ev.type = SDL_EVENT_GAMEPAD_ADDED;
  ev.gdevice.which = 3;
  e = must(ev);
  CHECK(e.kind == InputEventKind::PadAdded);
  CHECK(e.deviceId == 3);
  ev.type = SDL_EVENT_GAMEPAD_REMOVED;
  CHECK(must(ev).kind == InputEventKind::PadRemoved);

  ev = SDL_Event{};
  ev.type = SDL_EVENT_SENSOR_UPDATE;  // not an input the game reads
  CHECK_FALSE(translateSdlEvent(ev));
}

TEST_CASE("sdl input: mouse motion, buttons and leave (v0.45)", "[client][sdl][mouse]") {
  SDL_Event ev{};
  ev.type = SDL_EVENT_MOUSE_MOTION;
  ev.motion.which = 1;
  ev.motion.x = 320.5f;
  ev.motion.y = 200.0f;
  auto e = must(ev);
  CHECK(e.kind == InputEventKind::MouseMotion);
  CHECK(e.deviceId == kfm::platform::kKeyboardDevice);
  CHECK(e.x == 320.5f);
  CHECK(e.y == 200.0f);
  ev.motion.which = static_cast<SDL_MouseID>(-1);  // SDL_TOUCH_MOUSEID: a finger stays out
  CHECK_FALSE(translateSdlEvent(ev));

  ev = SDL_Event{};
  ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  ev.common.timestamp = 987654321;
  ev.button.which = 1;
  ev.button.button = SDL_BUTTON_LEFT;
  ev.button.x = 12.0f;
  ev.button.y = 34.0f;
  e = must(ev);
  CHECK(e.kind == InputEventKind::MouseButtonDown);
  CHECK(e.key == Key::MouseLeft);
  CHECK(e.deviceId == kfm::platform::kKeyboardDevice);
  CHECK(e.timestampNs == 987654321);  // the press's own stamp: perfect-dash timing
  CHECK(e.x == 12.0f);
  CHECK(e.y == 34.0f);
  ev.type = SDL_EVENT_MOUSE_BUTTON_UP;
  ev.button.button = SDL_BUTTON_RIGHT;
  e = must(ev);
  CHECK(e.kind == InputEventKind::MouseButtonUp);
  CHECK(e.key == Key::MouseRight);
  ev.button.button = SDL_BUTTON_MIDDLE;
  CHECK(must(ev).key == Key::MouseMiddle);
  ev.button.button = SDL_BUTTON_X1;  // side buttons bind nothing
  CHECK_FALSE(translateSdlEvent(ev));
  ev.button.button = SDL_BUTTON_LEFT;
  ev.button.which = static_cast<SDL_MouseID>(-1);  // SDL_TOUCH_MOUSEID
  CHECK_FALSE(translateSdlEvent(ev));

  ev = SDL_Event{};
  ev.type = SDL_EVENT_WINDOW_MOUSE_LEAVE;
  CHECK(must(ev).kind == InputEventKind::MouseLeave);
}
