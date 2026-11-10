// BotFactory (architecture §8): turns a BotSpec into a fresh IAgent, the one
// copy every bot host uses (kfm_arena, kfm_server's ServerHost and the
// client's BotRoster). A reference spec is a ReferenceBot at its tier. A
// module spec is read from its file and WasmModule::load()ed once per
// distinct path (the cache), under a WasmRuntime the factory owns while any
// module is loaded; every agent made from it is its own WasmAgent (own
// instance and linear memory).
//
// Its own target, kfm::ai_bots, so kfm_ai never sees WAMR: built with
// KFM_WASM_BOTS it links kfm::ai_wasm privately and runs modules; without it
// (the browser, -DKFM_WASM_BOTS=OFF) a module spec is an Unsupported error
// and no file is ever opened. This is the one place in engine/ai that reads
// a file (a module's bytes); the observation, the agents and AgentHost never
// do. What an error means is the host's policy, not the factory's: kfm_arena
// exits 3 (4 for Io), kfm_server refuses to start, the client falls back to
// the reference bot.
#pragma once

#include "kfm/ai/agent.hpp"
#include "kfm/ai/bot_spec.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace kfm::ai {

class WasmAgent;

// Whether this build's factory runs bot modules (KFM_WASM_BOTS).
[[nodiscard]] bool botModulesAvailable();

enum class BotErrorKind : uint8_t {
  Io,           // the module file could not be read
  Load,         // WasmModule::load refused it, or the runtime failed to initialise
  Unsupported,  // a module spec in a build without KFM_WASM_BOTS
};

struct BotError {
  BotErrorKind kind{BotErrorKind::Load};
  // One line: "cannot read PATH", "PATH: <LoadError message> (<kind>)",
  // "PATH: this build runs no bot modules (configured with KFM_WASM_BOTS=OFF)".
  std::string message;
};

class BotFactory {
 public:
  // fuelPerTick: every module's kfm_act budget (WasmAgentConfig::fuelPerTick);
  // 0 keeps the runtime default (100 000). The other WasmAgentConfig knobs
  // are the defaults.
  explicit BotFactory(uint32_t fuelPerTick = 0);
  ~BotFactory();
  BotFactory(const BotFactory&) = delete;
  BotFactory& operator=(const BotFactory&) = delete;
  BotFactory(BotFactory&&) noexcept;
  BotFactory& operator=(BotFactory&&) noexcept;

  // Loads a module spec's file into the cache unless it is there (a
  // reference spec: nothing to do). Reads at most maxModuleBytes + 1 bytes,
  // so an oversized file is refused as too large without reading all of it.
  [[nodiscard]] std::expected<void, BotError> prepare(const BotSpec& spec);

  struct Made {
    std::unique_ptr<IAgent> agent;  // uninitialised: AgentHost::attach calls init()
    WasmAgent* wasm{nullptr};       // the same agent when it is a module, for its stats
  };
  // A fresh agent for the spec (prepare()s a module first).
  [[nodiscard]] std::expected<Made, BotError> make(const BotSpec& spec);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kfm::ai
