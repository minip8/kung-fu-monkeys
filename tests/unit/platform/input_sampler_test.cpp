#include "kfm/platform/input_sampler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

using kfm::PlayerId;
using kfm::platform::DeviceBinding;
using kfm::platform::InputEvent;
using kfm::platform::InputEventKind;
using kfm::platform::InputSampler;
using kfm::platform::Key;
using kfm::platform::PadAxis;
using kfm::platform::PadButton;
using kfm::sim::Button;
using kfm::sim::PlayerInput;

namespace {

constexpr uint64_t kDur = 16'000'000;  // a round tick length keeps the arithmetic readable
constexpr uint64_t kT0 = 1'000'000'000;
constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};

constexpr uint64_t tickStart(uint64_t n) {
  return kT0 + (n * kDur);
}

InputEvent key(Key k, bool down, uint64_t ts = kT0) {
  InputEvent e;
  e.kind = down ? InputEventKind::KeyDown : InputEventKind::KeyUp;
  e.key = k;
  e.timestampNs = ts;
  return e;
}

InputEvent padButton(uint32_t id, PadButton b, bool down, uint64_t ts = kT0) {
  InputEvent e;
  e.kind = down ? InputEventKind::PadButtonDown : InputEventKind::PadButtonUp;
  e.deviceId = id;
  e.button = b;
  e.timestampNs = ts;
  return e;
}

InputEvent padAxis(uint32_t id, PadAxis a, float v) {
  InputEvent e;
  e.kind = InputEventKind::PadAxis;
  e.deviceId = id;
  e.axis = a;
  e.value = v;
  return e;
}

InputEvent padEvent(uint32_t id, InputEventKind kind) {
  InputEvent e;
  e.kind = kind;
  e.deviceId = id;
  return e;
}

// Samples one slot for tick n and ends the tick.
PlayerInput tick(InputSampler& s, PlayerId slot, uint64_t n) {
  const PlayerInput in = s.sample(slot, tickStart(n), kDur);
  s.endTick();
  return in;
}

PlayerInput withKeys(const std::vector<Key>& keys) {
  InputSampler s;
  REQUIRE(s.bind(kP0, DeviceBinding::keyboard(0)));
  for (const Key k : keys)
    s.push(key(k, true));
  return tick(s, kP0, 0);
}

}  // namespace

TEST_CASE("digital directions and diagonals", "[input]") {
  struct Case {
    std::vector<Key> keys;
    uint8_t dir;
  };
  const Case cases[] = {
      {.keys = {Key::D}, .dir = 0},   {.keys = {Key::W, Key::D}, .dir = 32},
      {.keys = {Key::W}, .dir = 64},  {.keys = {Key::W, Key::A}, .dir = 96},
      {.keys = {Key::A}, .dir = 128}, {.keys = {Key::S, Key::A}, .dir = 160},
      {.keys = {Key::S}, .dir = 192}, {.keys = {Key::S, Key::D}, .dir = 224},
  };
  for (const Case& c : cases) {
    const PlayerInput in = withKeys(c.keys);
    CHECK(in.moveDir == c.dir);
    CHECK(in.moveMag == 255);
    CHECK(in.aimDir == c.dir);
    CHECK(in.buttons == 0);
  }
}

TEST_CASE("opposing keys cancel", "[input]") {
  CHECK(withKeys({Key::W, Key::S}).moveMag == 0);
  CHECK(withKeys({Key::A, Key::D}).moveMag == 0);
  CHECK(withKeys({Key::W, Key::A, Key::S, Key::D}).moveMag == 0);
  const PlayerInput in = withKeys({Key::A, Key::D, Key::W});
  CHECK(in.moveMag == 255);
  CHECK(in.moveDir == 64);
}

TEST_CASE("analog deadzone edge and rescale", "[input]") {
  InputSampler s;
  REQUIRE(s.bind(kP0, DeviceBinding::pad(7)));
  s.push(padEvent(7, InputEventKind::PadAdded));
  const auto moveAt = [&](float x, float y) {
    s.push(padAxis(7, PadAxis::LeftX, x));
    s.push(padAxis(7, PadAxis::LeftY, y));
    return tick(s, kP0, 0);
  };
  CHECK(moveAt(0.0f, 0.0f).moveMag == 0);
  CHECK(moveAt(0.2f, 0.0f).moveMag == 0);    // on the edge: still inside
  CHECK(moveAt(0.15f, 0.1f).moveMag == 0);   // radial, r ~ 0.18
  CHECK(moveAt(0.201f, 0.0f).moveMag == 1);  // just outside: never 0
  CHECK(moveAt(0.6f, 0.0f).moveMag == 128);  // (0.6 - 0.2) / 0.8 = 0.5
  CHECK(moveAt(1.0f, 0.0f).moveMag == 255);
  CHECK(moveAt(1.0f, 1.0f).moveMag == 255);  // corners clamp to full
}

