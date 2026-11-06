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
// When one frame runs several ticks, sampleFrame(frame, start, dur, endNs)
// takes only the key/button transitions stamped before endNs (the tick's
// wall-clock end) and keeps the later ones for the next tick of the batch,
// so every press is stamped against the tick whose window contains it; the
// batch's last tick passes kNoCutoff and takes everything still pending.
// Analog axes are state, not transitions, and are read as of sampling time.
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
//   - Mouse aim (v0.45): a slot on a keyboard profile with `mouseAim` (the
//     primary profile, keyboard + mouse) aims at the cursor once the mouse
//     has moved or clicked, until it leaves the window (MouseLeave). The
//     sampler knows no world state: before sampling a tick the host sets
//     the cursor's ground point (setCursorGround, sim units) and each slot's
//     aim origin (setAimOrigin, its monkey's position); the aim is the
//     direction from the origin to the ground point. While mouse aim is on,
//     the move direction never sets the aim, and with either point unknown
//     or the cursor within kMinMouseAimDistance of the origin the previous
//     aim stays.
//   - The mouse-aim switch (setMouseAimEnabled, the client's Mouse aim
//     setting): off, no slot aims with the cursor and a mouseAim profile aims
//     by its move direction like any other; the mouse buttons still press.
//     Flipping it either way drops the "the mouse moved" latch, so switched
//     back on the cursor aims only once the mouse moves or clicks again.
//   - The mouse buttons are Keys (Key::MouseLeft...) on the keyboard device:
//     profiles bind them, they edge, stamp subTick and join like keys.
//   - Consumed buttons: consumeButtons() marks a slot's Throw/Dash/Ability presses
//     recorded so far, and the buttons held at that moment, as used up: they
//     set no bit and no subTick until the action has been released. Used for
//     a join press and for presses made while the match was paused, so
//     neither reaches the sim as a press edge.
#pragma once

#include "kfm/core/types.hpp"
#include "kfm/platform/input_event.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/input.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace kfm::platform {

// Digital actions a profile binds. Move directions only drive movement;
// Throw, Dash and (v0.21) Ability, the third ability slot's button, map to
// sim::Button bits.
enum class Action : uint8_t {
  Up,
  Down,
  Left,
  Right,
  Throw,
  Dash,
  Ability,  // sim::Button::Ability1 (the third slot, v0.21)
  kCount,   // NOLINT(readability-identifier-naming): count sentinel
};
inline constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::kCount);

// Up to this many keys per action; unused entries are Key::Unknown.
inline constexpr std::size_t kMaxKeyAliases = 3;
using KeyAliases = std::array<Key, kMaxKeyAliases>;

struct KeyboardProfile {
  std::array<KeyAliases, kActionCount> keys{};  // indexed by Action
  bool mouseAim{false};                         // v0.45: the cursor aims (see the header comment)

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

// Default keyboard profile 0 (left side of the keyboard, plus the mouse):
// WASD move, throw J, Space or the left mouse button, dash K, LeftShift or
// the right mouse button, ability L or E; the cursor aims (v0.45).
[[nodiscard]] KeyboardProfile defaultKeyboardProfile1();
// Default keyboard profile 1 (right side): arrow keys move,
// throw Period or RightCtrl, dash Slash or RightShift, ability Semicolon or
// Apostrophe.
[[nodiscard]] KeyboardProfile defaultKeyboardProfile2();
// Default gamepad profile: left stick (or dpad) moves, right stick aims,
// throw West or RightShoulder, dash South or LeftShoulder, ability North or
// East, deadzones 0.2.
[[nodiscard]] GamepadProfile defaultGamepadProfile();

// Number of keyboard profiles the sampler holds (two people per keyboard by
// default; more can be configured).
inline constexpr std::size_t kMaxKeyboardProfiles = 4;
// Pads tracked at once (connected or bound).
inline constexpr std::size_t kMaxPads = 16;
// sampleFrame() end time that takes every pending transition.
inline constexpr uint64_t kNoCutoff = std::numeric_limits<uint64_t>::max();

enum class DeviceKind : uint8_t { None, Keyboard, Gamepad };

// v0.45, mouse aim. A sim-space point in float (x right, y up), and the
// cursor in window coordinates (InputEvent::x/y).
struct AimPoint {
  float x{0.0f};
  float y{0.0f};
};
struct CursorPos {
  float x{0.0f};
  float y{0.0f};
};
// Closer than this (sim units) to its origin, the cursor keeps the previous aim.
inline constexpr float kMinMouseAimDistance = 0.05f;

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
  // in event order: a keyboard profile whose Throw, Dash or Ability key went down
  // (lowest profile index on a shared key), or a connected pad on which any
  // button went down. Call before endTick()/sampleFrame().
  [[nodiscard]] std::optional<DeviceBinding> pollJoin() const;

