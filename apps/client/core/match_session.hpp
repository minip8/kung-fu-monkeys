// MatchSession: the playable local client's match loop (architecture §6.8,
// §7 local multiplayer, §11 M2). SDL-free and unit-tested headlessly.
//
// It owns the two newest WorldStates (prev, curr), the loaded ContentDB and
// a mutable copy of it (the debug UI edits the copy live for feel tuning;
// it is passed to step() every tick with the session's arena and mode), the
// per-tick EventSink and a per-frame event
// accumulator for the presentation builder, the PresentationState, the
// InputSampler, the bot slots (an ai::AgentHost fed by a BotRoster), and the
// fixed-step clock.
//
// Two phases:
//   Lobby  (client-side, no world stepping). Slot 0 starts bound to keyboard
//          profile 0. Any unbound device whose Throw/Dash (any button on a
//          pad) is pressed joins the lowest free slot. botCount() further
//          slots are filled with bots. Enter / keypad Enter / pad Start (or
//          startMatch()) creates the world from that present mask and the
//          seed. curr() is initialWorld({seed, lobbyPresentMask()}) so the
//          arena can still be drawn.
//   Match  ticks are stepped by advance(). Local slots are sampled from the
//          InputSampler; bot slots are agents (§8) in an ai::AgentHost: the
//          reference bot at the lobby's difficulty tier, or a bot module
//          (BotRoster), attached afresh at every match start and restart
//          with seed botAgentSeed(botSeed, slot) and no input delay (local
//          play has no lead); a local
//          slot whose pad was removed is driven by "repeat last input" until
//          any unbound device presses a button (it is rebound to the lowest
//          such slot) or the match restarts (it becomes a bot). After
//          MatchEnded, Enter/Start returns to the lobby with the bindings kept.
//
// Per frame: push() every InputEvent, then advance(nowNs), then build the
// RenderState from renderPrev(), curr(), renderAlpha(), frameEvents().
#pragma once

#include "bot_roster.hpp"
#include "fixed_step_clock.hpp"
#include "kfm/ai/agent_host.hpp"
#include "kfm/ai/bot_spec.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/core/types.hpp"
#include "kfm/platform/input_event.hpp"
#include "kfm/platform/input_sampler.hpp"
#include "kfm/presentation/builder.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/world_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace kfm::client {

enum class SessionPhase : uint8_t { Lobby = 0, Match };

// Who drives a slot during a match.
enum class SlotOwner : uint8_t {
  None = 0,  // not present
  Local,     // a bound local device
  Dropped,   // local, but its pad was removed: repeats its last input
  Bot,       // an agent (BotRoster: the reference bot or a bot module)
  Remote,    // network mode: a slot driven by another client or the server
};

// How the frame is drawn (architecture §6.8).
enum class InterpolationPolicy : uint8_t {
  Auto = 0,     // newest tick when the display rate keeps up with the tick rate, else interpolate
  Interpolate,  // always lerp prev -> curr by the accumulator fraction
  Newest,       // always draw curr (alpha = 1, prev = curr)
};

// Auto renders the newest tick when displayHz >= kTickHz × timeScale × this
// (a tolerance so a 59.94 Hz display or a jittery measurement still counts).
inline constexpr double kNewestTickRateTolerance = 0.95;

// The bots' seed unless configured (--players-seed), separate from the
// world's.
inline constexpr uint64_t kDefaultBotSeed = 0x5C121DULL;

// The seed AgentHost::attach() gives the agent in `slot`: the same derivation
// as kfm_arena and kfm_server, so one bot seed means the same bots everywhere.
[[nodiscard]] constexpr uint64_t botAgentSeed(uint64_t botSeed, PlayerId slot) {
  return botSeed ^ (0x9E3779B97F4A7C15ULL * (uint64_t{slot.value} + 1));
}

struct SessionConfig {
  uint64_t seed{1};                      // world seed (MatchConfig::seed)
  uint8_t arena{0};                      // MatchConfig::arena / ::mode: indices into
  uint8_t mode{0};                       //   the content (out of range = 0)
  uint16_t powerUps{sim::kAllPowerUps};  // MatchConfig::powerUps
  uint8_t loadout{0};                    // MatchConfig::loadout (out of range = 0)
  // The loaded content (content/*.toml); nullptr = sim::defaultContent().
  // Copied: the session owns its content.
  const sim::ContentDB* content{nullptr};
  uint64_t botSeed{kDefaultBotSeed};  // the agents' seeds (botAgentSeed)
  uint8_t bots{0};                    // lobby bot count
  // What the bots are: the reference bot at a tier (default medium, the
  // casual lobby's), or a .wasm module (native only; one that cannot be
  // loaded falls back to the reference bot at spec.tier, see botSpecError()).
  ai::BotSpec botSpec{.kind = ai::BotKind::Reference, .tier = kDefaultBotTier, .path = {}};
  uint32_t maxTicksPerFrame{kDefaultMaxTicksPerFrame};
  bool bindKeyboardToSlot0{true};  // slot 0 starts bound to keyboard profile 0
  // The host starts what the lobby's start request means (network mode
  // connects instead of starting a local match): a start key in the lobby is
  // latched for takeStartRequest() and no local match begins.
  bool hostHandlesStart{false};
};

