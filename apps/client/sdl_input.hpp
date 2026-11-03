// SDL3 -> kfm::platform::InputEvent translation for kfm_client (architecture
// §7, §13). Physical keys come from scancodes (WASD stays WASD on AZERTY);
// pads use their SDL joystick instance id as deviceId; timestamps are
// SDL_Event::common.timestamp, the clock of Window::nowNs().
#pragma once

#include "kfm/platform/input_event.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <array>
#include <cstddef>
#include <optional>

namespace kfm::client {

// Key::Unknown for every scancode the game does not name.
[[nodiscard]] platform::Key keyFromScancode(SDL_Scancode scancode);

// The InputEvent for an SDL event, or nothing for events the sampler does not
// consume (key repeats, unmapped keys, out-of-range pad buttons or axes,
// everything that is not keyboard or gamepad).
[[nodiscard]] std::optional<platform::InputEvent> translateSdlEvent(const SDL_Event& event);

// Opens gamepads as SDL announces them (SDL_EVENT_GAMEPAD_ADDED) and closes
// them on removal and at shutdown. A gamepad must be open for SDL to deliver
// its button and axis events.
class GamepadSet {
 public:
  static constexpr std::size_t kMaxOpen = 16;

  GamepadSet() = default;
  GamepadSet(const GamepadSet&) = delete;
  GamepadSet& operator=(const GamepadSet&) = delete;
  GamepadSet(GamepadSet&&) = delete;
  GamepadSet& operator=(GamepadSet&&) = delete;
  ~GamepadSet() = default;  // closeAll() must run before SDL_Quit (Window::shutdown)

  void onEvent(const SDL_Event& event);
  void closeAll();
  [[nodiscard]] std::size_t openCount() const;

 private:
  std::array<SDL_Gamepad*, kMaxOpen> pads_{};
};

}  // namespace kfm::client
