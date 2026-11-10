#include "core/match_session.hpp"

#include "core/menus.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/platform/input_event.hpp"
#include "kfm/presentation/camera.hpp"
#include "kfm/presentation/world_basis.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>
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
  d.s->editContent().modes[0].roundsToWin = 1;
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

TEST_CASE("session: a deathmatch ends on its clock, restarts, and Enter returns to the lobby",
          "[client][session][modes]") {
  Driver d(SessionConfig{.seed = 8, .bots = 3});
  sim::ModeDef& dm = d.s->editContent().modes[1];
  dm.name = sim::ContentName::of("deathmatch");
  dm.kind = sim::ModeKind::Deathmatch;
  dm.timeLimitTicks = 20 * sim::kTickHz;
  dm.scoreToWin = 50;  // the clock decides
  d.s->editContent().modeCount = 2;
  d.s->setMode(1);
  REQUIRE(d.s->mode() == 1);
  d.start();

  const auto playToEnd = [&] {
    int guard = 0;
    while (d.s->curr().rules.phase != sim::MatchPhase::MatchEnded && guard++ < 3000)
      d.frame(1);
    REQUIRE(d.s->curr().rules.phase == sim::MatchPhase::MatchEnded);
    CHECK(d.s->curr().rules.round == 1);  // one long round
  };
  playToEnd();
  // A restart is a fresh deathmatch with the same mode.
  d.s->restart(d.now(), true);
  d.rebase();
  CHECK(d.s->mode() == 1);
  CHECK(d.s->curr().rules.phase != sim::MatchPhase::MatchEnded);
  CHECK_FALSE(d.s->curr().rules.hasModeCounters());
  playToEnd();
  d.push(key(Key::Enter, true));
  d.frame(1);
  CHECK(d.s->phase() == SessionPhase::Lobby);
  CHECK(d.s->mode() == 1);
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
  d.s->editContent().player.moveSpeed =
      Fixed::fromRaw(sim::defaultContent().player.moveSpeed.raw * 2);
  CHECK_FALSE(d.s->contentIsDefault());
  CHECK(d.s->contentHash() != sim::contentHash(sim::defaultContent()));
  d.s->resetContent();
  CHECK(d.s->contentIsDefault());
}

TEST_CASE("session: readers get the content const; editContent() reaches the bots",
          "[client][session][bots]") {
  // Drawing code holds a mutable session (drawLobby, the overlay): content()
  // must be const there too, so a read can never mark anything stale.
  STATIC_REQUIRE(
      std::is_same_v<decltype(std::declval<MatchSession&>().content()), const sim::ContentDB&>);
  STATIC_REQUIRE(
      std::is_same_v<decltype(std::declval<MatchSession&>().editContent()), sim::ContentDB&>);
  Driver d(SessionConfig{.seed = 2, .bots = 1, .bindKeyboardToSlot0 = false});
  d.start();
  d.frame(1);
  const PlayerId bot{0};
  REQUIRE(d.s->agents().has(bot));
  CHECK(d.s->agents().lastObservation().match.contentHash == d.s->contentHash());
  // The live edit (the overlay's tuning) is seen by the bot's next observation.
  d.s->editContent().player.moveSpeed =
      Fixed::fromRaw(sim::defaultContent().player.moveSpeed.raw * 2);
  REQUIRE(d.s->contentHash() != sim::contentHash(sim::defaultContent()));
  d.frame(1);
  CHECK(d.s->agents().lastObservation().match.contentHash == d.s->contentHash());
}

namespace {

// The default content plus a small second arena and a quick second mode.
sim::ContentDB twoArenas() {
  sim::ContentDB c = sim::defaultContent();
  sim::ArenaDef& pit = c.arenas[1];
  pit.name = sim::ContentName::of("pit");
  pit.halfExtents = Vec2{Fixed::fromInt(4), Fixed::fromInt(3)};
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i)
    pit.spawnPoints[i] = Vec2{Fixed::fromInt(i % 3) - Fixed::one(), Fixed::zero()};
  pit.spawnPointCount = sim::kMaxPlayers;
  c.arenaCount = 2;
  c.modes[1] = c.modes[0];
  c.modes[1].name = sim::ContentName::of("quick");
  c.modes[1].roundStartTicks = 5;
  c.modeCount = 2;
  return c;
}

}  // namespace