class MatchSession {
 public:
  // Per-frame event accumulator capacity (enough for kDefaultMaxTicksPerFrame
  // ticks of a full EventSink); further events are counted and dropped.
  static constexpr std::size_t kMaxFrameEvents =
      static_cast<std::size_t>(sim::kMaxEventsPerTick) * kDefaultMaxTicksPerFrame;

  explicit MatchSession(const SessionConfig& config = {});

  // --- per frame --------------------------------------------------------------
  // Feeds one platform input event (arrival order). Also watches for the
  // start key (Enter, keypad Enter, pad Start).
  void push(const platform::InputEvent& event);
  // Lobby: joins and the start request. Match: runs 0..maxTicksPerFrame ticks
  // (inputs sampled right before each step()), and handles rebinding and the
  // return to the lobby after MatchEnded. Clears frameEvents() first.
  void advance(uint64_t nowNs);

  // --- lobby ------------------------------------------------------------------
  [[nodiscard]] SessionPhase phase() const { return phase_; }
  // Clamped so bots never exceed the free slots at start.
  void setBotCount(uint8_t bots);
  [[nodiscard]] uint8_t botCount() const { return bots_; }
  // Bound slots plus the lowest botCount() unbound slots.
  [[nodiscard]] sim::PlayerMask lobbyPresentMask() const;
  // Creates the world from lobbyPresentMask() and seed() and starts the
  // clock at nowNs. Returns false (and stays in the lobby) when the mask is empty.
  bool startMatch(uint64_t nowNs);
  // hostHandlesStart: whether the lobby's start key was pressed since the
  // last call (cleared by the call).
  [[nodiscard]] bool takeStartRequest();
  void setHostHandlesStart(bool v) { hostHandlesStart_ = v; }
  // Back to the lobby; bindings are kept. Clears the pause (and any pending
  // single step) and the presentation state.
  void returnToLobby();

  // --- match control ----------------------------------------------------------
  // Re-creates the world with the same present mask and either the same seed
  // or a new one derived from it, re-seeds the bots and restarts the clock.
  // Dropped slots become bots. Like startMatch(), it clears the pause and any
  // pending single step.
  void restart(uint64_t nowNs, bool newSeed);
  [[nodiscard]] FixedStepClock& clock() { return clock_; }
  [[nodiscard]] const FixedStepClock& clock() const { return clock_; }
  void setPaused(bool paused) { clock_.setPaused(paused); }
  [[nodiscard]] bool paused() const { return clock_.paused(); }
  void requestSingleStep() { clock_.requestStep(); }
  void setTimeScale(double scale) { clock_.setTimeScale(scale); }

  [[nodiscard]] uint64_t seed() const { return seed_; }
  void setSeed(uint64_t seed);  // takes effect at the next start/restart
  // Arena and mode by index into content(); out of range falls back to 0.
  // Like setSeed(), they take effect at the next start/restart (at once in
  // the lobby).
  void setArena(uint8_t arena);
  void setMode(uint8_t mode);
  // The per-match power-up set (MatchConfig::powerUps) and the loadout
  // (MatchConfig::loadout, out of range = 0), same timing.
  void setPowerUps(uint16_t powerUps);
  void setLoadout(uint8_t loadout);
  [[nodiscard]] uint8_t arena() const { return arena_; }
  [[nodiscard]] uint8_t mode() const { return mode_; }
  [[nodiscard]] uint16_t powerUps() const { return powerUps_; }
  [[nodiscard]] uint8_t loadout() const { return loadout_; }
  // The config the current world was created with, and the content step()
  // runs it with (the live copy with this match's arena and mode).
  [[nodiscard]] const sim::MatchConfig& matchConfig() const { return matchConfig_; }
  [[nodiscard]] sim::MatchContent matchContent() const {
    return sim::matchContent(content_, matchConfig_);
  }
  [[nodiscard]] uint64_t botSeed() const { return botSeed_; }
  // The bots of the next start/restart (a running match keeps its agents).
  // A module spec loads the file now unless it is already loaded; false
  // (and botSpecError()) when it could not be: the bots are then reference
  // bots at spec.tier.
  bool setBotSpec(const ai::BotSpec& spec);
  // Only the tier of the current spec (the lobby's Bot difficulty row).
  void setBotTier(ai::ReferenceTier tier);
  [[nodiscard]] const ai::BotSpec& botSpec() const { return roster_.spec(); }
  [[nodiscard]] const BotRoster& botRoster() const { return roster_; }
  // Why the last module spec fell back to the reference bot (empty: it did not).
  [[nodiscard]] const std::string& botSpecError() const { return botSpecError_; }
  // Match: the agent counters of a bot slot (zero for any other slot).
  [[nodiscard]] BotSlotStats botStats(PlayerId slot) const;
  [[nodiscard]] const ai::AgentHost& agents() const { return agents_; }

