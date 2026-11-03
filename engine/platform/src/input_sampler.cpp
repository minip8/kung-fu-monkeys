// InputSampler (architecture §5.4, §5.11, §7): see the header for the
// per-tick contract and the semantics it implements.
#include "kfm/platform/input_sampler.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kfm::platform {

namespace {

constexpr uint16_t kNoCode = 0xFFFF;
constexpr std::size_t kMaxAliases = std::max(kMaxKeyAliases, kMaxPadAliases);
using AliasCodes = std::array<uint16_t, kMaxAliases>;
using ActionCodes = std::array<AliasCodes, kActionCount>;

constexpr std::size_t idx(Action a) {
  return static_cast<std::size_t>(a);
}

ActionCodes codesOf(const KeyboardProfile& p) {
  ActionCodes out{};
  for (std::size_t a = 0; a < kActionCount; ++a) {
    out[a].fill(kNoCode);
    for (std::size_t k = 0; k < kMaxKeyAliases; ++k) {
      const Key key = p.keys[a][k];
      if (key != Key::Unknown && key < Key::kCount) out[a][k] = static_cast<uint16_t>(key);
    }
  }
  return out;
}

ActionCodes codesOf(const GamepadProfile& p) {
  ActionCodes out{};
  for (std::size_t a = 0; a < kActionCount; ++a) {
    out[a].fill(kNoCode);
    for (std::size_t k = 0; k < kMaxPadAliases; ++k) {
      const PadButton b = p.buttons[a][k];
      if (b < PadButton::kCount) out[a][k] = static_cast<uint16_t>(b);
    }
  }
  return out;
}

struct ActionSample {
  std::array<bool, kActionCount> held{};     // down at the end of the sampled transitions
  std::array<bool, kActionCount> pressed{};  // went 0 -> 1 during the tick
  bool buttonPressed{false};                 // a Throw/Dash press edge happened
  uint64_t lastPressNs{0};                   // stamp of the latest such edge
};

// Replays the first `count` pending transitions of one device against the
// state at tick start, at action granularity: an action is down while any of
// its aliases is down, so pressing a second alias of an already-held action
// is not an edge. `held` is the replayed state, or
// downNow when every pending transition is replayed (`all`).
template <class Transitions, class DownAtStart, class DownNow>
ActionSample sampleActions(const ActionCodes& codes, uint32_t deviceId,
                           const Transitions& transitions, std::size_t count, bool all,
                           DownAtStart downAtStart, DownNow downNow) {
  ActionSample out;
  std::array<std::array<bool, kMaxAliases>, kActionCount> aliasDown{};
  for (std::size_t a = 0; a < kActionCount; ++a) {
    for (std::size_t k = 0; k < kMaxAliases; ++k) {
      const uint16_t c = codes[a][k];
      if (c == kNoCode) continue;
      aliasDown[a][k] = downAtStart(c);
      if (all) out.held[a] = out.held[a] || downNow(c);
    }
  }
  const auto anyDown = [](const std::array<bool, kMaxAliases>& v) {
    return std::ranges::any_of(v, [](bool b) { return b; });
  };
  for (std::size_t j = 0; j < count; ++j) {
    const auto& t = transitions[j];
    if (t.deviceId != deviceId) continue;
    for (std::size_t a = 0; a < kActionCount; ++a) {
      for (std::size_t k = 0; k < kMaxAliases; ++k) {
        if (codes[a][k] != t.code) continue;
        const bool before = anyDown(aliasDown[a]);
        aliasDown[a][k] = t.down;
        if (!before && anyDown(aliasDown[a])) {
          out.pressed[a] = true;
          if (a == idx(Action::Throw) || a == idx(Action::Dash)) {
            out.buttonPressed = true;
            out.lastPressNs = t.timestampNs;
          }
        }
      }
    }
  }
  if (!all) {
    for (std::size_t a = 0; a < kActionCount; ++a)
      out.held[a] = anyDown(aliasDown[a]);
  }
  return out;
}

uint8_t subTickOf(uint64_t pressNs, uint64_t tickStartNs, uint64_t tickDurationNs) {
  if (tickDurationNs == 0 || pressNs <= tickStartNs) return 0;
  const uint64_t d = pressNs - tickStartNs;
  if (d >= tickDurationNs) return 255;
  return static_cast<uint8_t>(std::min<uint64_t>(d * 256 / tickDurationNs, 255));
}

struct Move {
  uint8_t dir{0};
  uint8_t mag{0};  // 0 = idle
};

// 8-way digital movement; opposing directions cancel.
Move digitalMove(bool up, bool down, bool left, bool right) {
  const int dx = (right ? 1 : 0) - (left ? 1 : 0);
  const int dy = (up ? 1 : 0) - (down ? 1 : 0);
  if (dx == 0 && dy == 0) return {};
  // Indexed by (dy + 1) * 3 + (dx + 1); 1/256 turn, 0 = +x, 64 = +y (up).
  constexpr std::array<uint8_t, 9> kDirs{160, 192, 224, 128, 0, 0, 96, 64, 32};
  const int i = ((dy + 1) * 3) + (dx + 1);
  return Move{.dir = kDirs[static_cast<std::size_t>(i)], .mag = 255};
}

// Radial deadzone rescaled to 0..1; x right, ySdl down (flipped to sim +y up).
Move analogMove(float x, float ySdl, float deadzone) {
  const float dz = std::clamp(deadzone, 0.0f, 0.99f);
  const float y = -ySdl;
  const float r = std::hypot(x, y);
  if (!(r > dz)) return {};  // also rejects NaN
  const float m = (std::min(r, 1.0f) - dz) / (1.0f - dz);
  const long mag = std::clamp(std::lround(m * 255.0f), 1L, 255L);
  const float turns = std::atan2(y, x) / (2.0f * std::numbers::pi_v<float>);
  const long dir = ((std::lround(turns * 256.0f) % 256) + 256) % 256;
  return Move{.dir = static_cast<uint8_t>(dir), .mag = static_cast<uint8_t>(mag)};
}

bool validSlot(PlayerId slot) {
  return slot.isValid() && slot.value < sim::kMaxPlayers;
}

}  // namespace

