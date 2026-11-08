// The replay viewer's ReplaySession (M7 step 6): the corpus plays back to
// its recorded per-tick checksums through advance(), seeks land on the
// recorded state, pause / single step / speed follow the clock, and broken
// or foreign replays are refused.
#include "core/replay_session.hpp"

#include "kfm/content/loader.hpp"
#include "kfm/core/bitstream.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/replay.hpp"
#include "kfm/sim/step.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace kfm;
using client::ReplayError;
using client::ReplaySession;

namespace {

constexpr uint64_t kT0 = 1'000'000'000ULL;

// Wall time at which exactly k ticks are due since `base` at 1x (ceil keeps
// the accumulator from falling short by rounding).
[[nodiscard]] constexpr uint64_t timeAtTick(uint64_t base, uint64_t k) {
  return base + ((k * 1'000'000'000ULL) + 59) / 60;
}

// The corpus is recorded with the content compiled into every binary.
const sim::ContentDB& shipped() {
  static const sim::ContentDB db = [] {
    sim::ContentDB d{};
    content::Diagnostics diag;
    if (!content::loadEmbeddedContent(d, diag)) d = sim::defaultContent();
    return d;
  }();
  return db;
}

[[nodiscard]] std::filesystem::path corpus() {
  return std::filesystem::path{KFM_REPLAY_CORPUS_DIR};
}

[[nodiscard]] std::vector<std::filesystem::path> corpusReplays() {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(corpus(), ec)) {
    if (entry.path().extension() == ".kfmr") out.push_back(entry.path());
  }
  std::ranges::sort(out);
  return out;
}

[[nodiscard]] std::vector<std::byte> readBytes(const std::filesystem::path& path) {
  std::vector<std::byte> bytes;
  std::string error;
  if (!client::readFileBytes(path.string(), bytes, error)) FAIL(error);
  return bytes;
}

// The .checksums beside a replay: line t is "t hex", the state at tick t
// (0 = the initial world, the last line the final state).
[[nodiscard]] std::vector<uint32_t> expectedChecksums(const std::filesystem::path& replay) {
  std::filesystem::path p = replay;
  p.replace_extension(".checksums");
  std::ifstream in(p);
  std::vector<uint32_t> out;
  unsigned long tick = 0;
  unsigned long sum = 0;
  while (in >> tick >> std::hex >> sum >> std::dec) {
    if (tick != out.size()) FAIL("checksum lines out of order in " + p.string());
    out.push_back(static_cast<uint32_t>(sum));
  }
  return out;
}

[[nodiscard]] std::filesystem::path shortReplay() {
  return corpus() / "scripted_4p_1200t.kfmr";
}

// Plays a session at 1x in frames of `ticksPerFrame` ticks.
struct Driver {
  std::unique_ptr<ReplaySession> s{std::make_unique<ReplaySession>()};
  uint64_t base{kT0};
  uint64_t due{0};

  [[nodiscard]] uint64_t now() const { return timeAtTick(base, due); }
  void load(std::span<const std::byte> bytes) {
    REQUIRE(s->load(bytes, shipped(), now()));
    rebase();
  }
  void rebase() {
    base = now();
    due = 0;
  }
  void frame(uint64_t ticks) {
    due += ticks;
    s->advance(now());
  }
};

[[nodiscard]] bool sameEvent(const sim::SimEvent& x, const sim::SimEvent& y) {
  return x.kind == y.kind && x.tick.value == y.tick.value && x.player.value == y.player.value &&
         x.a == y.a && x.b == y.b;
}

// A replay of `ticks` idle frames with this header's fields (contentHash
// defaults to the shipped content's).
[[nodiscard]] std::vector<std::byte> syntheticReplay(sim::ReplayHeader h, bool sameTickTwice) {
  std::vector<std::byte> buf(sim::replayMaxBytes(h));
  BitWriter w(buf);
  sim::writeReplayHeader(w, h);
  for (uint32_t i = 0; i < h.tickCount; ++i) {
    sim::InputFrame f{};
    f.tick = Tick{sameTickTwice ? 0u : i};
    f.present = h.players;
    sim::writeReplayFrame(w, f);
  }
  REQUIRE(w.ok());
  buf.resize(w.bytesWritten());
  return buf;
}

[[nodiscard]] sim::ReplayHeader idleHeader(uint32_t ticks) {
  sim::ReplayHeader h{};
  h.seed = 7;
  h.contentHash = sim::contentHash(shipped());
  h.players = 0x3;
  h.teams = sim::kFreeForAllTeams;
  h.tickCount = ticks;
  return h;
}

[[nodiscard]] std::string hex32(uint32_t v) {
  std::array<char, 11> buf{};
  (void)std::snprintf(buf.data(), buf.size(), "0x%08x", v);
  return buf.data();
}

}  // namespace

TEST_CASE("replay: every corpus replay plays back to its recorded checksums", "[client][replay]") {
  const auto replays = corpusReplays();
  REQUIRE(!replays.empty());
  for (const auto& path : replays) {
    INFO(path.filename().string());
    const std::vector<uint32_t> expected = expectedChecksums(path);
    const std::vector<std::byte> bytes = readBytes(path);
    Driver d;
    d.load(bytes);
    const ReplaySession& s = *d.s;
    REQUIRE(s.tickCount() + 1 == expected.size());
    CHECK(s.header().contentHash == sim::contentHash(shipped()));
    CHECK(s.tick() == 0);
    CHECK(s.checksum() == expected[0]);
    // One tick per frame: every tick's state is compared.
    uint32_t mismatches = 0;
    uint32_t firstMismatch = 0;
    while (!s.atEnd()) {
      d.frame(1);
      if (s.ticksThisFrame() != 1) FAIL("a 1-tick frame stepped " << s.ticksThisFrame());
      if (s.checksum() == expected[s.tick()]) continue;
      firstMismatch = mismatches == 0 ? s.tick() : firstMismatch;
      ++mismatches;
    }
    INFO("first mismatch at tick " << firstMismatch);
    CHECK(mismatches == 0);
    CHECK(s.tick() == s.tickCount());
    CHECK(s.checksum() == expected.back());
    // At the end nothing more is stepped and the final state is drawn still.
    d.frame(4);
    CHECK(s.ticksThisFrame() == 0);
    CHECK(s.tick() == s.tickCount());
    CHECK(&s.renderPrev() == &s.curr());
    CHECK(s.renderAlpha() == 1.0f);
  }
}

TEST_CASE("replay: frameEvents() are the events of the ticks stepped", "[client][replay]") {
  const std::vector<std::byte> bytes = readBytes(shortReplay());
  // The reference: the replay stepped directly, every tick's events kept.
  BitReader r(bytes);
  sim::ReplayHeader h{};
  REQUIRE(sim::readReplayHeader(r, h));
  const sim::MatchContent mc = sim::matchContent(shipped(), h.matchConfig());
  auto world = std::make_unique<sim::WorldState>(sim::initialWorld(h.matchConfig()));
  auto sink = std::make_unique<sim::EventSink>();
  std::vector<std::vector<sim::SimEvent>> perTick;
  for (uint32_t i = 0; i < h.tickCount; ++i) {
    sim::InputFrame f{};
    REQUIRE(sim::readReplayFrame(r, f));
    sink->clear();
    sim::step(*world, f, mc, *sink);
    perTick.emplace_back(sink->begin(), sink->end());
  }

  Driver d;
  d.load(bytes);
  const ReplaySession& s = *d.s;
  uint64_t total = 0;
  uint32_t mismatches = 0;
  while (!s.atEnd()) {
    const uint32_t from = s.tick();
    d.frame(3);
    std::vector<sim::SimEvent> want;
    for (uint32_t t = from; t < s.tick(); ++t)
      want.insert(want.end(), perTick[t].begin(), perTick[t].end());
    const auto got = s.frameEvents();
    if (got.size() != want.size() || !std::equal(got.begin(), got.end(), want.begin(), sameEvent)) {
      ++mismatches;
    }
    total += got.size();
  }
  CHECK(mismatches == 0);
  CHECK(total > 0);  // a scripted match throws, hits and eliminates
  CHECK(s.droppedEvents() == 0);
}

TEST_CASE("replay: seeks land on the recorded state, silently", "[client][replay]") {
  const std::vector<std::byte> bytes = readBytes(shortReplay());
  const std::vector<uint32_t> expected = expectedChecksums(shortReplay());
  Driver d;
  d.load(bytes);
  ReplaySession& s = *d.s;
  REQUIRE(s.tickCount() == 1200);

  // Play a little, so there are events and a moving frame.
  for (int i = 0; i < 100; ++i)
    d.frame(2);
  REQUIRE(s.tick() == 200);

  SECTION("forward") {
    s.seek(700, d.now());
    d.rebase();
    CHECK(s.tick() == 700);
    CHECK(s.checksum() == expected[700]);
    CHECK(s.frameEvents().empty());
    CHECK(s.ticksThisFrame() == 0);
    CHECK(&s.renderPrev() == &s.curr());
    CHECK(s.renderAlpha() == 1.0f);
  }
  SECTION("backward, then play to the end") {
    s.seek(700, d.now());
    s.seek(300, d.now());
    d.rebase();
    CHECK(s.tick() == 300);
    CHECK(s.checksum() == expected[300]);
    CHECK(s.frameEvents().empty());
    while (!s.atEnd())
      d.frame(4);
    CHECK(s.checksum() == expected.back());
  }
  SECTION("past the end clamps to the end; restart plays from tick 0") {
    s.setPaused(true);
    s.seek(5000, d.now());
    d.rebase();
    CHECK(s.atEnd());
    CHECK(s.tick() == 1200);
    CHECK(s.checksum() == expected.back());
    CHECK(s.paused());  // a seek keeps the pause
    s.restart(d.now());
    d.rebase();
    CHECK_FALSE(s.paused());
    CHECK(s.tick() == 0);
    CHECK(s.checksum() == expected[0]);
    d.frame(2);
    CHECK(s.tick() == 2);
    CHECK(s.checksum() == expected[2]);
  }
  SECTION("the clock restarts at the seek: no backlog is owed") {
    // A long stall before the seek is not stepped after it.
    d.due += 600;
    s.seek(400, d.now());
    d.rebase();
    d.frame(1);
    CHECK(s.ticksThisFrame() == 1);
    CHECK(s.tick() == 401);
    CHECK(s.checksum() == expected[401]);
  }
}

TEST_CASE("replay: pause, single step and speed", "[client][replay]") {
  const std::vector<std::byte> bytes = readBytes(shortReplay());
  const std::vector<uint32_t> expected = expectedChecksums(shortReplay());

  SECTION("pause stops stepping; a single step steps one tick") {
    Driver d;
    d.load(bytes);
    ReplaySession& s = *d.s;
    d.frame(5);
    REQUIRE(s.tick() == 5);
    s.setPaused(true);
    d.frame(5);
    CHECK(s.ticksThisFrame() == 0);
    CHECK(s.tick() == 5);
    CHECK(s.presentationSeconds(0.1) == 0.0);  // a paused replay draws a still frame
    s.requestSingleStep();
    d.frame(5);
    CHECK(s.ticksThisFrame() == 1);
    CHECK(s.tick() == 6);
    CHECK(s.checksum() == expected[6]);
    CHECK(s.presentationSeconds(0.1) > 0.0);
    d.frame(5);
    CHECK(s.ticksThisFrame() == 0);
    s.setPaused(false);
    d.frame(3);
    CHECK(s.tick() == 9);  // time spent paused is not owed
    CHECK(s.presentationSeconds(0.1) == 0.1);
  }
  SECTION("a single step at the end steps nothing") {
    Driver d;
    d.load(bytes);
    ReplaySession& s = *d.s;
    s.seek(s.tickCount(), d.now());
    d.rebase();
    s.setPaused(true);
    s.requestSingleStep();
    d.frame(1);
    CHECK(s.ticksThisFrame() == 0);
    CHECK(s.atEnd());
  }
  SECTION("speeds snap to the fixed set") {
    ReplaySession s;
    CHECK(s.speed() == 1.0);
    s.setSpeed(2.0);
    CHECK(s.speed() == 2.0);
    CHECK(s.clock().timeScale() == 2.0);
    s.setSpeed(3.1);
    CHECK(s.speed() == 4.0);
    CHECK(s.clock().timeScale() == 4.0);
    CHECK(s.speedIndex() == 4);
    s.setSpeed(0.3);
    CHECK(s.speed() == 0.25);
    CHECK(s.speedIndex() == 0);
    s.setSpeed(100.0);
    CHECK(s.speed() == 4.0);
  }
  SECTION("speed scales the ticks stepped for the same wall time") {
    for (const double speed : client::kReplaySpeeds) {
      INFO("speed " << speed);
      Driver d;
      d.load(bytes);
      d.s->setSpeed(speed);
      // 1x would step 120 ticks over 60 frames of 2 ticks each.
      for (int i = 0; i < 60; ++i)
        d.frame(2);
      const double want = 120.0 * speed;
      CHECK(static_cast<double>(d.s->tick()) >= want - 1.0);
      CHECK(static_cast<double>(d.s->tick()) <= want + 1.0);
      CHECK(d.s->checksum() == expected[d.s->tick()]);
    }
  }
}

TEST_CASE("replay: broken and foreign replays are refused", "[client][replay]") {
  const std::vector<std::byte> bytes = readBytes(shortReplay());
  ReplaySession s;

  SECTION("bad magic") {
    std::vector<std::byte> b = bytes;
    b[0] = std::byte{'X'};
    CHECK_FALSE(s.load(b, shipped(), kT0));
    CHECK(s.errorKind() == ReplayError::BadHeader);
    CHECK_FALSE(s.error().empty());
    CHECK_FALSE(s.loaded());
  }
  SECTION("another version") {
    std::vector<std::byte> b = bytes;
    b[4] = std::byte{0x7F};  // the low byte of the version
    CHECK_FALSE(s.load(b, shipped(), kT0));
    CHECK(s.errorKind() == ReplayError::BadHeader);
  }
  SECTION("a short header") {
    CHECK_FALSE(s.load(std::span(bytes).first(10), shipped(), kT0));
    CHECK(s.errorKind() == ReplayError::BadHeader);
    CHECK_FALSE(s.load({}, shipped(), kT0));
    CHECK(s.errorKind() == ReplayError::BadHeader);
  }
  SECTION("truncated frames") {
    CHECK_FALSE(s.load(std::span(bytes).first(bytes.size() / 2), shipped(), kT0));
    CHECK(s.errorKind() == ReplayError::BadFrames);
    CHECK(s.error().find("truncated") != std::string::npos);
  }
  SECTION("frames out of tick order") {
    CHECK_FALSE(s.load(syntheticReplay(idleHeader(3), true), shipped(), kT0));
    CHECK(s.errorKind() == ReplayError::BadFrames);
  }
  SECTION("other content: both hashes are named") {
    sim::ContentDB other = shipped();
    other.arenaCount = static_cast<uint8_t>(other.arenaCount - 1);  // any change moves the hash
    const uint32_t ours = sim::contentHash(other);
    REQUIRE(ours != sim::contentHash(shipped()));
    CHECK_FALSE(s.load(bytes, other, kT0));
    CHECK(s.errorKind() == ReplayError::ContentMismatch);
    CHECK(s.error().find(hex32(sim::contentHash(shipped()))) != std::string::npos);
    CHECK(s.error().find(hex32(ours)) != std::string::npos);
  }
  SECTION("an arena, mode or loadout the content does not have") {
    sim::ReplayHeader h = idleHeader(3);
    h.arena = shipped().arenaCount;
    CHECK_FALSE(s.load(syntheticReplay(h, false), shipped(), kT0));
    CHECK(s.errorKind() == ReplayError::BadMatchConfig);
    h = idleHeader(3);
    h.mode = shipped().modeCount;
    CHECK_FALSE(s.load(syntheticReplay(h, false), shipped(), kT0));
    CHECK(s.errorKind() == ReplayError::BadMatchConfig);
    h = idleHeader(3);
    h.loadout = shipped().loadoutCount;
    CHECK_FALSE(s.load(syntheticReplay(h, false), shipped(), kT0));
    CHECK(s.errorKind() == ReplayError::BadMatchConfig);
  }
  SECTION("no ticks") {
    CHECK_FALSE(s.load(syntheticReplay(idleHeader(0), false), shipped(), kT0));
    CHECK(s.errorKind() == ReplayError::NoTicks);
  }
  SECTION("a refusal drops the replay that was loaded; a good load clears the error") {
    REQUIRE(s.load(bytes, shipped(), kT0));
    CHECK(s.loaded());
    CHECK_FALSE(s.load(syntheticReplay(idleHeader(0), false), shipped(), kT0));
    CHECK_FALSE(s.loaded());
    CHECK(s.tickCount() == 0);
    s.advance(timeAtTick(kT0, 10));
    CHECK(s.ticksThisFrame() == 0);
    REQUIRE(s.load(syntheticReplay(idleHeader(3), false), shipped(), kT0));
    CHECK(s.errorKind() == ReplayError::None);
    CHECK(s.error().empty());
    CHECK(s.tickCount() == 3);
    CHECK(s.header().buildHash == 0);  // not checked: every build writes 0 today
  }
}

TEST_CASE("replay: readFileBytes", "[client][replay]") {
  std::vector<std::byte> out;
  std::string error;
  CHECK_FALSE(client::readFileBytes((corpus() / "no-such-replay.kfmr").string(), out, error));
  CHECK(error.find("no-such-replay.kfmr") != std::string::npos);
  REQUIRE(client::readFileBytes(shortReplay().string(), out, error));
  CHECK(error.empty());
  CHECK(out.size() > sim::kReplayHeaderBytes);
}
