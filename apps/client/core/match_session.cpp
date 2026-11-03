#include "match_session.hpp"

#include "kfm/sim/scripted_inputs.hpp"
#include "kfm/sim/step.hpp"

#include <algorithm>
#include <bit>
#include <optional>

namespace kfm::client {

namespace {

[[nodiscard]] constexpr sim::PlayerMask bit(uint8_t slot) {
  return static_cast<sim::PlayerMask>(1u << slot);
}

[[nodiscard]] constexpr bool has(sim::PlayerMask m, uint8_t slot) {
  return (m & bit(slot)) != 0;
}

// Lowest slot of `m`, if any.
[[nodiscard]] std::optional<uint8_t> lowest(sim::PlayerMask m) {
  if (m == 0) return std::nullopt;
  return static_cast<uint8_t>(std::countr_zero(static_cast<uint32_t>(m)));
}

constexpr sim::PlayerMask kAllSlots = static_cast<sim::PlayerMask>((1u << sim::kMaxPlayers) - 1u);

[[nodiscard]] bool isStartEvent(const platform::InputEvent& e) {
  using platform::InputEventKind;
  using platform::Key;
  if (e.kind == InputEventKind::KeyDown) return e.key == Key::Enter || e.key == Key::KpEnter;
  return e.kind == InputEventKind::PadButtonDown && e.button == platform::PadButton::Start;
}

}  // namespace

MatchSession::MatchSession(const SessionConfig& config)
    : seed_(config.seed), botSeed_(config.botSeed), content_(sim::defaultContent()) {
  clock_.setMaxTicksPerFrame(std::min(config.maxTicksPerFrame, kDefaultMaxTicksPerFrame));
  if (config.bindKeyboardToSlot0) {
    (void)sampler_.bind(PlayerId{0}, platform::DeviceBinding::keyboard(0));
  }
  setBotCount(config.bots);
  createWorld(lobbyPresentMask());
}

// --- per frame ----------------------------------------------------------------

void MatchSession::push(const platform::InputEvent& event) {
  if (isStartEvent(event)) startRequested_ = true;
  sampler_.push(event);
}

void MatchSession::advance(uint64_t nowNs) {
  frameEventCount_ = 0;
  ticksThisFrame_ = 0;

  if (phase_ == SessionPhase::Lobby) {
    handleLobbyJoins();
    const bool start = startRequested_;
    startRequested_ = false;
    sampler_.endTick();  // lobby presses never reach a match
    if (start) {
      (void)startMatch(nowNs);
    } else {
      createWorld(lobbyPresentMask());  // keep the drawn lobby arena in sync
    }
    return;
  }

  handleRebinds();
  if (startRequested_ && curr_.rules.phase == sim::MatchPhase::MatchEnded) {
    startRequested_ = false;
    sampler_.endTick();
    returnToLobby();
    return;
  }
  startRequested_ = false;

  // Tick i of the batch takes the presses stamped inside its wall-clock
  // window; the last one ends at nowNs and takes everything still pending.
  const uint32_t ticks = clock_.advance(nowNs);
  const uint64_t wall = clock_.tickWallNs();
  for (uint32_t i = 0; i < ticks; ++i) {
    const uint64_t start = clock_.tickStartNs(i);
    stepOnce(start, wall, i + 1 < ticks ? start + wall : platform::kNoCutoff);
  }
  // While paused, presses are discarded rather than piling up for the next tick.
  if (ticks == 0 && clock_.paused()) sampler_.endTick();
}

void MatchSession::stepOnce(uint64_t tickStartNs, uint64_t tickWallNs, uint64_t tickEndNs) {
  sim::InputFrame frame{};
  const sim::PlayerMask bound = sampler_.sampleFrame(frame, tickStartNs, tickWallNs, tickEndNs);
  frame.tick = curr_.tick;
  frame.present = curr_.rules.present;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if (!has(frame.present, i)) {
      frame.inputs[i] = sim::PlayerInput{};
    } else if (has(botMask_, i)) {
      frame.inputs[i] = sim::scriptedInput(botRng_, curr_, content_, PlayerId{i});
    } else if (has(localMask_, i) && !has(bound, i)) {
      frame.inputs[i] = lastInputs_[i];  // dropped pad: repeat last input
    }
  }
  lastInputs_ = frame.inputs;
  lastFrame_ = frame;

  prev_ = curr_;
  sink_.clear();
  sim::step(curr_, frame, content_, sink_);
  appendEvents(sink_);
  ++ticksThisFrame_;
}

void MatchSession::appendEvents(const sim::EventSink& sink) {
  droppedEvents_ += sink.dropped;
  for (const sim::SimEvent& e : sink) {
    if (frameEventCount_ < frameEvents_.size()) {
      frameEvents_[frameEventCount_++] = e;
    } else {
      ++droppedEvents_;
    }
  }
}

// --- lobby --------------------------------------------------------------------

void MatchSession::setBotCount(uint8_t bots) {
  const auto bound =
      static_cast<uint32_t>(std::popcount(static_cast<uint32_t>(sampler_.boundSlots())));
  const uint32_t free = sim::kMaxPlayers - bound;
  bots_ = static_cast<uint8_t>(std::min<uint32_t>(bots, free));
}

