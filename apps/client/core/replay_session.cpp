#include "replay_session.hpp"

#include "kfm/core/bitstream.hpp"
#include "kfm/sim/step.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace kfm::client {

namespace {

[[nodiscard]] std::string hex32(uint32_t v) {
  std::array<char, 11> buf{};
  (void)std::snprintf(buf.data(), buf.size(), "0x%08x", v);
  return buf.data();
}

// The fewest bits a frame takes in a replay: no player present, so the
// tick and the present mask alone (visit(InputFrame)), measured from the
// codec itself.
[[nodiscard]] std::size_t minFrameBits() {
  static const std::size_t bits = [] {
    std::array<std::byte, 64> buf{};
    BitWriter w(buf);
    sim::writeReplayFrame(w, sim::InputFrame{});
    return w.bitsWritten();
  }();
  return bits;
}

}  // namespace

ReplaySession::ReplaySession() {
  clock_.setMaxTicksPerFrame(kReplayMaxTicksPerFrame);
  clock_.setTimeScale(speed_);
}

// --- loading ------------------------------------------------------------------

void ReplaySession::fail(ReplayError kind, std::string message) {
  loaded_ = false;
  errorKind_ = kind;
  error_ = std::move(message);
  header_ = {};
  frames_.clear();
  frames_.shrink_to_fit();
  keyframes_.clear();
  keyframes_.shrink_to_fit();
  lastSeekSteps_ = 0;
  matchConfig_ = {};
  prev_ = {};
  curr_ = {};
  lastFrame_ = {};
  tick_ = 0;
  still_ = true;
  frameEventCount_ = 0;
  ticksThisFrame_ = 0;
  presentation_ = presentation::PresentationState{};
}

bool ReplaySession::load(std::span<const std::byte> bytes, const sim::ContentDB& content,
                         uint64_t nowNs) {
  BitReader r(bytes);
  sim::ReplayHeader h{};
  if (!sim::readReplayHeader(r, h)) {
    fail(ReplayError::BadHeader,
         "not a replay this client can read (bad magic, version or header; it reads KFMR version " +
             std::to_string(sim::kReplayVersion) + ")");
    return false;
  }
  const uint32_t ours = sim::contentHash(content);
  if (h.contentHash != ours) {
    fail(ReplayError::ContentMismatch,
         "the replay was recorded with content " + hex32(h.contentHash) +
             " but this client has content " + hex32(ours) +
             ": open it in the client of the release that recorded it");
    return false;
  }
  if (h.arena >= content.arenaCount || h.mode >= content.modeCount ||
      h.loadout >= content.loadoutCount) {
    fail(ReplayError::BadMatchConfig, "the replay's arena " + std::to_string(h.arena) + ", mode " +
                                          std::to_string(h.mode) + " or loadout " +
                                          std::to_string(h.loadout) + " is not in the content");
    return false;
  }
  if (h.tickCount == 0) {
    fail(ReplayError::NoTicks, "the replay has no ticks");
    return false;
  }

  // A tickCount the data cannot hold is refused before anything is
  // allocated for it (a corrupt or hostile header must not reserve
  // gigabytes): what is reserved is at most bytes.size() / 6 frames.
  const std::size_t room = r.bitsRemaining() / minFrameBits();
  if (h.tickCount > room) {
    fail(ReplayError::BadFrames, "the replay is truncated: it claims " +
                                     std::to_string(h.tickCount) + " ticks but holds at most " +
                                     std::to_string(room));
    return false;
  }
  std::vector<sim::InputFrame> frames;
  frames.reserve(h.tickCount);
  const sim::WorldState start = sim::initialWorld(h.matchConfig());
  for (uint32_t i = 0; i < h.tickCount; ++i) {
    sim::InputFrame f{};
    if (!sim::readReplayFrame(r, f)) {
      fail(ReplayError::BadFrames, "the replay is truncated or malformed at tick " +
                                       std::to_string(i) + " of " + std::to_string(h.tickCount));
      return false;
    }
    if (f.tick.value != start.tick.value + i) {
      fail(ReplayError::BadFrames, "the replay's frame " + std::to_string(i) + " is for tick " +
                                       std::to_string(f.tick.value) + ", expected " +
                                       std::to_string(start.tick.value + i));
      return false;
    }
    frames.push_back(f);
  }

  loaded_ = true;
  errorKind_ = ReplayError::None;
  error_.clear();
  header_ = h;
  frames_ = std::move(frames);
  keyframes_.clear();
  keyframes_.resize((h.tickCount / kReplayKeyframeTicks) + 1);
  lastSeekSteps_ = 0;
  content_ = content;
  matchConfig_ = h.matchConfig();
  droppedEvents_ = 0;
  speed_ = kDefaultReplaySpeed;
  clock_.setTimeScale(speed_);
  clock_.setPaused(false);
  resetWorld();
  clock_.reset(nowNs);
  return true;
}