KeyboardProfile defaultKeyboardProfile1() {
  KeyboardProfile p;
  p[Action::Up] = {Key::W, Key::Unknown, Key::Unknown};
  p[Action::Down] = {Key::S, Key::Unknown, Key::Unknown};
  p[Action::Left] = {Key::A, Key::Unknown, Key::Unknown};
  p[Action::Right] = {Key::D, Key::Unknown, Key::Unknown};
  p[Action::Throw] = {Key::J, Key::Space, Key::Unknown};
  p[Action::Dash] = {Key::K, Key::LeftShift, Key::Unknown};
  return p;
}

KeyboardProfile defaultKeyboardProfile2() {
  KeyboardProfile p;
  p[Action::Up] = {Key::Up, Key::Unknown, Key::Unknown};
  p[Action::Down] = {Key::Down, Key::Unknown, Key::Unknown};
  p[Action::Left] = {Key::Left, Key::Unknown, Key::Unknown};
  p[Action::Right] = {Key::Right, Key::Unknown, Key::Unknown};
  p[Action::Throw] = {Key::Period, Key::RightCtrl, Key::Unknown};
  p[Action::Dash] = {Key::Slash, Key::RightShift, Key::Unknown};
  return p;
}

GamepadProfile defaultGamepadProfile() {
  GamepadProfile p;
  p[Action::Up] = {PadButton::DpadUp, PadButton::kCount};
  p[Action::Down] = {PadButton::DpadDown, PadButton::kCount};
  p[Action::Left] = {PadButton::DpadLeft, PadButton::kCount};
  p[Action::Right] = {PadButton::DpadRight, PadButton::kCount};
  p[Action::Throw] = {PadButton::West, PadButton::RightShoulder};
  p[Action::Dash] = {PadButton::South, PadButton::LeftShoulder};
  p.moveDeadzone = 0.2f;
  p.aimDeadzone = 0.2f;
  return p;
}

InputSampler::InputSampler() : padProfile_(defaultGamepadProfile()) {
  for (auto& p : keyProfiles_) {
    for (auto& aliases : p.keys)
      aliases.fill(Key::Unknown);
  }
  keyProfiles_[0] = defaultKeyboardProfile1();
  keyProfiles_[1] = defaultKeyboardProfile2();
  transitions_.reserve(64);
}

void InputSampler::setKeyboardProfile(uint8_t index, const KeyboardProfile& profile) {
  if (index < kMaxKeyboardProfiles) keyProfiles_[index] = profile;
}

const KeyboardProfile& InputSampler::keyboardProfile(uint8_t index) const {
  return keyProfiles_[std::min<std::size_t>(index, kMaxKeyboardProfiles - 1)];
}

void InputSampler::setGamepadProfile(const GamepadProfile& profile) {
  padProfile_ = profile;
}

bool InputSampler::bind(PlayerId slot, DeviceBinding device, uint8_t initialAim) {
  if (!validSlot(slot)) return false;
  if (device.kind == DeviceKind::None) return false;
  if (device.kind == DeviceKind::Keyboard && device.keyboardProfile >= kMaxKeyboardProfiles) {
    return false;
  }
  if (device.kind == DeviceKind::Gamepad && device.padId == kKeyboardDevice) return false;
  if (const auto other = slotOf(device); other && *other != slot) unbind(*other);
  slots_[slot.value] = Slot{.device = device, .aim = initialAim};
  dropped_ = static_cast<sim::PlayerMask>(dropped_ & ~(1u << slot.value));
  return true;
}