TEST_CASE("session: arena and mode select the match's content", "[client][session]") {
  const sim::ContentDB content = twoArenas();
  Driver d(SessionConfig{.seed = 4, .arena = 1, .mode = 1, .content = &content, .bots = 2});
  CHECK(d.s->matchConfig().arena == 1);
  CHECK(&d.s->matchContent().arena == &d.s->content().arenas[1]);
  d.start();
  CHECK(d.s->matchConfig().arena == 1);
  CHECK(d.s->matchConfig().mode == 1);
  for (int i = 0; i < 120; ++i)
    d.frame(1);
  const Fixed limit = Fixed::fromInt(4) - content.player.radius;
  for (const auto& p : d.s->curr().players.items) {
    CHECK(abs(p.transform.pos.x) <= limit);
  }
  // Out-of-range indices fall back to the default.
  d.s->setArena(9);
  CHECK(d.s->arena() == 0);
  d.s->setMode(1);
  d.s->restart(d.now(), false);
  CHECK(d.s->matchConfig().arena == 0);
  CHECK(d.s->matchConfig().mode == 1);
}

TEST_CASE("session: replaceContent (hot reload) restarts with the new content",
          "[client][session]") {
  Driver d(SessionConfig{.seed = 6, .bots = 3});
  d.start();
  for (int i = 0; i < 200; ++i)
    d.frame(1);
  d.s->editContent().player.moveSpeed = Fixed::one();  // a live edit, dropped by the reload
  const sim::ContentDB content = twoArenas();
  d.s->replaceContent(content, d.now());
  d.rebase();
  CHECK(d.s->phase() == SessionPhase::Match);
  CHECK(d.s->curr().tick.value == 0);
  CHECK(d.s->seed() == 6);
  CHECK(d.s->contentIsDefault());  // the live copy is the new loaded content
  CHECK(d.s->contentHash() == sim::contentHash(content));
  CHECK(sim::contentHash(d.s->loadedContent()) == sim::contentHash(content));

  // A reload whose content lacks the selected arena falls back to index 0.
  d.s->setArena(1);
  d.s->replaceContent(sim::defaultContent(), d.now());
  d.rebase();
  CHECK(d.s->arena() == 0);
  CHECK(d.s->matchConfig().arena == 0);
}

TEST_CASE("session: a pause does not carry into the next match", "[client][session]") {
  Driver d(SessionConfig{.seed = 4, .bots = 1});
  d.start();
  d.frame(2);
  d.s->setPaused(true);
  d.s->requestSingleStep();  // pending, never consumed by a frame
  d.s->returnToLobby();      // Escape
  CHECK_FALSE(d.s->paused());
  d.push(key(Key::Enter, true));
  d.frame(1);
  REQUIRE(d.s->phase() == SessionPhase::Match);
  CHECK_FALSE(d.s->paused());
  d.frame(3);
  CHECK(d.s->curr().tick.value == 3);

  // Restart (the overlay's button) clears the pause and a pending step too.
  d.s->setPaused(true);
  d.s->requestSingleStep();
  d.s->restart(d.now(), false);
  d.rebase();
  CHECK_FALSE(d.s->paused());
  d.frame(2);
  CHECK(d.s->curr().tick.value == 2);
  d.s->setPaused(true);
  d.frame(1);
  CHECK(d.s->curr().tick.value == 2);  // no stale single step was left behind
}

TEST_CASE("session: returning to the lobby resets the match visuals", "[client][session]") {
  Driver d(SessionConfig{.seed = 4, .bots = 1});
  d.start();
  d.frame(1);
  presentation::PresentationState& ps = d.s->presentation();
  ps.shake = 0.5f;
  ps.effectCount = 3;
  ps.time = 12.0f;
  ps.players[0].bodyYaw = 1.0f;
  ps.players[0].flashRemaining = 0.2f;
  d.s->returnToLobby();
  CHECK(d.s->presentation().shake == 0.0f);
  CHECK(d.s->presentation().effectCount == 0);
  CHECK(d.s->presentation().time == 0.0f);
  CHECK(d.s->presentation().players[0].bodyYaw == 0.0f);
  CHECK(d.s->presentation().players[0].flashRemaining == 0.0f);
}