TEST_CASE("analog cardinal directions flip SDL's Y", "[input]") {
  InputSampler s;
  REQUIRE(s.bind(kP0, DeviceBinding::pad(7)));
  const auto dirAt = [&](float x, float y) {
    s.push(padAxis(7, PadAxis::LeftX, x));
    s.push(padAxis(7, PadAxis::LeftY, y));
    const PlayerInput in = tick(s, kP0, 0);
    CHECK(in.moveMag == 255);
    return in.moveDir;
  };
  CHECK(dirAt(1.0f, 0.0f) == 0);      // right
  CHECK(dirAt(0.0f, -1.0f) == 64);    // SDL up (-Y) is sim +y
  CHECK(dirAt(-1.0f, 0.0f) == 128);   // left
  CHECK(dirAt(0.0f, 1.0f) == 192);    // SDL down
  CHECK(dirAt(0.75f, -0.75f) == 32);  // up-right
}

TEST_CASE("dpad is the digital fallback while the stick is idle", "[input]") {
  InputSampler s;
  REQUIRE(s.bind(kP0, DeviceBinding::pad(7)));
  s.push(padButton(7, PadButton::DpadUp, true));
  s.push(padButton(7, PadButton::DpadLeft, true));
  PlayerInput in = tick(s, kP0, 0);
  CHECK(in.moveDir == 96);
  CHECK(in.moveMag == 255);
  s.push(padAxis(7, PadAxis::LeftX, 1.0f));  // the stick wins when deflected
  in = tick(s, kP0, 1);
  CHECK(in.moveDir == 0);
}

TEST_CASE("subTick stamps the latest press edge", "[input]") {
  InputSampler s;
  REQUIRE(s.bind(kP0, DeviceBinding::keyboard(0)));

  SECTION("press at mid-tick") {
    s.push(key(Key::J, true, tickStart(0) + (kDur / 2)));
    const PlayerInput in = tick(s, kP0, 0);
    CHECK(in.has(Button::Throw));
    CHECK(in.subTick == 128);
  }
  SECTION("press stamped before tick start clamps to 0") {
    s.push(key(Key::J, true, tickStart(0) - 5));
    const PlayerInput in = tick(s, kP0, 0);
    CHECK(in.has(Button::Throw));
    CHECK(in.subTick == 0);
  }
  SECTION("press stamped after the tick clamps to 255") {
    s.push(key(Key::J, true, tickStart(0) + (kDur * 3)));
    CHECK(tick(s, kP0, 0).subTick == 255);
  }
  SECTION("two presses: the latest wins") {
    s.push(key(Key::J, true, tickStart(0) + (kDur / 4)));
    s.push(key(Key::K, true, tickStart(0) + (kDur * 3 / 4)));
    const PlayerInput in = tick(s, kP0, 0);
    CHECK(in.has(Button::Throw));
    CHECK(in.has(Button::Dash));
    CHECK(in.subTick == 192);
  }
  SECTION("release-only tick is 0") {
    s.push(key(Key::J, true, tickStart(0) + (kDur / 2)));
    CHECK(tick(s, kP0, 0).subTick == 128);
    s.push(key(Key::J, false, tickStart(1) + (kDur / 2)));
    const PlayerInput in = tick(s, kP0, 1);
    CHECK(in.buttons == 0);
    CHECK(in.subTick == 0);
  }
  SECTION("a release after a press does not move the stamp") {
    s.push(key(Key::J, true, tickStart(0) + (kDur / 4)));
    s.push(key(Key::K, true, tickStart(0) + (kDur / 2)));
    s.push(key(Key::K, false, tickStart(0) + (kDur * 3 / 4)));
    CHECK(tick(s, kP0, 0).subTick == 128);
  }
  SECTION("movement keys are not button edges") {
    s.push(key(Key::W, true, tickStart(0) + (kDur / 2)));
    CHECK(tick(s, kP0, 0).subTick == 0);
  }
  SECTION("a second alias of a held action is not an edge") {
    s.push(key(Key::Space, true, tickStart(0) + (kDur / 4)));
    CHECK(tick(s, kP0, 0).subTick == 64);
    s.push(key(Key::J, true, tickStart(1) + (kDur / 2)));
    const PlayerInput in = tick(s, kP0, 1);
    CHECK(in.has(Button::Throw));
    CHECK(in.subTick == 0);
  }
}