void InputSampler::unbind(PlayerId slot) {
  if (validSlot(slot)) slots_[slot.value] = Slot{};
}

DeviceBinding InputSampler::binding(PlayerId slot) const {
  return validSlot(slot) ? slots_[slot.value].device : DeviceBinding{};
}

bool InputSampler::isBound(PlayerId slot) const {
  return binding(slot).kind != DeviceKind::None;
}

sim::PlayerMask InputSampler::boundSlots() const {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < sim::kMaxPlayers; ++i) {
    if (slots_[i].device.kind != DeviceKind::None) mask |= 1u << i;
  }
  return static_cast<sim::PlayerMask>(mask);
}

bool InputSampler::isPadConnected(uint32_t padId) const {
  const PadState* pad = findPad(padId);
  return pad != nullptr && pad->connected;
}

InputSampler::PadState* InputSampler::findPad(uint32_t id) {
  for (auto& p : pads_) {
    if (p.inUse && p.id == id) return &p;
  }
  return nullptr;
}

const InputSampler::PadState* InputSampler::findPad(uint32_t id) const {
  for (const auto& p : pads_) {
    if (p.inUse && p.id == id) return &p;
  }
  return nullptr;
}

InputSampler::PadState* InputSampler::findOrAddPad(uint32_t id) {
  if (PadState* p = findPad(id)) return p;
  for (auto& p : pads_) {
    if (!p.inUse) {
      p = PadState{};
      p.id = id;
      p.inUse = true;
      p.connected = true;
      return &p;
    }
  }
  return nullptr;  // more than kMaxPads pads: ignored
}

std::optional<PlayerId> InputSampler::slotOf(DeviceBinding device) const {
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if (slots_[i].device == device) return PlayerId{i};
  }
  return std::nullopt;
}

void InputSampler::push(const InputEvent& e) {
  switch (e.kind) {
    case InputEventKind::KeyDown:
    case InputEventKind::KeyUp: {
      if (e.key == Key::Unknown || e.key >= Key::kCount) return;
      const bool down = e.kind == InputEventKind::KeyDown;
      auto& state = keyDown_[static_cast<std::size_t>(e.key)];
      if (state == down) return;  // key repeat or unmatched release
      state = down;
      transitions_.push_back(Transition{.deviceId = kKeyboardDevice,
                                        .code = static_cast<uint16_t>(e.key),
                                        .down = down,
                                        .timestampNs = e.timestampNs});
      return;
    }
    case InputEventKind::PadButtonDown:
    case InputEventKind::PadButtonUp: {
      if (e.deviceId == kKeyboardDevice || e.button >= PadButton::kCount) return;
      PadState* pad = findOrAddPad(e.deviceId);
      if (pad == nullptr) return;
      const bool down = e.kind == InputEventKind::PadButtonDown;
      auto& state = pad->down[static_cast<std::size_t>(e.button)];
      if (state == down) return;
      state = down;
      transitions_.push_back(Transition{.deviceId = e.deviceId,
                                        .code = static_cast<uint16_t>(e.button),
                                        .down = down,
                                        .timestampNs = e.timestampNs});
      return;
    }
    case InputEventKind::PadAxis: {
      if (e.deviceId == kKeyboardDevice || e.axis >= PadAxis::kCount) return;
      PadState* pad = findOrAddPad(e.deviceId);
      if (pad == nullptr) return;
      const float v = std::isnan(e.value) ? 0.0f : std::clamp(e.value, -1.0f, 1.0f);
      pad->axes[static_cast<std::size_t>(e.axis)] = v;
      return;
    }
    case InputEventKind::PadAdded: {
      if (e.deviceId != kKeyboardDevice) (void)findOrAddPad(e.deviceId);
      return;
    }
    case InputEventKind::PadRemoved: {
      if (e.deviceId == kKeyboardDevice) return;
      if (PadState* pad = findPad(e.deviceId)) *pad = PadState{};
      if (const auto slot = slotOf(DeviceBinding::pad(e.deviceId))) {
        unbind(*slot);
        dropped_ = static_cast<sim::PlayerMask>(dropped_ | (1u << slot->value));
      }
      return;
    }
  }
}

std::optional<DeviceBinding> InputSampler::pollJoin() const {
  for (const auto& t : transitions_) {
    if (!t.down) continue;
    if (t.deviceId == kKeyboardDevice) {
      for (uint8_t p = 0; p < kMaxKeyboardProfiles; ++p) {
        const DeviceBinding dev = DeviceBinding::keyboard(p);
        if (slotOf(dev)) continue;
        const KeyboardProfile& prof = keyProfiles_[p];
        const auto matches = [&](Action a) {
          return std::ranges::any_of(prof[a], [&](Key k) {
            return k != Key::Unknown && static_cast<uint16_t>(k) == t.code;
          });
        };
        if (matches(Action::Throw) || matches(Action::Dash)) return dev;
      }
    } else {
      const DeviceBinding dev = DeviceBinding::pad(t.deviceId);
      if (isPadConnected(t.deviceId) && !slotOf(dev)) return dev;
    }
  }
  return std::nullopt;
}