  // --- state ------------------------------------------------------------------
  [[nodiscard]] const sim::WorldState& prev() const { return prev_; }
  [[nodiscard]] const sim::WorldState& curr() const { return curr_; }
  [[nodiscard]] uint32_t checksum() const;
  [[nodiscard]] const sim::InputFrame& lastFrame() const { return lastFrame_; }

  // What the builder draws this frame, per the interpolation policy.
  [[nodiscard]] const sim::WorldState& renderPrev() const;
  [[nodiscard]] float renderAlpha() const;
  [[nodiscard]] bool rendersNewest() const;
  void setInterpolationPolicy(InterpolationPolicy p) { policy_ = p; }
  [[nodiscard]] InterpolationPolicy interpolationPolicy() const { return policy_; }

  // Events of every tick stepped by the last advance(), each exactly once.
  [[nodiscard]] std::span<const sim::SimEvent> frameEvents() const {
    return {frameEvents_.data(), frameEventCount_};
  }
  [[nodiscard]] uint64_t droppedEvents() const { return droppedEvents_; }
  [[nodiscard]] uint32_t ticksThisFrame() const { return ticksThisFrame_; }

  [[nodiscard]] presentation::PresentationState& presentation() { return presentation_; }

  // The live content copy (edited by the debug UI) and the loaded content
  // it started from (its defaults). The mutable accessor marks the agents'
  // cached content hash stale (AgentHost caches it per ContentDB object and
  // this one is edited in place).
  [[nodiscard]] sim::ContentDB& content() {
    agents_.invalidateContentHash();
    return content_;
  }
  [[nodiscard]] const sim::ContentDB& content() const { return content_; }
  [[nodiscard]] const sim::ContentDB& loadedContent() const { return loaded_; }
  void resetContent();
  [[nodiscard]] uint32_t contentHash() const;
  [[nodiscard]] bool contentIsDefault() const;
  // Hot reload (§5.10): replaces the loaded content and the live copy (debug
  // UI edits are dropped), keeps the arena, mode and loadout when the new content
  // still has them (else index 0), and restarts a running match with the
  // same seed, or re-creates the lobby world.
  void replaceContent(const sim::ContentDB& content, uint64_t nowNs);

  [[nodiscard]] platform::InputSampler& sampler() { return sampler_; }
  [[nodiscard]] const platform::InputSampler& sampler() const { return sampler_; }

  // Match: who drives each slot. Lobby: Local for bound slots, Bot for the
  // slots bots would fill, else None.
  [[nodiscard]] SlotOwner owner(PlayerId slot) const;
  [[nodiscard]] sim::PlayerMask localMask() const { return localMask_; }
  [[nodiscard]] sim::PlayerMask botMask() const { return botMask_; }
  // Slots whose pad was removed and that are not rebound yet (for a notice).
  [[nodiscard]] sim::PlayerMask droppedMask() const { return sampler_.droppedSlots(); }

 private:
  void createWorld(sim::PlayerMask present);
  void beginMatch(sim::PlayerMask present, sim::PlayerMask local, uint64_t nowNs);
  void detachAgents();
  void handleLobbyJoins();
  void handleRebinds();
  void stepOnce(uint64_t tickStartNs, uint64_t tickWallNs, uint64_t tickEndNs);
  void appendEvents(const sim::EventSink& sink);

  SessionPhase phase_{SessionPhase::Lobby};
  uint64_t seed_{1};
  uint8_t arena_{0};
  uint8_t mode_{0};
  uint16_t powerUps_{sim::kAllPowerUps};
  uint8_t loadout_{0};
  sim::MatchConfig matchConfig_{};
  uint64_t botSeed_{kDefaultBotSeed};
  uint8_t bots_{0};
  bool startRequested_{false};
  bool hostHandlesStart_{false};
  bool hostStartPending_{false};

  sim::ContentDB loaded_{};
  sim::ContentDB content_{};
  sim::WorldState prev_{};
  sim::WorldState curr_{};
  sim::EventSink sink_{};
  sim::InputFrame lastFrame_{};
  std::array<sim::PlayerInput, sim::kMaxPlayers> lastInputs_{};
  BotRoster roster_{};
  std::string botSpecError_;
  ai::AgentHost agents_{};
  sim::PlayerMask moduleMask_{0};  // bot slots whose agent is a WasmAgent
  sim::PlayerMask localMask_{0};
  sim::PlayerMask botMask_{0};

  std::array<sim::SimEvent, kMaxFrameEvents> frameEvents_{};
  std::size_t frameEventCount_{0};
  uint64_t droppedEvents_{0};
  uint32_t ticksThisFrame_{0};

  presentation::PresentationState presentation_{};
  platform::InputSampler sampler_{};
  FixedStepClock clock_{};
  InterpolationPolicy policy_{InterpolationPolicy::Auto};
};

}  // namespace kfm::client