  // The input for `slot` this tick, from every pending transition;
  // PlayerInput{} for an unbound slot. tickStartNs is on the
  // InputEvent::timestampNs clock. Updates the slot's retained aim and its
  // consumed buttons; does not clear the tick's press records (endTick() does).
  [[nodiscard]] sim::PlayerInput sample(PlayerId slot, uint64_t tickStartNs,
                                        uint64_t tickDurationNs);

  // Samples every slot into frame.inputs (unbound slots get PlayerInput{})
  // from the pending transitions stamped before tickEndNs, then ends the tick
  // for those; later transitions stay pending for the next tick. Leaves
  // frame.tick and frame.present to the caller. Returns boundSlots().
  sim::PlayerMask sampleFrame(sim::InputFrame& frame, uint64_t tickStartNs, uint64_t tickDurationNs,
                              uint64_t tickEndNs = kNoCutoff);

  // Ends the tick: clears every pending press record.
  void endTick();

  // --- mouse aim (v0.45) ---------------------------------------------------
  // The cursor while mouse aim is on (the switch on, and the mouse moved or
  // clicked since it last left the window or the switch last flipped), else
  // nothing.
  [[nodiscard]] std::optional<CursorPos> cursor() const;
  // Whether `slot` aims with the cursor now: the switch on, bound to a
  // keyboard profile with mouseAim, and mouse aim on.
  [[nodiscard]] bool usesMouseAim(PlayerId slot) const;
  // The mouse-aim switch (on by default; see the header comment). A change
  // either way also forgets that the mouse moved.
  void setMouseAimEnabled(bool enabled);
  [[nodiscard]] bool mouseAimEnabled() const { return mouseAimEnabled_; }
  // The cursor's ground point and a slot's aim origin, set by the host
  // before every sample (nothing = unknown: the slot keeps its aim). Both
  // persist until set again.
  void setCursorGround(std::optional<AimPoint> point) { cursorGround_ = point; }
  void setAimOrigin(PlayerId slot, std::optional<AimPoint> origin);

  // Throw/Dash/Ability presses of `slot` recorded so far and the buttons it holds now
  // are consumed: they set no bit and no subTick until the action has been
  // released. Presses after this call sample normally. bind() clears it.
  void consumeButtons(PlayerId slot);
  // consumeButtons() for every slot.
  void consumeAllButtons();

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
    // Per action (only the button actions are ever set): consumed until released.
    std::array<bool, kActionCount> consumed{};
    // Index into transitions_ at which consumption happened: earlier press
    // edges of a consumed action never count.
    std::size_t consumedFrom{0};
    std::optional<AimPoint> aimOrigin;  // v0.45: setAimOrigin()
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
  [[nodiscard]] sim::PlayerInput sampleSlot(PlayerId slot, uint64_t tickStartNs,
                                            uint64_t tickDurationNs, std::size_t count);
  // Pending transitions stamped before endNs (a prefix, in arrival order).
  [[nodiscard]] std::size_t countBefore(uint64_t endNs) const;
  // Ends the tick for the first `count` pending transitions.
  void endTickAfter(std::size_t count);

  std::array<KeyboardProfile, kMaxKeyboardProfiles> keyProfiles_{};
  GamepadProfile padProfile_{};
  std::array<Slot, sim::kMaxPlayers> slots_{};
  sim::PlayerMask dropped_{0};

  std::array<bool, static_cast<std::size_t>(Key::kCount)> keyDown_{};
  std::array<bool, static_cast<std::size_t>(Key::kCount)> keyDownAtTickStart_{};
  std::array<PadState, kMaxPads> pads_{};
  std::vector<Transition> transitions_;
  // v0.45, mouse aim.
  CursorPos cursor_{};
  bool mouseActive_{false};
  bool mouseAimEnabled_{true};
  std::optional<AimPoint> cursorGround_;
};

}  // namespace kfm::platform