sim::PlayerMask MatchSession::lobbyPresentMask() const {
  sim::PlayerMask present = sampler_.boundSlots();
  uint8_t remaining = bots_;
  for (uint8_t i = 0; i < sim::kMaxPlayers && remaining > 0; ++i) {
    if (has(present, i)) continue;
    present = static_cast<sim::PlayerMask>(present | bit(i));
    --remaining;
  }
  return present;
}

void MatchSession::handleLobbyJoins() {
  for (uint32_t guard = 0; guard < sim::kMaxPlayers; ++guard) {
    const std::optional<platform::DeviceBinding> device = sampler_.pollJoin();
    if (!device) break;
    const auto free = lowest(static_cast<sim::PlayerMask>(~sampler_.boundSlots() & kAllSlots));
    if (!free) break;
    (void)sampler_.bind(PlayerId{*free}, *device);
  }
  setBotCount(bots_);  // joins may have taken bot slots
}

void MatchSession::handleRebinds() {
  for (uint32_t guard = 0; guard < sim::kMaxPlayers; ++guard) {
    const sim::PlayerMask dropped =
        static_cast<sim::PlayerMask>(localMask_ & ~sampler_.boundSlots() & kAllSlots);
    const auto slot = lowest(dropped);
    if (!slot) break;
    const std::optional<platform::DeviceBinding> device = sampler_.pollJoin();
    if (!device) break;
    (void)sampler_.bind(PlayerId{*slot}, *device, lastInputs_[*slot].aimDir);
  }
}

bool MatchSession::startMatch(uint64_t nowNs) {
  const sim::PlayerMask present = lobbyPresentMask();
  if (present == 0) return false;
  beginMatch(present, static_cast<sim::PlayerMask>(present & sampler_.boundSlots()), nowNs);
  return true;
}

void MatchSession::returnToLobby() {
  phase_ = SessionPhase::Lobby;
  localMask_ = 0;
  botMask_ = 0;
  frameEventCount_ = 0;
  setBotCount(bots_);
  createWorld(lobbyPresentMask());
}

void MatchSession::restart(uint64_t nowNs, bool newSeed) {
  if (newSeed) {
    Rng r = Rng::seeded(seed_);
    seed_ = (static_cast<uint64_t>(r.next()) << 32) | r.next();
  }
  if (phase_ == SessionPhase::Lobby) {
    createWorld(lobbyPresentMask());
    return;
  }
  const sim::PlayerMask present = curr_.rules.present;
  beginMatch(present, static_cast<sim::PlayerMask>(localMask_ & sampler_.boundSlots()), nowNs);
}

void MatchSession::setSeed(uint64_t seed) {
  seed_ = seed;
  if (phase_ == SessionPhase::Lobby) createWorld(lobbyPresentMask());
}

void MatchSession::createWorld(sim::PlayerMask present) {
  curr_ = sim::initialWorld(sim::MatchConfig{.seed = seed_, .players = present});
  prev_ = curr_;
}

void MatchSession::beginMatch(sim::PlayerMask present, sim::PlayerMask local, uint64_t nowNs) {
  createWorld(present);
  phase_ = SessionPhase::Match;
  localMask_ = local;
  botMask_ = static_cast<sim::PlayerMask>(present & ~local);
  botRng_ = Rng::seeded(botSeed_);
  lastInputs_ = {};
  lastFrame_ = {};
  frameEventCount_ = 0;
  presentation_ = presentation::PresentationState{};
  sampler_.clearDropped();
  sampler_.endTick();
  startRequested_ = false;
  clock_.reset(nowNs);
}

// --- state --------------------------------------------------------------------

uint32_t MatchSession::checksum() const {
  return sim::checksum(curr_);
}

bool MatchSession::rendersNewest() const {
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

const sim::WorldState& MatchSession::renderPrev() const {
  return rendersNewest() ? curr_ : prev_;
}

float MatchSession::renderAlpha() const {
  return rendersNewest() ? 1.0f : clock_.alpha();
}

void MatchSession::resetContent() {
  content_ = sim::defaultContent();
}

uint32_t MatchSession::contentHash() const {
  return sim::contentHash(content_);
}

bool MatchSession::contentIsDefault() const {
  return contentHash() == sim::contentHash(sim::defaultContent());
}

SlotOwner MatchSession::owner(PlayerId slot) const {
  if (!slot.isValid() || slot.value >= sim::kMaxPlayers) return SlotOwner::None;
  const uint8_t i = slot.value;
  if (phase_ == SessionPhase::Lobby) {
    if (sampler_.isBound(slot)) return SlotOwner::Local;
    return has(lobbyPresentMask(), i) ? SlotOwner::Bot : SlotOwner::None;
  }
  if (!has(curr_.rules.present, i)) return SlotOwner::None;
  if (has(botMask_, i)) return SlotOwner::Bot;
  return sampler_.isBound(slot) ? SlotOwner::Local : SlotOwner::Dropped;
}

}  // namespace kfm::client