TEST_CASE("session: a button pressed during a pause does not fire on resume", "[client][session]") {
  Driver d(SessionConfig{.seed = 4, .bots = 1});
  d.start();
  d.frame(2);
  d.s->setPaused(true);
  d.push(key(Key::K, true));  // Dash goes down during the pause and stays held
  d.frame(1);
  d.frame(1);
  d.s->setPaused(false);
  d.frame(1);
  REQUIRE(d.s->ticksThisFrame() == 1);
  CHECK_FALSE(d.s->lastFrame().inputs[0].has(sim::Button::Dash));
  CHECK(d.s->curr().players.items[0].intent.pressed == 0);
  d.frame(3);
  CHECK_FALSE(d.s->lastFrame().inputs[0].has(sim::Button::Dash));
  // Released and pressed again: a real press.
  d.push(key(Key::K, false));
  d.frame(1);
  d.push(key(Key::K, true));
  d.frame(1);
  CHECK(d.s->lastFrame().inputs[0].has(sim::Button::Dash));
  CHECK(d.s->curr().players.items[0].intent.pressed == sim::buttonBit(sim::Button::Dash));

  // A single step while paused ignores the held key too, but takes a press
  // made in the stepped frame.
  d.s->setPaused(true);
  d.push(key(Key::K, false));
  d.push(key(Key::J, true));
  d.frame(1);
  d.s->requestSingleStep();
  d.frame(1);
  REQUIRE(d.s->ticksThisFrame() == 1);
  CHECK(d.s->lastFrame().inputs[0].buttons == 0);
  d.push(key(Key::K, true));
  d.s->requestSingleStep();
  d.frame(1);
  CHECK(d.s->lastFrame().inputs[0].buttons == sim::buttonBit(sim::Button::Dash));
}

TEST_CASE("session: a button held from the lobby does not fire at the start", "[client][session]") {
  Driver d(SessionConfig{.seed = 4, .bots = 1});
  d.push(key(Key::Space, true));  // Throw held while Enter starts the match
  d.push(key(Key::Enter, true));
  d.frame(1);
  REQUIRE(d.s->phase() == SessionPhase::Match);
  d.frame(1);
  CHECK(d.s->lastFrame().inputs[0].buttons == 0);
}

TEST_CASE("session: the press that takes over a dropped slot is not sampled", "[client][session]") {
  Driver d(SessionConfig{.seed = 5, .bots = 1, .bindKeyboardToSlot0 = false});
  d.push(padEvent(9, InputEventKind::PadAdded));
  d.push(padButton(9, PadButton::South, true));
  d.frame(1);
  d.push(padButton(9, PadButton::South, false));
  d.start();
  d.frame(1);
  d.push(padEvent(9, InputEventKind::PadRemoved));
  d.frame(1);
  REQUIRE(d.s->owner(PlayerId{0}) == SlotOwner::Dropped);

  // Dash takes the slot over: no dash in that tick, nor while it stays held.
  d.push(key(Key::K, true));
  d.frame(1);
  REQUIRE(d.s->owner(PlayerId{0}) == SlotOwner::Local);
  CHECK(d.s->lastFrame().inputs[0].buttons == 0);
  CHECK(d.s->lastFrame().inputs[0].subTick == 0);
  d.frame(2);
  CHECK(d.s->lastFrame().inputs[0].buttons == 0);
  d.push(key(Key::K, false));
  d.frame(1);
  // The next press is a normal one.
  d.push(key(Key::J, true));
  d.frame(1);
  CHECK(d.s->lastFrame().inputs[0].buttons == sim::buttonBit(sim::Button::Throw));
}

