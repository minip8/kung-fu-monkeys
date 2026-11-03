// InputSampler: platform-neutral InputEvents -> one sim::PlayerInput per local
// player slot per tick (architecture §5.4, §5.11, §6.8, §7 local multiplayer).
// SDL-free and unit-tested headlessly; float math is fine here (not sim).
//
// Per tick, in this order:
//   1. push() every InputEvent received since the previous tick;
//   2. optionally pollJoin() and bind() newly joined devices;
//   3. sampleFrame() (or sample() per slot followed by endTick()) right
//      before sim::step(). endTick() clears this tick's press records; key,
//      button and axis states persist.
//
// Semantics (the decisions the architecture doc records):
//   - A button bit is set if the action is held at sampling time OR was
//     pressed during the tick, so a tap shorter than a tick still yields one
//     pressed tick. The sim edge-detects, so a held button never re-fires.
//   - subTick = clamp((pressNs - tickStartNs) * 256 / tickDurationNs, 0, 255)
//     for the latest press (0 -> 1) edge of any action this tick; releases
//     never set it; 0 without a press edge. Presses stamped before the tick
//     start (late arrivals) clamp to 0.
//   - Digital movement: 8 directions, moveMag 255, opposing keys cancel.
//     Analog: radial deadzone rescaled so the deadzone edge is 0 and full
//     deflection 1; moveMag 1..255 outside the deadzone, exactly 0 inside.
//     SDL's +Y-down is flipped to the sim's +y-up.
//   - Aim: the right stick when outside its deadzone, else the current move
//     direction, else the previous aim (it never snaps back to 0).
#pragma once

#include "kfm/core/types.hpp"
#include "kfm/platform/input_event.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/input.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace kfm::platform {

// Digital actions a profile binds. Move directions only drive movement;
// Throw and Dash map to sim::Button bits.
enum class Action : uint8_t {
  Up,
  Down,
  Left,
  Right,
  Throw,
  Dash,
  kCount,  // NOLINT(readability-identifier-naming): count sentinel
};
inline constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::kCount);

// Up to this many keys per action; unused entries are Key::Unknown.
inline constexpr std::size_t kMaxKeyAliases = 3;
using KeyAliases = std::array<Key, kMaxKeyAliases>;

struct KeyboardProfile {
  std::array<KeyAliases, kActionCount> keys{};  // indexed by Action

  [[nodiscard]] constexpr const KeyAliases& operator[](Action a) const {
    return keys[static_cast<std::size_t>(a)];
  }
  [[nodiscard]] constexpr KeyAliases& operator[](Action a) {
    return keys[static_cast<std::size_t>(a)];
  }
};

// Up to this many pad buttons per action; unused entries are PadButton::kCount.
inline constexpr std::size_t kMaxPadAliases = 2;
using PadAliases = std::array<PadButton, kMaxPadAliases>;

// Left stick moves (dpad is the digital fallback while the stick is inside
// its deadzone), right stick aims. Throw/Dash are pad buttons; move entries
// of `buttons` are the dpad.
struct GamepadProfile {
  std::array<PadAliases, kActionCount> buttons{};  // indexed by Action
  float moveDeadzone{0.2f};                        // radial, fraction of full deflection
  float aimDeadzone{0.2f};

  [[nodiscard]] constexpr const PadAliases& operator[](Action a) const {
    return buttons[static_cast<std::size_t>(a)];
  }
  [[nodiscard]] constexpr PadAliases& operator[](Action a) {
    return buttons[static_cast<std::size_t>(a)];
  }
};

// Default keyboard profile 0 (left side of the keyboard): WASD move,
// throw J or Space, dash K or LeftShift.
[[nodiscard]] KeyboardProfile defaultKeyboardProfile1();
// Default keyboard profile 1 (right side): arrow keys move,
// throw Period or RightCtrl, dash Slash or RightShift.
[[nodiscard]] KeyboardProfile defaultKeyboardProfile2();
// Default gamepad profile: left stick (or dpad) moves, right stick aims,
// throw West or RightShoulder, dash South or LeftShoulder, deadzones 0.2.
[[nodiscard]] GamepadProfile defaultGamepadProfile();

// Number of keyboard profiles the sampler holds (two people per keyboard by
// default; more can be configured).
inline constexpr std::size_t kMaxKeyboardProfiles = 4;
// Pads tracked at once (connected or bound).
inline constexpr std::size_t kMaxPads = 16;

enum class DeviceKind : uint8_t { None, Keyboard, Gamepad };

// What feeds a local player slot: the keyboard read through one profile, or
// one pad by SDL joystick instance id.
struct DeviceBinding {
  DeviceKind kind{DeviceKind::None};
  uint8_t keyboardProfile{0};  // valid when kind == Keyboard
  uint32_t padId{0};           // valid when kind == Gamepad

  [[nodiscard]] static constexpr DeviceBinding keyboard(uint8_t profile) {
    return DeviceBinding{.kind = DeviceKind::Keyboard, .keyboardProfile = profile, .padId = 0};
  }
  [[nodiscard]] static constexpr DeviceBinding pad(uint32_t id) {
    return DeviceBinding{.kind = DeviceKind::Gamepad, .keyboardProfile = 0, .padId = id};
  }
  constexpr bool operator==(const DeviceBinding&) const = default;
};

