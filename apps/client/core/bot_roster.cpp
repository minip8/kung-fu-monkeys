#include "bot_roster.hpp"

#include "kfm/ai/reference_bot.hpp"

#ifdef KFM_CLIENT_WASM_BOTS
#include "kfm/ai/wasm_runtime.hpp"
#endif

#include <expected>
#include <utility>

namespace kfm::client {

bool moduleBotsSupported() {
  return ai::botModulesAvailable();
}

bool BotRoster::moduleActive() const {
  return spec_.kind == ai::BotKind::Module && moduleActive_;
}

std::string BotRoster::describe() const {
  if (moduleActive()) return spec_.path;
  return "ref:" + std::string(ai::toString(spec_.tier));
}

bool BotRoster::configure(const ai::BotSpec& spec, std::string& error) {
  spec_ = spec;
  moduleActive_ = false;
  if (spec.kind != ai::BotKind::Module) return true;
  // The factory caches every module it loaded: the same path again (a tier
  // change, a lobby round trip) does not read the file again.
  const std::expected<void, ai::BotError> ok = factory_.prepare(spec);
  if (!ok) {
    error = ok.error().kind == ai::BotErrorKind::Unsupported
                ? "bot modules are not supported in this build ('" + spec.path + "')"
                : "bot module refused: " + ok.error().message;
    return false;
  }
  moduleActive_ = true;
  return true;
}

std::unique_ptr<ai::IAgent> BotRoster::make() {
  if (moduleActive()) {
    std::expected<ai::BotFactory::Made, ai::BotError> made = factory_.make(spec_);
    if (made) return std::move(made->agent);
  }
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