TEST_CASE("hostHandlesStart latches the lobby's start key instead of starting a match",
          "[client][session]") {
  Driver d(SessionConfig{.seed = 3, .bots = 1, .hostHandlesStart = true});
  d.frame(1);
  CHECK(!d.s->takeStartRequest());
  d.push(key(Key::Enter, true));
  d.frame(1);
  CHECK(d.s->phase() == SessionPhase::Lobby);  // no local match
  CHECK(d.s->takeStartRequest());
  CHECK(!d.s->takeStartRequest());  // cleared by the call
  // Turned off again, the start key starts a local match as in M2.
  d.s->setHostHandlesStart(false);
  d.push(key(Key::Enter, false));
  d.push(key(Key::Enter, true));
  d.frame(1);
  CHECK(d.s->phase() == SessionPhase::Match);
}

TEST_CASE("session: the cursor aims from the local monkey's position (v0.45)",
          "[client][session][mouse]") {
  Driver d(SessionConfig{.seed = 3, .bots = 1});
  constexpr float kW = 1280.0f;
  constexpr float kH = 720.0f;
  // A density of 2: the cursor's window coordinates are half the pixels.
  d.s->setAimViewport({.width = kW, .height = kH, .pixelDensity = 2.0f});
  d.start();
  d.frame(1);
  const sim::Player* p = d.s->curr().playerBySlot(PlayerId{0});
  REQUIRE(p != nullptr);
  const presentation::Vec2f at = presentation::toFloat(p->transform.pos);
  const presentation::Camera cam = presentation::makeArenaCamera(d.s->matchContent().arena, kW, kH);
  // Each cursor 3 units from the monkey along one axis; aim = that direction.
  const auto aimAt = [&](float dx, float dy) {
    const presentation::ScreenPoint px = presentation::worldToScreen(
        cam, presentation::simToWorld(presentation::Vec2f{at.x + dx, at.y + dy}));
    InputEvent e{};
    e.kind = InputEventKind::MouseMotion;
    e.x = px.x / 2.0f;
    e.y = px.y / 2.0f;
    d.push(e);
    d.frame(1);
    return static_cast<int>(d.s->lastFrame().inputs[0].aimDir);
  };
  CHECK(aimAt(3.0f, 0.0f) == 0);
  CHECK(aimAt(0.0f, 3.0f) == 64);
  CHECK(aimAt(-3.0f, 0.0f) == 128);
  CHECK(aimAt(0.0f, -3.0f) == 192);
  // A left click throws and a right click dashes, like J and K.
  InputEvent click{};
  click.kind = InputEventKind::MouseButtonDown;
  click.key = Key::MouseLeft;
  d.push(click);
  d.frame(1);
  CHECK(d.s->lastFrame().inputs[0].has(sim::Button::Throw));
  click.kind = InputEventKind::MouseButtonUp;
  d.push(click);
  click.kind = InputEventKind::MouseButtonDown;
  click.key = Key::MouseRight;
  d.push(click);
  d.frame(1);
  CHECK(d.s->lastFrame().inputs[0].has(sim::Button::Dash));
}

TEST_CASE("session: Mouse aim off aims by moving, on again waits for the mouse",
          "[client][session][mouse]") {
  Driver d(SessionConfig{.seed = 3, .bots = 1});
  constexpr float kW = 1280.0f;
  constexpr float kH = 720.0f;
  d.s->setAimViewport({.width = kW, .height = kH, .pixelDensity = 1.0f});
  d.start();
  d.frame(1);
  const sim::Player* p = d.s->curr().playerBySlot(PlayerId{0});
  REQUIRE(p != nullptr);
  const presentation::Vec2f at = presentation::toFloat(p->transform.pos);
  const presentation::Camera cam = presentation::makeArenaCamera(d.s->matchContent().arena, kW, kH);
  // The cursor 3 units east of the monkey: mouse aim points +x (0).
  const presentation::ScreenPoint px = presentation::worldToScreen(
      cam, presentation::simToWorld(presentation::Vec2f{at.x + 3.0f, at.y}));
  InputEvent motion{};
  motion.kind = InputEventKind::MouseMotion;
  motion.x = px.x;
  motion.y = px.y;
  d.push(motion);
  d.frame(1);
  REQUIRE(d.s->lastFrame().inputs[0].aimDir == 0);
  REQUIRE(d.s->sampler().usesMouseAim(PlayerId{0}));

  // Off (Settings > Mouse aim): at once, the move direction aims and the
  // "the mouse moved" latch is gone; clicks still throw.
  d.s->sampler().setMouseAimEnabled(false);
  CHECK_FALSE(d.s->sampler().usesMouseAim(PlayerId{0}));
  CHECK_FALSE(d.s->sampler().cursor().has_value());
  d.push(key(Key::W, true));
  d.push(motion);  // the mouse moving while off changes nothing
  d.frame(1);
  CHECK(d.s->lastFrame().inputs[0].aimDir == 64);
  InputEvent click{};
  click.kind = InputEventKind::MouseButtonDown;
  click.key = Key::MouseLeft;
  click.x = px.x;
  click.y = px.y;
  d.push(click);
  d.frame(1);
  CHECK(d.s->lastFrame().inputs[0].has(sim::Button::Throw));
  CHECK(d.s->lastFrame().inputs[0].aimDir == 64);

  // On again: like a fresh start, the keys aim until the mouse moves.
  d.s->sampler().setMouseAimEnabled(true);
  CHECK_FALSE(d.s->sampler().usesMouseAim(PlayerId{0}));
  d.push(key(Key::W, false));
  d.push(key(Key::S, true));
  d.frame(1);
  CHECK(d.s->lastFrame().inputs[0].aimDir == 192);
  d.push(motion);
  d.frame(1);
  // The cursor again: about east (the monkey has moved a little since), not
  // the move's south (192).
  const int back = d.s->lastFrame().inputs[0].aimDir;
  CHECK((back <= 8 || back >= 248));
}