TEST_CASE("a tap inside one tick is one pressed tick, then released", "[input]") {
  InputSampler s;
  REQUIRE(s.bind(kP0, DeviceBinding::keyboard(0)));
  s.push(key(Key::K, true, tickStart(0) + (kDur / 4)));
  s.push(key(Key::K, false, tickStart(0) + (kDur / 2)));
  PlayerInput in = tick(s, kP0, 0);
  CHECK(in.buttons == kfm::sim::buttonBit(Button::Dash));
  CHECK(in.subTick == 64);
  in = tick(s, kP0, 1);
  CHECK(in.buttons == 0);
  CHECK(in.subTick == 0);
}

TEST_CASE("a held key keeps its bit and never changes subTick again", "[input]") {
  InputSampler s;
  REQUIRE(s.bind(kP0, DeviceBinding::keyboard(0)));
  s.push(key(Key::J, true, tickStart(0) + (kDur / 2)));
  CHECK(tick(s, kP0, 0).subTick == 128);
  for (uint64_t n = 1; n < 4; ++n) {
    s.push(key(Key::J, true, tickStart(n) + (kDur / 4)));  // OS key repeat: ignored
    const PlayerInput in = tick(s, kP0, n);
    CHECK(in.has(Button::Throw));
    CHECK(in.subTick == 0);
  }
}

TEST_CASE("aim is retained while idle", "[input]") {
  SECTION("keyboard aim follows movement") {
    InputSampler s;
    REQUIRE(s.bind(kP0, DeviceBinding::keyboard(0)));
    s.push(key(Key::A, true));
    CHECK(tick(s, kP0, 0).aimDir == 128);
    s.push(key(Key::A, false));
    const PlayerInput in = tick(s, kP0, 1);
    CHECK(in.moveMag == 0);
    CHECK(in.aimDir == 128);
    CHECK(tick(s, kP0, 2).aimDir == 128);
  }
  SECTION("pad: right stick, then move direction, then retained") {
    InputSampler s;
    REQUIRE(s.bind(kP0, DeviceBinding::pad(7)));
    s.push(padAxis(7, PadAxis::RightX, -1.0f));
    s.push(padAxis(7, PadAxis::LeftY, 1.0f));  // move down, aim left
    PlayerInput in = tick(s, kP0, 0);
    CHECK(in.moveDir == 192);
    CHECK(in.aimDir == 128);
    s.push(padAxis(7, PadAxis::RightX, 0.1f));  // right stick back inside its deadzone
    in = tick(s, kP0, 1);
    CHECK(in.aimDir == 192);  // falls back to the move direction
    s.push(padAxis(7, PadAxis::LeftY, 0.0f));
    in = tick(s, kP0, 2);
    CHECK(in.moveMag == 0);
    CHECK(in.aimDir == 192);  // all idle: kept, never reset to 0
  }
  SECTION("bind sets the initial aim") {
    InputSampler s;
    REQUIRE(s.bind(kP0, DeviceBinding::keyboard(0), 200));
    CHECK(tick(s, kP0, 0).aimDir == 200);
  }
}

TEST_CASE("two keyboard profiles sample independently", "[input]") {
  InputSampler s;
  REQUIRE(s.bind(kP0, DeviceBinding::keyboard(0)));
  REQUIRE(s.bind(kP1, DeviceBinding::keyboard(1)));
  s.push(key(Key::W, true));
  s.push(key(Key::Left, true));
  s.push(key(Key::Period, true, tickStart(0) + (kDur / 2)));
  const PlayerInput a = s.sample(kP0, tickStart(0), kDur);
  const PlayerInput b = s.sample(kP1, tickStart(0), kDur);
  s.endTick();
  CHECK(a.moveDir == 64);
  CHECK(a.buttons == 0);
  CHECK(a.subTick == 0);
  CHECK(b.moveDir == 128);
  CHECK(b.buttons == kfm::sim::buttonBit(Button::Throw));
  CHECK(b.subTick == 128);

  s.push(key(Key::LeftShift, true));
  const PlayerInput a2 = s.sample(kP0, tickStart(1), kDur);
  const PlayerInput b2 = s.sample(kP1, tickStart(1), kDur);
  s.endTick();
  CHECK(a2.buttons == kfm::sim::buttonBit(Button::Dash));
  CHECK(b2.buttons == kfm::sim::buttonBit(Button::Throw));
}

TEST_CASE("two pads by device id", "[input]") {
  InputSampler s;
  constexpr PlayerId kP2{2};
  constexpr PlayerId kP3{3};
  s.push(padEvent(7, InputEventKind::PadAdded));
  s.push(padEvent(9, InputEventKind::PadAdded));
  REQUIRE(s.bind(kP2, DeviceBinding::pad(7)));
  REQUIRE(s.bind(kP3, DeviceBinding::pad(9)));
  s.push(padButton(9, PadButton::South, true, tickStart(0) + (kDur / 2)));
  s.push(padAxis(9, PadAxis::LeftX, -1.0f));
  s.push(padButton(7, PadButton::RightShoulder, true));

  kfm::sim::InputFrame frame;
  CHECK(s.sampleFrame(frame, tickStart(0), kDur) == 0b1100);
  const PlayerInput& a = frame.inputs[2];
  const PlayerInput& b = frame.inputs[3];
  CHECK(a.buttons == kfm::sim::buttonBit(Button::Throw));
  CHECK(a.moveMag == 0);
  CHECK(b.buttons == kfm::sim::buttonBit(Button::Dash));
  CHECK(b.subTick == 128);
  CHECK(b.moveDir == 128);
  CHECK(frame.inputs[0] == PlayerInput{});
}