// --- per frame ----------------------------------------------------------------

void ReplaySession::advance(uint64_t nowNs) {
  frameEventCount_ = 0;
  ticksThisFrame_ = 0;
  if (!loaded_) return;
  const uint32_t ticks = clock_.advance(nowNs);
  for (uint32_t i = 0; i < ticks && !atEnd(); ++i) {
    prev_ = curr_;
    stepOnce(sink_);
    appendEvents(sink_);
    ++ticksThisFrame_;
    still_ = false;
  }
}

void ReplaySession::stepOnce(sim::EventSink& sink) {
  const sim::InputFrame& frame = frames_[tick_];
  sink.clear();
  sim::step(curr_, frame, matchContent(), sink);
  lastFrame_ = frame;
  ++tick_;
  if (tick_ % kReplayKeyframeTicks == 0) {
    std::unique_ptr<sim::WorldState>& slot = keyframes_[tick_ / kReplayKeyframeTicks];
    if (!slot) slot = std::make_unique<sim::WorldState>(curr_);
  }
}

void ReplaySession::appendEvents(const sim::EventSink& sink) {
  droppedEvents_ += sink.dropped;
  for (const sim::SimEvent& e : sink) {
    if (frameEventCount_ < frameEvents_.size()) {
      frameEvents_[frameEventCount_++] = e;
    } else {
      ++droppedEvents_;
    }
  }
}

// --- controls -----------------------------------------------------------------

void ReplaySession::setSpeed(double speed) {
  double best = kDefaultReplaySpeed;
  if (!std::isnan(speed)) {
    best = kReplaySpeeds[0];
    for (const double s : kReplaySpeeds) {
      if (std::abs(s - speed) < std::abs(best - speed)) best = s;
    }
  }
  speed_ = best;
  clock_.setTimeScale(speed_);
}

std::size_t ReplaySession::speedIndex() const {
  for (std::size_t i = 0; i < kReplaySpeeds.size(); ++i) {
    if (kReplaySpeeds[i] == speed_) return i;
  }
  return 0;
}

void ReplaySession::restart(uint64_t nowNs) {
  clock_.setPaused(false);
  seek(0, nowNs);
}

void ReplaySession::seek(uint32_t tick, uint64_t nowNs) {
  frameEventCount_ = 0;
  ticksThisFrame_ = 0;
  lastSeekSteps_ = 0;
  if (!loaded_) return;
  const uint32_t target = std::min(tick, tickCount());
  // Backward, or forward past a keyframe: start from the newest keyframe at
  // or before the target (the initial world when there is none).
  const uint32_t from = keyframeAtOrBefore(target);
  if (target < tick_ || from > tick_) restoreKeyframe(from);
  // Each step's events are dropped with the scratch sink: a jump is silent.
  while (tick_ < target) {
    stepOnce(sink_);
    ++lastSeekSteps_;
  }
  sink_.clear();
  prev_ = curr_;
  still_ = true;
  presentation_ = presentation::PresentationState{};
  // reset() keeps the pause and the time scale and drops a pending single step.
  clock_.reset(nowNs);
}