sim::PlayerInput InputSampler::sample(PlayerId slot, uint64_t tickStartNs,
                                      uint64_t tickDurationNs) {
  return sampleSlot(slot, tickStartNs, tickDurationNs, transitions_.size());
}

sim::PlayerInput InputSampler::sampleSlot(PlayerId slot, uint64_t tickStartNs,
                                          uint64_t tickDurationNs, std::size_t count) {
  if (!isBound(slot)) return {};
  Slot& s = slots_[slot.value];
  const bool all = count >= transitions_.size();

  ActionSample act;
  Move move;
  std::optional<uint8_t> stickAim;
  if (s.device.kind == DeviceKind::Keyboard) {
    act = sampleActions(
        codesOf(keyProfiles_[s.device.keyboardProfile]), kKeyboardDevice, transitions_, count, all,
        [&](uint16_t c) { return keyDownAtTickStart_[c]; },
        [&](uint16_t c) { return keyDown_[c]; });
  } else if (const PadState* pad = findPad(s.device.padId)) {
    act = sampleActions(
        codesOf(padProfile_), s.device.padId, transitions_, count, all,
        [&](uint16_t c) { return pad->downAtTickStart[c]; },
        [&](uint16_t c) { return pad->down[c]; });
    const auto axis = [&](PadAxis a) { return pad->axes[static_cast<std::size_t>(a)]; };
    move = analogMove(axis(PadAxis::LeftX), axis(PadAxis::LeftY), padProfile_.moveDeadzone);
    const Move aim =
        analogMove(axis(PadAxis::RightX), axis(PadAxis::RightY), padProfile_.aimDeadzone);
    if (aim.mag != 0) stickAim = aim.dir;
  }
  if (move.mag == 0) {
    move = digitalMove(act.held[idx(Action::Up)], act.held[idx(Action::Down)],
                       act.held[idx(Action::Left)], act.held[idx(Action::Right)]);
  }

  if (stickAim) {
    s.aim = *stickAim;
  } else if (move.mag != 0) {
    s.aim = move.dir;
  }

  uint32_t buttons = 0;
  if (act.held[idx(Action::Throw)] || act.pressed[idx(Action::Throw)]) {
    buttons |= sim::buttonBit(sim::Button::Throw);
  }
  if (act.held[idx(Action::Dash)] || act.pressed[idx(Action::Dash)]) {
    buttons |= sim::buttonBit(sim::Button::Dash);
  }

  sim::PlayerInput in;
  in.moveDir = move.dir;
  in.moveMag = move.mag;
  in.aimDir = s.aim;
  in.buttons = static_cast<uint8_t>(buttons);
  in.subTick = act.buttonPressed ? subTickOf(act.lastPressNs, tickStartNs, tickDurationNs) : 0;
  return in;
}

sim::PlayerMask InputSampler::sampleFrame(sim::InputFrame& frame, uint64_t tickStartNs,
                                          uint64_t tickDurationNs, uint64_t tickEndNs) {
  const std::size_t count = countBefore(tickEndNs);
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    frame.inputs[i] = sampleSlot(PlayerId{i}, tickStartNs, tickDurationNs, count);
  }
  endTickAfter(count);
  return boundSlots();
}

std::size_t InputSampler::countBefore(uint64_t endNs) const {
  if (endNs == kNoCutoff) return transitions_.size();
  std::size_t n = 0;
  while (n < transitions_.size() && transitions_[n].timestampNs < endNs)
    ++n;
  return n;
}

void InputSampler::endTick() {
  endTickAfter(transitions_.size());
}

void InputSampler::endTickAfter(std::size_t count) {
  if (count >= transitions_.size()) {
    transitions_.clear();
    keyDownAtTickStart_ = keyDown_;
    for (auto& p : pads_)
      p.downAtTickStart = p.down;
    return;
  }
  // Apply the sampled prefix to the tick-start state; the rest stays pending.
  for (std::size_t j = 0; j < count; ++j) {
    const Transition& t = transitions_[j];
    if (t.deviceId == kKeyboardDevice) {
      keyDownAtTickStart_[t.code] = t.down;
    } else if (PadState* pad = findPad(t.deviceId)) {
      pad->downAtTickStart[t.code] = t.down;
    }
  }
  transitions_.erase(transitions_.begin(),
                     transitions_.begin() + static_cast<std::ptrdiff_t>(count));
}

}  // namespace kfm::platform