TEST_CASE("a device feeds at most one slot", "[input]") {
  InputSampler s;
  REQUIRE(s.bind(kP0, DeviceBinding::keyboard(0)));
  REQUIRE(s.bind(kP1, DeviceBinding::keyboard(0)));
  CHECK_FALSE(s.isBound(kP0));
  CHECK(s.binding(kP1) == DeviceBinding::keyboard(0));
  CHECK_FALSE(s.bind(PlayerId{kfm::sim::kMaxPlayers}, DeviceBinding::keyboard(1)));
  CHECK_FALSE(s.bind(kP0, DeviceBinding::keyboard(kfm::platform::kMaxKeyboardProfiles)));
  CHECK_FALSE(s.bind(kP0, DeviceBinding{}));
}

TEST_CASE("join detection", "[input]") {
  InputSampler s;
  CHECK_FALSE(s.pollJoin().has_value());

  s.push(key(Key::W, true));  // movement is not a join press
  CHECK_FALSE(s.pollJoin().has_value());
  s.push(key(Key::K, true));
  CHECK(s.pollJoin() == std::optional{DeviceBinding::keyboard(0)});
  REQUIRE(s.bind(kP0, s.pollJoin().value_or(DeviceBinding{})));
  CHECK_FALSE(s.pollJoin().has_value());  // already bound: not reported
  s.endTick();

  s.push(padEvent(5, InputEventKind::PadAdded));
  s.push(padButton(5, PadButton::Start, true));
  s.push(key(Key::Slash, true));
  CHECK(s.pollJoin() == std::optional{DeviceBinding::pad(5)});  // first in event order
  REQUIRE(s.bind(kP1, DeviceBinding::pad(5)));
  CHECK(s.pollJoin() == std::optional{DeviceBinding::keyboard(1)});
  s.endTick();
  CHECK_FALSE(s.pollJoin().has_value());  // presses only count in their tick
}

TEST_CASE("pad removal unbinds its slot", "[input]") {
  InputSampler s;
  s.push(padEvent(5, InputEventKind::PadAdded));
  REQUIRE(s.bind(kP1, DeviceBinding::pad(5)));
  s.push(padButton(5, PadButton::West, true));
  CHECK(tick(s, kP1, 0).has(Button::Throw));
  CHECK(s.isPadConnected(5));

  s.push(padEvent(5, InputEventKind::PadRemoved));
  CHECK_FALSE(s.isPadConnected(5));
  CHECK_FALSE(s.isBound(kP1));
  CHECK(s.droppedSlots() == 0b10);
  CHECK(tick(s, kP1, 1) == PlayerInput{});

  s.push(padEvent(6, InputEventKind::PadAdded));  // not rebound automatically
  CHECK_FALSE(s.isBound(kP1));
  REQUIRE(s.bind(kP1, DeviceBinding::pad(6)));
  CHECK(s.droppedSlots() == 0);
}

TEST_CASE("sampleFrame keeps transitions after the tick end for the next tick", "[input]") {
  InputSampler s;
  REQUIRE(s.bind(kP0, DeviceBinding::keyboard(0)));
  // Throw tapped in tick 0; Dash pressed in tick 1 and held.
  s.push(key(Key::J, true, tickStart(0) + (kDur / 4)));
  s.push(key(Key::J, false, tickStart(0) + (kDur / 2)));
  s.push(key(Key::K, true, tickStart(1) + (kDur / 2)));
  kfm::sim::InputFrame f{};
  (void)s.sampleFrame(f, tickStart(0), kDur, tickStart(1));
  CHECK(f.inputs[0].buttons == kfm::sim::buttonBit(Button::Throw));
  CHECK(f.inputs[0].subTick == 64);
  (void)s.sampleFrame(f, tickStart(1), kDur);
  CHECK(f.inputs[0].buttons == kfm::sim::buttonBit(Button::Dash));
  CHECK(f.inputs[0].subTick == 128);
  (void)s.sampleFrame(f, tickStart(2), kDur);
  CHECK(f.inputs[0].buttons == kfm::sim::buttonBit(Button::Dash));  // held, no new edge
  CHECK(f.inputs[0].subTick == 0);
}