void ReplaySession::resetWorld() {
  curr_ = sim::initialWorld(matchConfig_);
  prev_ = curr_;
  sink_.clear();
  lastFrame_ = {};
  tick_ = 0;
  still_ = true;
  frameEventCount_ = 0;
  ticksThisFrame_ = 0;
  presentation_ = presentation::PresentationState{};
}

uint32_t ReplaySession::keyframeAtOrBefore(uint32_t tick) const {
  for (std::size_t k = std::min<std::size_t>(tick / kReplayKeyframeTicks, keyframes_.size() - 1);
       k > 0; --k) {
    if (keyframes_[k]) return static_cast<uint32_t>(k) * kReplayKeyframeTicks;
  }
  return 0;
}

void ReplaySession::restoreKeyframe(uint32_t tick) {
  if (tick == 0) {
    resetWorld();
    return;
  }
  // A keyframe is the state straight play reaches at its tick (a copy of a
  // trivially copyable WorldState), so stepping on from it is exact.
  curr_ = *keyframes_[tick / kReplayKeyframeTicks];
  prev_ = curr_;
  sink_.clear();
  lastFrame_ = frames_[tick - 1];
  tick_ = tick;
}

std::size_t ReplaySession::keyframeCount() const {
  return static_cast<std::size_t>(
      std::ranges::count_if(keyframes_, [](const auto& k) { return k != nullptr; }));
}

// --- state --------------------------------------------------------------------

uint32_t ReplaySession::checksum() const {
  return sim::checksum(curr_);
}

bool ReplaySession::rendersNewest() const {
  // Still, at the end or paused: the tick the session is at (a single step
  // shows the tick it reached, never a blend with the one before).
  if (still_ || atEnd() || clock_.paused()) return true;
  switch (policy_) {
    case InterpolationPolicy::Interpolate:
      return false;
    case InterpolationPolicy::Newest:
      return true;
    case InterpolationPolicy::Auto:
      break;
  }
  // Below 1x a tick lasts longer than a 60 Hz tick on any display: holding
  // it would show the replay at 60 x speed frames a second.
  if (speed_ < 1.0) return false;
  const double tickHz = static_cast<double>(sim::kTickHz) * clock_.timeScale();
  return clock_.displayHz() >= tickHz * kNewestTickRateTolerance;
}

const sim::WorldState& ReplaySession::renderPrev() const {
  return rendersNewest() ? curr_ : prev_;
}

float ReplaySession::renderAlpha() const {
  return rendersNewest() ? 1.0f : clock_.alpha();
}

double ReplaySession::presentationSeconds(double wallSeconds) const {
  if (!clock_.paused()) return wallSeconds;
  return static_cast<double>(ticksThisFrame_) * static_cast<double>(clock_.tickWallNs()) * 1e-9;
}

// --- files --------------------------------------------------------------------

bool readFileBytes(const std::string& path, std::vector<std::byte>& out, std::string& error,
                   std::size_t maxBytes) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  // The size first: a file over the cap is refused before anything is read.
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    error = "cannot read " + path + " (" + ec.message() + ")";
    return false;
  }
  if (size > maxBytes) {
    error = path + " is larger than " +
            (maxBytes % (std::size_t{1} << 20) == 0 ? std::to_string(maxBytes >> 20) + " MiB"
                                                    : std::to_string(maxBytes) + " bytes");
    return false;
  }
  out.resize(static_cast<std::size_t>(size));
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes as chars for read()
  f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
  if (f.gcount() != static_cast<std::streamsize>(out.size())) {
    out.clear();
    error = "cannot read " + path;
    return false;
  }
  error.clear();
  return true;
}

}  // namespace kfm::client
