#include "core/match_session.hpp"

#include "kfm/core/rng.hpp"
#include "kfm/platform/input_event.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace kfm;
using client::MatchSession;
using client::SessionConfig;
using client::SessionPhase;
using client::SlotOwner;
using platform::InputEvent;
using platform::InputEventKind;
using platform::Key;
using platform::PadButton;

namespace {

constexpr uint64_t kT0 = 1'000'000'000ULL;

// Wall time at which exactly k ticks are due since `base` (ceil keeps the
// accumulator from falling short by rounding).
[[nodiscard]] constexpr uint64_t timeAtTick(uint64_t base, uint64_t k) {
  return base + ((k * 1'000'000'000ULL) + 59) / 60;
}

// Drives a session in whole ticks per frame on an exact clock.
struct Driver {
  std::unique_ptr<MatchSession> s;
  uint64_t base{kT0};
  uint64_t ticksDue{0};

  explicit Driver(const SessionConfig& c) : s(std::make_unique<MatchSession>(c)) {}

  [[nodiscard]] uint64_t now() const { return timeAtTick(base, ticksDue); }
  // The match clock starts at the current time: count ticks from there.
  void rebase() {
    base = now();
    ticksDue = 0;
  }
  void start() {
    REQUIRE(s->startMatch(now()));
    rebase();
  }
  // One frame that makes n ticks due.
  void frame(uint64_t n) {
    const bool lobby = s->phase() == SessionPhase::Lobby;
    ticksDue += n;
    s->advance(now());
    if (lobby && s->phase() == SessionPhase::Match) rebase();
  }
  void push(InputEvent e) const {
    e.timestampNs = now();
    s->push(e);
  }
};

[[nodiscard]] InputEvent key(Key k, bool down) {
  InputEvent e{};
  e.kind = down ? InputEventKind::KeyDown : InputEventKind::KeyUp;
  e.key = k;
  return e;
}

[[nodiscard]] InputEvent padButton(uint32_t id, PadButton b, bool down) {
  InputEvent e{};
  e.kind = down ? InputEventKind::PadButtonDown : InputEventKind::PadButtonUp;
  e.deviceId = id;
  e.button = b;
  return e;
}

[[nodiscard]] InputEvent padEvent(uint32_t id, InputEventKind kind) {
  InputEvent e{};
  e.kind = kind;
  e.deviceId = id;
  return e;
}

[[nodiscard]] bool sameEvent(const sim::SimEvent& a, const sim::SimEvent& b) {
  return a.kind == b.kind && a.tick == b.tick && a.player == b.player && a.entity == b.entity &&
         a.a == b.a && a.b == b.b;
}

[[nodiscard]] bool idle(const sim::PlayerInput& in) {
  return in.moveMag == 0 && in.buttons == 0;
}

}  // namespace

TEST_CASE("session: starts in the lobby with keyboard 0 on slot 0", "[client][session]") {
  Driver d(SessionConfig{.seed = 7, .bots = 2});
  CHECK(d.s->phase() == SessionPhase::Lobby);
  CHECK(d.s->sampler().binding(PlayerId{0}) == platform::DeviceBinding::keyboard(0));
  CHECK(d.s->lobbyPresentMask() == 0b111);
  CHECK(d.s->owner(PlayerId{0}) == SlotOwner::Local);
  CHECK(d.s->owner(PlayerId{1}) == SlotOwner::Bot);
  CHECK(d.s->owner(PlayerId{3}) == SlotOwner::None);
  // The lobby never steps the world.
  d.frame(10);
  CHECK(d.s->curr().tick.value == 0);
  CHECK(d.s->frameEvents().empty());
}

TEST_CASE("session: lobby joins take the next free slot", "[client][session]") {
  Driver d(SessionConfig{.seed = 7, .bots = 1});
  // Keyboard right side: Period is profile 1's Throw.
  d.push(key(Key::Period, true));
  d.frame(1);
  CHECK(d.s->sampler().binding(PlayerId{1}) == platform::DeviceBinding::keyboard(1));
  // A pad joins with any button.
  d.push(padEvent(42, InputEventKind::PadAdded));
  d.push(padButton(42, PadButton::South, true));
  d.frame(1);
  CHECK(d.s->sampler().binding(PlayerId{2}) == platform::DeviceBinding::pad(42));
  // Movement keys do not join; the bot fills the slot after the humans.
  d.push(key(Key::Num1, true));
  d.frame(1);
  CHECK(d.s->sampler().boundSlots() == 0b111);
  CHECK(d.s->lobbyPresentMask() == 0b1111);
  CHECK(d.s->owner(PlayerId{3}) == SlotOwner::Bot);
}

TEST_CASE("session: Enter or pad Start begins the match", "[client][session]") {
  Driver d(SessionConfig{.seed = 7, .bots = 1});
  d.push(key(Key::Enter, true));
  d.frame(1);
  REQUIRE(d.s->phase() == SessionPhase::Match);
  CHECK(d.s->curr().rules.present == 0b11);
  CHECK(d.s->localMask() == 0b1);
  CHECK(d.s->botMask() == 0b10);
  // The match clock starts at the frame that started it.
  d.frame(2);
  CHECK(d.s->curr().tick.value == 2);

  Driver p(SessionConfig{.seed = 7, .bots = 1});
  p.push(padEvent(5, InputEventKind::PadAdded));
  p.push(padButton(5, PadButton::Start, true));
  p.frame(1);
  REQUIRE(p.s->phase() == SessionPhase::Match);
  CHECK(p.s->localMask() == 0b11);  // the pad joined on its Start press
  CHECK(p.s->curr().rules.present == 0b111);
}

TEST_CASE("session: an empty lobby cannot start", "[client][session]") {
  Driver d(SessionConfig{.seed = 7, .bots = 0, .bindKeyboardToSlot0 = false});
  CHECK_FALSE(d.s->startMatch(d.now()));
  CHECK(d.s->phase() == SessionPhase::Lobby);
}

TEST_CASE("session: accumulator stepping, cap, pause and time scale", "[client][session]") {
  Driver d(SessionConfig{.seed = 3, .bots = 1});
  d.start();
  d.frame(0);
  CHECK(d.s->ticksThisFrame() == 0);
  d.frame(3);
  CHECK(d.s->ticksThisFrame() == 3);
  CHECK(d.s->curr().tick.value == 3);
  d.frame(10);  // capped at 4, 6 dropped
  CHECK(d.s->ticksThisFrame() == 4);
  CHECK(d.s->curr().tick.value == 7);
  CHECK(d.s->clock().stats().droppedTicks == 6);

  d.s->setPaused(true);
  d.frame(2);
  CHECK(d.s->curr().tick.value == 7);
  d.s->requestSingleStep();
  d.frame(1);
  CHECK(d.s->curr().tick.value == 8);
  d.s->setPaused(false);

  d.s->setTimeScale(0.5);
  d.frame(2);  // two ticks of wall time at half speed
  CHECK(d.s->curr().tick.value == 9);
}

TEST_CASE("session: a pushed press lands in the next tick's frame", "[client][session]") {
  Driver d(SessionConfig{.seed = 3, .bots = 1});
  d.start();
  d.frame(1);
  CHECK(d.s->lastFrame().inputs[0].buttons == 0);

  // Pressed and released within one frame that steps nothing: the press
  // still reaches the next tick.
  d.push(key(Key::J, true));
  d.push(key(Key::J, false));
  d.s->advance(d.now() + 1'000'000);
  CHECK(d.s->ticksThisFrame() == 0);
  d.frame(1);
  REQUIRE(d.s->ticksThisFrame() == 1);
  const sim::InputFrame& f = d.s->lastFrame();
  CHECK(f.tick.value == 1);
  CHECK(f.present == 0b11);
  CHECK(f.inputs[0].has(sim::Button::Throw));
  d.frame(1);
  CHECK_FALSE(d.s->lastFrame().inputs[0].has(sim::Button::Throw));

  // A held movement key shows up every tick.
  d.push(key(Key::D, true));
  d.frame(2);
  CHECK(d.s->lastFrame().inputs[0].moveMag == 255);
  CHECK(d.s->lastFrame().inputs[0].moveDir == 0);  // +x
}

namespace {

struct StampStats {
  int presses{0};
  int stamped255{0};
  int wrongBy2OrMore{0};
  double mean{0.0};
};

// One Throw press per tick, spread evenly over the tick's window, on frames
// of `ticksPerFrame` ticks whose clock carries a leftover accumulator of
// `phase` ticks. Returns how the presses were stamped.
[[nodiscard]] StampStats stampPresses(double phase, uint32_t ticksPerFrame, int frames) {
  MatchSession s(SessionConfig{.seed = 3, .bots = 1});
  constexpr uint64_t kStart = 2'000'000'000ULL;
  REQUIRE(s.startMatch(kStart));
  // Wall time at which `ticks` (fractional) ticks have accumulated.
  const auto at = [&](double ticks) {
    return kStart + static_cast<uint64_t>(std::llround(ticks * 1e9 / 60.0));
  };
  s.advance(at(phase));  // sets the leftover; no tick due yet
  REQUIRE(s.ticksThisFrame() == 0);

  StampStats st;
  long sum = 0;
  const int total = frames * static_cast<int>(ticksPerFrame);
  int n = 0;
  for (int f = 0; f < frames; ++f) {
    const double frameStart = phase + (static_cast<double>(f) * ticksPerFrame);
    // Each tick of this frame gets one press at fraction `frac` of its window.
    std::vector<double> fracs;
    for (uint32_t t = 0; t < ticksPerFrame; ++t, ++n) {
      const double frac = (static_cast<double>(n) + 0.5) / total;
      fracs.push_back(frac);
      InputEvent down = key(Key::J, true);
      down.timestampNs = at(frameStart + t + frac);
      s.push(down);
      InputEvent up = key(Key::J, false);
      up.timestampNs = at(frameStart + t + frac) + 1;
      s.push(up);
    }
    s.advance(at(frameStart + ticksPerFrame));
    REQUIRE(s.ticksThisFrame() == ticksPerFrame);
    // Only the last tick's frame is visible: check its stamp.
    const sim::PlayerInput in = s.lastFrame().inputs[0];
    REQUIRE(in.has(sim::Button::Throw));
    const double frac = fracs.back();
    const int expected = static_cast<int>(frac * 256.0);
    ++st.presses;
    if (in.subTick == 255 && frac < 255.0 / 256.0) ++st.stamped255;
    if (std::abs(static_cast<int>(in.subTick) - expected) >= 2) ++st.wrongBy2OrMore;
    sum += in.subTick;
  }
  st.mean = static_cast<double>(sum) / st.presses;
  return st;
}

}  // namespace

TEST_CASE("session: presses are stamped against the tick whose window holds them",
          "[client][session]") {
  // 60 Hz frames with a non-zero leftover accumulator: every press lands in
  // its own tick's window whatever the phase (it used to clamp to 255 for a
  // share of presses equal to the phase).
  for (const double phase : {0.06, 0.48, 0.90}) {
    CAPTURE(phase);
    const StampStats st = stampPresses(phase, 1, 600);
    CHECK(st.presses == 600);
    CHECK(st.stamped255 == 0);
    CHECK(st.wrongBy2OrMore == 0);
    CHECK(st.mean > 108.0);
    CHECK(st.mean < 148.0);
  }
  // Two ticks per frame (a 30 Hz display): each press goes to the tick whose
  // window contains it, not all to the first.
  for (const double phase : {0.02, 0.48}) {
    CAPTURE(phase);
    const StampStats st = stampPresses(phase, 2, 300);
    CHECK(st.stamped255 == 0);
    CHECK(st.wrongBy2OrMore == 0);
    CHECK(st.mean > 108.0);
    CHECK(st.mean < 148.0);
  }
}

TEST_CASE("session: a frame of two ticks splits presses by timestamp", "[client][session]") {
  MatchSession s(SessionConfig{.seed = 3, .bots = 1});
  constexpr uint64_t kStart = 2'000'000'000ULL;
  REQUIRE(s.startMatch(kStart));
  const uint64_t t1 = kStart + 16'666'667;
  const uint64_t t2 = kStart + 33'333'334;
  // Throw tapped in the first tick, Dash pressed (and held) in the second.
  InputEvent j = key(Key::J, true);
  j.timestampNs = kStart + 4'166'667;  // a quarter into tick 0
  s.push(j);
  j = key(Key::J, false);
  j.timestampNs = kStart + 8'000'000;
  s.push(j);
  InputEvent k = key(Key::K, true);
  k.timestampNs = t1 + 8'333'333;  // half way into tick 1
  s.push(k);
  s.advance(t2);
  REQUIRE(s.ticksThisFrame() == 2);
  // Tick 0 (the state before the last step) saw only the Throw tap, stamped
  // a quarter in; the Dash was not down yet.
  const sim::Player& p0 = s.prev().players.items[0];
  CHECK(p0.prevButtons == sim::buttonBit(sim::Button::Throw));
  CHECK(static_cast<int>(p0.intent.subTick) == 63);
  const sim::PlayerInput last = s.lastFrame().inputs[0];
  CHECK(last.has(sim::Button::Dash));
  CHECK_FALSE(last.has(sim::Button::Throw));
  CHECK(static_cast<int>(last.subTick) == 127);
}

TEST_CASE("session: bots fill the non-local present slots", "[client][session]") {
  Driver d(SessionConfig{.seed = 11, .bots = 3});
  d.start();
  CHECK(d.s->localMask() == 0b0001);
  CHECK(d.s->botMask() == 0b1110);
  std::array<bool, sim::kMaxPlayers> active{};
  for (int i = 0; i < 300; ++i) {
    d.frame(1);
    const sim::InputFrame& f = d.s->lastFrame();
    CHECK(idle(f.inputs[0]));  // the local player touches nothing
    for (uint8_t slot = 1; slot < 4; ++slot)
      active[slot] = active[slot] || !idle(f.inputs[slot]);
    for (uint8_t slot = 4; slot < sim::kMaxPlayers; ++slot)
      CHECK(idle(f.inputs[slot]));
  }
  CHECK(active[1]);
  CHECK(active[2]);
  CHECK(active[3]);
}

TEST_CASE("session: restart with the same seed reproduces the checksum", "[client][session]") {
  Driver d(SessionConfig{.seed = 99, .bots = 4});
  d.start();
  for (int i = 0; i < 400; ++i)
    d.frame(1);
  const uint32_t first = d.s->checksum();
  const uint32_t tick = d.s->curr().tick.value;

  d.s->restart(d.now(), false);
  d.rebase();
  CHECK(d.s->curr().tick.value == 0);
  for (int i = 0; i < 400; ++i)
    d.frame(1);
  CHECK(d.s->curr().tick.value == tick);
  CHECK(d.s->checksum() == first);

  const uint64_t oldSeed = d.s->seed();
  d.s->restart(d.now(), true);
  d.rebase();
  CHECK(d.s->seed() != oldSeed);
  for (int i = 0; i < 400; ++i)
    d.frame(1);
  CHECK(d.s->checksum() != first);
}

TEST_CASE("session: identical seeds and events stay checksum-identical", "[client][session]") {
  const SessionConfig cfg{.seed = 1234, .botSeed = 77, .bots = 2};
  Driver a(cfg);
  Driver b(cfg);
  for (Driver* d : {&a, &b}) {
    d->push(key(Key::Slash, true));  // keyboard right joins slot 1
    d->push(key(Key::Slash, false));
    d->frame(1);
    d->start();
  }
  REQUIRE(a.s->localMask() == 0b011);

  Rng script = Rng::seeded(5);
  constexpr std::array<Key, 12> kKeys = {Key::W,    Key::A,     Key::S,      Key::D,
                                         Key::J,    Key::K,     Key::Up,     Key::Down,
                                         Key::Left, Key::Right, Key::Period, Key::Slash};
  std::array<bool, kKeys.size()> down{};
  for (int t = 0; t < 600; ++t) {
    if (script.below(4) == 0) {
      const uint32_t k = script.below(static_cast<uint32_t>(kKeys.size()));
      down[k] = !down[k];
      const InputEvent e = key(kKeys[k], down[k]);
      a.push(e);
      b.push(e);
    }
    const uint64_t n = (t % 3 == 0) ? 2 : 1;  // vary ticks per frame too
    a.frame(n);
    b.frame(n);
    REQUIRE(a.s->curr().tick == b.s->curr().tick);
    REQUIRE(a.s->checksum() == b.s->checksum());
  }
  CHECK(a.s->curr().tick.value >= 600);
}

TEST_CASE("session: each tick's events are delivered exactly once", "[client][session]") {
  const SessionConfig cfg{.seed = 21, .bots = 5, .bindKeyboardToSlot0 = false};
  Driver ref(cfg);
  Driver var(cfg);
  ref.start();
  var.start();

  std::vector<sim::SimEvent> expected;
  std::vector<sim::SimEvent> got;
  constexpr std::array<uint64_t, 3> kPattern = {0, 1, 3};
  uint64_t frames = 0;
  while (var.s->curr().tick.value < 900) {
    const uint64_t n = kPattern[frames++ % kPattern.size()];
    const uint32_t before = var.s->curr().tick.value;
    var.frame(n);
    REQUIRE(var.s->ticksThisFrame() == n);
    if (n == 0) CHECK(var.s->frameEvents().empty());
    for (const sim::SimEvent& e : var.s->frameEvents()) {
      CHECK(e.tick.value >= before);
      CHECK(e.tick.value < var.s->curr().tick.value);
      got.push_back(e);
    }
  }
  while (ref.s->curr().tick.value < var.s->curr().tick.value) {
    ref.frame(1);
    for (const sim::SimEvent& e : ref.s->frameEvents())
      expected.push_back(e);
  }
  REQUIRE(ref.s->checksum() == var.s->checksum());
  REQUIRE(got.size() == expected.size());
  for (std::size_t i = 0; i < got.size(); ++i)
    CHECK(sameEvent(got[i], expected[i]));
  CHECK(var.s->droppedEvents() == 0);
  // The match produced real events, including the start.
  REQUIRE_FALSE(got.empty());
  CHECK(got.front().kind == sim::EventKind::MatchStarted);
}

TEST_CASE("session: a dropped pad repeats its last input until rebound", "[client][session]") {
  Driver d(SessionConfig{.seed = 5, .bots = 1, .bindKeyboardToSlot0 = false});
  d.push(padEvent(9, InputEventKind::PadAdded));
  d.push(padButton(9, PadButton::South, true));
  d.frame(1);
  REQUIRE(d.s->sampler().binding(PlayerId{0}) == platform::DeviceBinding::pad(9));
  d.push(padButton(9, PadButton::South, false));
  d.start();

  // Hold the stick right, then pull the pad.
  InputEvent axis{};
  axis.kind = InputEventKind::PadAxis;
  axis.deviceId = 9;
  axis.axis = platform::PadAxis::LeftX;
  axis.value = 1.0f;
  d.push(axis);
  d.frame(2);
  const sim::PlayerInput held = d.s->lastFrame().inputs[0];
  CHECK(held.moveMag == 255);

  d.push(padEvent(9, InputEventKind::PadRemoved));
  d.frame(1);
  CHECK(d.s->owner(PlayerId{0}) == SlotOwner::Dropped);
  CHECK(d.s->droppedMask() == 0b1);
  CHECK(d.s->lastFrame().inputs[0] == held);
  d.frame(3);
  CHECK(d.s->lastFrame().inputs[0] == held);

  // A free device pressing a button takes the slot over.
  d.push(key(Key::K, true));
  d.frame(1);
  CHECK(d.s->owner(PlayerId{0}) == SlotOwner::Local);
  CHECK(d.s->sampler().binding(PlayerId{0}) == platform::DeviceBinding::keyboard(0));
  CHECK(d.s->droppedMask() == 0);
  CHECK(d.s->lastFrame().inputs[0].moveMag == 0);
}

TEST_CASE("session: restart turns a dropped slot into a bot", "[client][session]") {
  Driver d(SessionConfig{.seed = 5, .bots = 1, .bindKeyboardToSlot0 = false});
  d.push(padEvent(9, InputEventKind::PadAdded));
  d.push(padButton(9, PadButton::South, true));
  d.frame(1);
  d.start();
  d.frame(1);
  d.push(padEvent(9, InputEventKind::PadRemoved));
  d.frame(1);
  REQUIRE(d.s->owner(PlayerId{0}) == SlotOwner::Dropped);
  d.s->restart(d.now(), false);
  CHECK(d.s->owner(PlayerId{0}) == SlotOwner::Bot);
  CHECK(d.s->botMask() == 0b11);
  CHECK(d.s->droppedMask() == 0);
}

TEST_CASE("session: after the match ends, Enter returns to the lobby", "[client][session]") {
  Driver d(SessionConfig{.seed = 8, .bots = 3});
  d.s->content().mode.roundsToWin = 1;
  d.start();
  // Enter does nothing while the match is running.
  d.push(key(Key::Enter, true));
  d.push(key(Key::Enter, false));
  d.frame(1);
  REQUIRE(d.s->phase() == SessionPhase::Match);

  int guard = 0;
  while (d.s->curr().rules.phase != sim::MatchPhase::MatchEnded && guard++ < 20000)
    d.frame(1);
  REQUIRE(d.s->curr().rules.phase == sim::MatchPhase::MatchEnded);
  d.push(key(Key::Enter, true));
  d.frame(1);
  CHECK(d.s->phase() == SessionPhase::Lobby);
  // Bindings and the bot count are kept.
  CHECK(d.s->sampler().binding(PlayerId{0}) == platform::DeviceBinding::keyboard(0));
  CHECK(d.s->botCount() == 3);
}

TEST_CASE("session: interpolation policy", "[client][session]") {
  Driver d(SessionConfig{.seed = 2, .bots = 1});
  d.start();
  // 30 Hz frames: interpolate with the accumulator fraction.
  uint64_t t = d.now();
  for (int i = 0; i < 30; ++i) {
    t += 33'333'333;
    d.s->advance(t);
  }
  CHECK_FALSE(d.s->rendersNewest());
  CHECK(&d.s->renderPrev() == &d.s->prev());
  CHECK(d.s->renderAlpha() == d.s->clock().alpha());
  // 120 Hz frames: draw the newest tick.
  for (int i = 0; i < 120; ++i) {
    t += 8'333'333;
    d.s->advance(t);
  }
  CHECK(d.s->rendersNewest());
  CHECK(&d.s->renderPrev() == &d.s->curr());
  CHECK(d.s->renderAlpha() == 1.0f);
  // Overrides.
  d.s->setInterpolationPolicy(client::InterpolationPolicy::Interpolate);
  CHECK_FALSE(d.s->rendersNewest());
  d.s->setInterpolationPolicy(client::InterpolationPolicy::Newest);
  CHECK(d.s->rendersNewest());
}

TEST_CASE("session: content edits apply live and reset", "[client][session]") {
  Driver d(SessionConfig{.seed = 2, .bots = 1});
  CHECK(d.s->contentIsDefault());
  d.s->content().player.moveSpeed = Fixed::fromRaw(sim::defaultContent().player.moveSpeed.raw * 2);
  CHECK_FALSE(d.s->contentIsDefault());
  CHECK(d.s->contentHash() != sim::contentHash(sim::defaultContent()));
  d.s->resetContent();
  CHECK(d.s->contentIsDefault());
}