class InputSampler {
 public:
  // Starts with the two default keyboard profiles in indices 0 and 1, the
  // default gamepad profile, and no slot bound.
  InputSampler();

  // --- configuration -------------------------------------------------------
  void setKeyboardProfile(uint8_t index, const KeyboardProfile& profile);
  [[nodiscard]] const KeyboardProfile& keyboardProfile(uint8_t index) const;
  void setGamepadProfile(const GamepadProfile& profile);
  [[nodiscard]] const GamepadProfile& gamepadProfile() const { return padProfile_; }

  // --- slots ---------------------------------------------------------------
  // Binds `slot` to a device. A device feeds at most one slot: binding it
  // elsewhere unbinds the old slot. Returns false (and changes nothing) for an
  // invalid slot, a keyboard profile index out of range, or kind None.
  // Rebinding resets the slot's retained aim to `initialAim` and clears its
  // dropped flag.
  bool bind(PlayerId slot, DeviceBinding device, uint8_t initialAim = 0);
  void unbind(PlayerId slot);
  [[nodiscard]] DeviceBinding binding(PlayerId slot) const;
  [[nodiscard]] bool isBound(PlayerId slot) const;
  // Bit i set: slot i is bound.
  [[nodiscard]] sim::PlayerMask boundSlots() const;

  // Slots whose pad was removed (PadRemoved) and which were unbound because
  // of it; nothing is rebound automatically. A bit stays set until the slot
  // is bound again or clearDropped() is called.
  [[nodiscard]] sim::PlayerMask droppedSlots() const { return dropped_; }
  void clearDropped() { dropped_ = 0; }

  // Whether a pad is currently connected (PadAdded seen, or any event from
  // it since its last PadRemoved).
  [[nodiscard]] bool isPadConnected(uint32_t padId) const;

  // --- per tick ------------------------------------------------------------
  // Accumulates one event into this tick. Events must be pushed in arrival
  // order; state (held keys, buttons, axes) updates immediately.
  void push(const InputEvent& event);

  // The first device not bound to any slot that pressed a button this tick,
  // in event order: a keyboard profile whose Throw or Dash key went down
  // (lowest profile index on a shared key), or a connected pad on which any
  // button went down. Call before endTick()/sampleFrame().
  [[nodiscard]] std::optional<DeviceBinding> pollJoin() const;

  // The input for `slot` this tick; PlayerInput{} for an unbound slot.
  // tickStartNs is on the InputEvent::timestampNs clock. Updates the slot's
  // retained aim; does not clear the tick's press records (endTick() does).
  [[nodiscard]] sim::PlayerInput sample(PlayerId slot, uint64_t tickStartNs,
                                        uint64_t tickDurationNs);

  // Samples every slot into frame.inputs (unbound slots get PlayerInput{}),
  // then calls endTick(). Leaves frame.tick and frame.present to the caller.
  // Returns boundSlots().
  sim::PlayerMask sampleFrame(sim::InputFrame& frame, uint64_t tickStartNs,
                              uint64_t tickDurationNs);

  // Ends the tick: clears this tick's press records.
  void endTick();

 private:
  struct PadState {
    uint32_t id{0};
    bool inUse{false};
    bool connected{false};
    std::array<bool, static_cast<std::size_t>(PadButton::kCount)> down{};
    std::array<bool, static_cast<std::size_t>(PadButton::kCount)> downAtTickStart{};
    std::array<float, static_cast<std::size_t>(PadAxis::kCount)> axes{};
  };

  struct Slot {
    DeviceBinding device{};
    uint8_t aim{0};
  };

  // Key/button transitions this tick, in order (repeats filtered out).
  struct Transition {
    uint32_t deviceId{0};
    uint16_t code{0};  // Key or PadButton
    bool down{false};
    uint64_t timestampNs{0};
  };

  [[nodiscard]] PadState* findPad(uint32_t id);
  [[nodiscard]] const PadState* findPad(uint32_t id) const;
  PadState* findOrAddPad(uint32_t id);
  [[nodiscard]] std::optional<PlayerId> slotOf(DeviceBinding device) const;

  std::array<KeyboardProfile, kMaxKeyboardProfiles> keyProfiles_{};
  GamepadProfile padProfile_{};
  std::array<Slot, sim::kMaxPlayers> slots_{};
  sim::PlayerMask dropped_{0};

  std::array<bool, static_cast<std::size_t>(Key::kCount)> keyDown_{};
  std::array<bool, static_cast<std::size_t>(Key::kCount)> keyDownAtTickStart_{};
  std::array<PadState, kMaxPads> pads_{};
  std::vector<Transition> transitions_;
};

// Transitional: the placeholder apps/client/main.cpp still calls this. Delete
// together with that placeholder when the platform work unit replaces it.
[[nodiscard]] inline int placeholder() {
  return 1;
}

}  // namespace kfm::platform
