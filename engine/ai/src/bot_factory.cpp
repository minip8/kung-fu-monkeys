// BotFactory (see bot_factory.hpp).
#include "kfm/ai/bot_factory.hpp"

#include "kfm/ai/reference_bot.hpp"

#ifdef KFM_AI_BOTS_WASM
#include "kfm/ai/wasm_runtime.hpp"

#include <array>
#include <cstddef>
#include <fstream>
#include <functional>
#include <ios>
#include <map>
#include <vector>
#endif

#include <format>
#include <utility>

namespace kfm::ai {

namespace {

#ifdef KFM_AI_BOTS_WASM
// The whole file, at most `cap` + 1 bytes (one more than the cap makes the
// module loader refuse it as too large).
[[nodiscard]] std::expected<std::vector<std::byte>, BotError> readModule(const std::string& path,
                                                                         size_t cap) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::unexpected(BotError{BotErrorKind::Io, std::format("cannot read {}", path)});
  std::vector<std::byte> out;
  std::array<char, 16384> chunk{};
  while (out.size() <= cap && f) {
    f.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const auto n = static_cast<size_t>(f.gcount());
    for (size_t i = 0; i < n; ++i)
      out.push_back(static_cast<std::byte>(chunk[i]));
  }
  if (f.bad()) {
    return std::unexpected(BotError{BotErrorKind::Io, std::format("cannot read {}", path)});
  }
  if (out.size() > cap + 1) out.resize(cap + 1);
  return out;
}
#endif

}  // namespace

struct BotFactory::Impl {
  uint32_t fuelPerTick{0};
#ifdef KFM_AI_BOTS_WASM
  std::unique_ptr<WasmRuntime> runtime;
  std::map<std::string, WasmModule, std::less<>> modules;  // one per distinct path
#endif
};

bool botModulesAvailable() {
#ifdef KFM_AI_BOTS_WASM
  return true;
#else
  return false;
#endif
}

BotFactory::BotFactory(uint32_t fuelPerTick) : impl_(std::make_unique<Impl>()) {
  impl_->fuelPerTick = fuelPerTick;
}

BotFactory::~BotFactory() = default;
BotFactory::BotFactory(BotFactory&&) noexcept = default;
BotFactory& BotFactory::operator=(BotFactory&&) noexcept = default;

std::expected<void, BotError> BotFactory::prepare(const BotSpec& spec) {
  if (spec.kind == BotKind::Reference) return {};
#ifdef KFM_AI_BOTS_WASM
  if (impl_->modules.contains(spec.path)) return {};
  if (!impl_->runtime) {
    impl_->runtime = std::make_unique<WasmRuntime>();
    if (!impl_->runtime->ok()) {
      impl_->runtime.reset();
      return std::unexpected(BotError{BotErrorKind::Load, "the wasm runtime failed to initialise"});
    }
  }
  WasmAgentConfig config{};
  if (impl_->fuelPerTick != 0) config.fuelPerTick = impl_->fuelPerTick;
  auto bytes = readModule(spec.path, config.maxModuleBytes);
  if (!bytes) return std::unexpected(std::move(bytes.error()));
  auto module = WasmModule::load(*bytes, config);
  if (!module) {
    return std::unexpected(
        BotError{BotErrorKind::Load, std::format("{}: {} ({})", spec.path, module.error().message,
                                                 toString(module.error().kind))});
  }
  impl_->modules.emplace(spec.path, std::move(*module));
  return {};
#else
  return std::unexpected(
      BotError{BotErrorKind::Unsupported,
               std::format("{}: this build runs no bot modules (configured with KFM_WASM_BOTS=OFF)",
                           spec.path)});
#endif
}

std::expected<BotFactory::Made, BotError> BotFactory::make(const BotSpec& spec) {
  if (spec.kind == BotKind::Reference) {
    return Made{std::make_unique<ReferenceBot>(spec.referenceConfig()), nullptr};
  }
  if (auto ok = prepare(spec); !ok) return std::unexpected(std::move(ok.error()));
#ifdef KFM_AI_BOTS_WASM
  const auto it = impl_->modules.find(spec.path);
  auto agent = std::make_unique<WasmAgent>(it->second);
  WasmAgent* raw = agent.get();
  return Made{std::move(agent), raw};
#else
  return std::unexpected(BotError{BotErrorKind::Unsupported, "no bot modules in this build"});
#endif
}

}  // namespace kfm::ai
