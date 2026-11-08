#include "replay_session.hpp"

#include "kfm/core/bitstream.hpp"
#include "kfm/sim/step.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <utility>

namespace kfm::client {

namespace {

[[nodiscard]] std::string hex32(uint32_t v) {
  std::array<char, 11> buf{};
  (void)std::snprintf(buf.data(), buf.size(), "0x%08x", v);
  return buf.data();
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

  // Every frame is at least a few bytes: never reserve more frames than the
  // data could hold (a corrupt tickCount must not allocate gigabytes).
  std::vector<sim::InputFrame> frames;
  frames.reserve(std::min<std::size_t>(h.tickCount, bytes.size()));
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
  if (!loaded_) return;
  const uint32_t target = std::min(tick, tickCount());
  if (target < tick_) resetWorld();
  // Each step's events are dropped with the scratch sink: a jump is silent.
  while (tick_ < target)
    stepOnce(sink_);
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

// --- state --------------------------------------------------------------------

uint32_t ReplaySession::checksum() const {
  return sim::checksum(curr_);
}

bool ReplaySession::rendersNewest() const {
  if (still_ || atEnd()) return true;
  switch (policy_) {
    case InterpolationPolicy::Interpolate:
      return false;
    case InterpolationPolicy::Newest:
      return true;
    case InterpolationPolicy::Auto:
      break;
  }
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

bool readFileBytes(const std::string& path, std::vector<std::byte>& out, std::string& error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
  if (f.bad()) {
    error = "cannot read " + path;
    return false;
  }
  out.resize(raw.size());
  std::ranges::transform(raw, out.begin(), [](char c) { return static_cast<std::byte>(c); });
  error.clear();
  return true;
}

}  // namespace kfm::client
