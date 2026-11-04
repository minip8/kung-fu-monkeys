// BotRoster: what the local client's bot slots are (architecture §8: the
// reference bot fills lobby slots in the shipped game; the native client runs
// bot modules as local players through the same runtime as the server and
// kfm_arena). SDL-free; MatchSession owns one and asks it for a fresh agent
// per bot slot at every match start.
//
// A spec is an ai::BotSpec: the reference bot at a difficulty tier (casual
// lobbies, default medium), or a .wasm bot module. A module is read from
// disk and validated once by configure() and cached; make() then gives every
// bot slot its own WasmAgent (own instance and linear memory) over it. A
// module that cannot be loaded (missing file, refused by the loader, or a
// build without KFM_WASM_BOTS, e.g. the browser) falls back to the reference
// bot at the spec's tier, and configure() reports why: the client always
// starts.
#pragma once

#include "kfm/ai/agent.hpp"
#include "kfm/ai/bot_spec.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace kfm::ai {
class WasmModule;
class WasmRuntime;
}  // namespace kfm::ai

namespace kfm::client {

// The default of casual lobbies (§8: tiers exist only for the reference bot
// there; `hard` is the contest baseline).
inline constexpr ai::ReferenceTier kDefaultBotTier = ai::ReferenceTier::Medium;

// Whether this build can run bot modules (KFM_WASM_BOTS, native only).
[[nodiscard]] bool moduleBotsSupported();

// Per-bot counters for the debug overlay: the host's (AgentHost::stats) and,
// for a module, the runtime's (WasmAgent::stats).
struct BotSlotStats {
  uint32_t acts{0};
  uint32_t substitutions{0};  // ticks whose input repeated the last one
  bool module{false};         // the fields below are meaningful
  bool alive{true};           // a module whose init faulted is dead
  uint64_t traps{0};
  uint64_t overBudget{0};
  uint64_t badReturns{0};
  uint32_t fuelLast{0};
  uint32_t fuelMax{0};
};

class BotRoster {
 public:
  BotRoster();
  ~BotRoster();
  BotRoster(const BotRoster&) = delete;
  BotRoster& operator=(const BotRoster&) = delete;
  BotRoster(BotRoster&&) noexcept;
  BotRoster& operator=(BotRoster&&) noexcept;

  // Makes `spec` the spec of the next agents. A module spec reads and loads
  // the file unless it is the module already loaded. False (with `error`)
  // when a module could not be loaded: the roster then makes reference bots
  // at spec.tier.
  bool configure(const ai::BotSpec& spec, std::string& error);

  [[nodiscard]] const ai::BotSpec& spec() const { return spec_; }
  // Whether make() returns module agents (a module spec that loaded).
  [[nodiscard]] bool moduleActive() const;
  // "ref:medium" or the module's path.
  [[nodiscard]] std::string describe() const;

  // A fresh, uninitialised agent (AgentHost::attach calls init()).
  [[nodiscard]] std::unique_ptr<ai::IAgent> make() const;

  // The module counters of an agent make() returned while moduleActive()
  // (no RTTI: the caller remembers which agents are modules). Fills the
  // module fields of `out`; a no-op in a build without module bots.
  static void moduleStats(const ai::IAgent& agent, BotSlotStats& out);

 private:
  ai::BotSpec spec_{.kind = ai::BotKind::Reference, .tier = kDefaultBotTier, .path = {}};
  std::unique_ptr<ai::WasmRuntime> runtime_;
  std::unique_ptr<ai::WasmModule> module_;  // set while a module spec is loaded
  std::string modulePath_;                  // the path module_ was loaded from
};

}  // namespace kfm::client