TEST_CASE("the pause menu pauses a local match while open (v0.46)", "[client][session][menu]") {
  Driver d(SessionConfig{.seed = 4, .bots = 1});
  d.start();
  d.frame(2);
  REQUIRE(d.s->curr().tick.value == 2);
  client::PauseMenu menu;
  client::openPauseMenu(menu, *d.s);
  CHECK(menu.isOpen());
  CHECK_FALSE(menu.online());
  CHECK(d.s->paused());
  d.push(key(Key::K, true));  // Dash held while the menu is open (or pressed just before)
  d.frame(3);
  CHECK(d.s->curr().tick.value == 2);  // the match waits
  CHECK(d.s->ticksThisFrame() == 0);

  // Resume within a frame, before its advance(): the held Dash does not fire.
  client::closePauseMenu(menu, *d.s);
  CHECK_FALSE(menu.isOpen());
  CHECK_FALSE(d.s->paused());
  d.frame(1);
  REQUIRE(d.s->ticksThisFrame() == 1);
  CHECK(d.s->curr().tick.value == 3);
  CHECK_FALSE(d.s->lastFrame().inputs[0].has(sim::Button::Dash));
  d.push(key(Key::K, false));
  d.frame(1);
  d.push(key(Key::K, true));
  d.frame(1);
  CHECK(d.s->lastFrame().inputs[0].has(sim::Button::Dash));

  // A press that reaches the sampler in the frame the menu closes is dropped too.
  client::openPauseMenu(menu, *d.s);
  d.frame(1);
  d.push(key(Key::K, false));
  d.push(key(Key::J, true));
  client::closePauseMenu(menu, *d.s);
  d.frame(1);
  CHECK_FALSE(d.s->lastFrame().inputs[0].has(sim::Button::Throw));
}

TEST_CASE("session: presentation time stands still while paused", "[client][session]") {
  Driver d(SessionConfig{.seed = 4, .bots = 1});
  d.start();
  d.frame(1);
  CHECK(d.s->presentationSeconds(0.004) == 0.004);  // running: the frame's wall time
  d.s->setPaused(true);
  d.frame(1);
  REQUIRE(d.s->ticksThisFrame() == 0);
  CHECK(d.s->presentationSeconds(0.004) == 0.0);  // a still frame
  // A single step moves it by one tick's wall time.
  d.s->requestSingleStep();
  d.frame(1);
  REQUIRE(d.s->ticksThisFrame() == 1);
  CHECK(std::abs(d.s->presentationSeconds(0.004) - (1.0 / 60.0)) < 1e-6);
  d.frame(1);
  CHECK(d.s->presentationSeconds(0.004) == 0.0);
  d.s->setPaused(false);
  d.frame(1);
  CHECK(d.s->presentationSeconds(0.004) == 0.004);
}
