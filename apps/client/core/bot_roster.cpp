#include "bot_roster.hpp"

#include "kfm/ai/reference_bot.hpp"

#ifdef KFM_CLIENT_WASM_BOTS
#include "kfm/ai/wasm_runtime.hpp"

#include <cstddef>
#include <expected>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <vector>
#endif

#include <utility>

#ifndef KFM_CLIENT_WASM_BOTS
namespace kfm::ai {
// Complete (empty) types for the unique_ptr deleters of a build without the
// runtime; they are never instantiated.
class WasmModule {};
class WasmRuntime {};
}  // namespace kfm::ai
#endif

namespace kfm::client {

bool moduleBotsSupported() {
#ifdef KFM_CLIENT_WASM_BOTS
  return true;
#else
  return false;
#endif
}

BotRoster::BotRoster() = default;
BotRoster::~BotRoster() = default;
BotRoster::BotRoster(BotRoster&&) noexcept = default;
BotRoster& BotRoster::operator=(BotRoster&&) noexcept = default;

bool BotRoster::moduleActive() const {
  return spec_.kind == ai::BotKind::Module && module_ != nullptr;
}

std::string BotRoster::describe() const {
  if (moduleActive()) return spec_.path;
  return "ref:" + std::string(ai::toString(spec_.tier));
}

bool BotRoster::configure(const ai::BotSpec& spec, std::string& error) {
  spec_ = spec;
  if (spec.kind != ai::BotKind::Module) return true;
  if (module_ != nullptr && modulePath_ == spec.path) return true;
  module_.reset();
  modulePath_.clear();
#ifdef KFM_CLIENT_WASM_BOTS
  std::ifstream in(spec.path, std::ios::binary);
  if (!in) {
    error = "cannot read the bot module '" + spec.path + "'";
    return false;
  }
  const std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
  const std::span<const std::byte> bytes = std::as_bytes(std::span<const char>(chars));
  if (runtime_ == nullptr) runtime_ = std::make_unique<ai::WasmRuntime>();
  if (!runtime_->ok()) {
    error = "the wasm bot runtime failed to initialise";
    return false;
  }
  std::expected<ai::WasmModule, ai::LoadError> loaded = ai::WasmModule::load(bytes);
  if (!loaded) {
    error = "bot module '" + spec.path + "' refused (" +
            std::string(ai::toString(loaded.error().kind)) + "): " + loaded.error().message;
    return false;
  }
  module_ = std::make_unique<ai::WasmModule>(std::move(*loaded));
  modulePath_ = spec.path;
  return true;
#else
  error = "bot modules are not supported in this build ('" + spec.path + "')";
  return false;
#endif
}

std::unique_ptr<ai::IAgent> BotRoster::make() const {
#ifdef KFM_CLIENT_WASM_BOTS
  if (moduleActive()) return std::make_unique<ai::WasmAgent>(*module_);
#endif
  return std::make_unique<ai::ReferenceBot>(spec_.referenceConfig());
}

void BotRoster::moduleStats([[maybe_unused]] const ai::IAgent& agent,
                            [[maybe_unused]] BotSlotStats& out) {
#ifdef KFM_CLIENT_WASM_BOTS
  const auto& wasm = static_cast<const ai::WasmAgent&>(agent);
  const ai::WasmAgentStats& st = wasm.stats();
  out.module = true;
  out.alive = wasm.alive();
  out.traps = st.traps;
  out.overBudget = st.overBudget;
  out.badReturns = st.badReturns;
  out.fuelLast = st.fuelLast;
  out.fuelMax = st.fuelMax;
#endif
}

}  // namespace kfm::client
