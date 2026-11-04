// BotSpec (architecture §8): how every host names a bot on its command line
// or in a lobby setting, so kfm_arena, kfm_server and kfm_client share one
// grammar:
//   ref              the built-in reference bot at its hard tier
//   ref:easy|medium|hard   the reference bot at that difficulty tier
//   <path>.wasm      a bot module (ABI v1) loaded from that file
// Parsing only: engine/ai never touches the file system, so the host reads
// the module bytes and builds a WasmAgent itself (apps/, §4).
#pragma once

#include "kfm/ai/reference_bot.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace kfm::ai {

enum class BotKind : uint8_t { Reference = 0, Module };

enum class ReferenceTier : uint8_t { Easy = 0, Medium, Hard };

struct BotSpec {
  BotKind kind{BotKind::Reference};
  ReferenceTier tier{ReferenceTier::Hard};  // Reference only
  std::string path;                         // Module only: the .wasm file

  [[nodiscard]] ReferenceBotConfig referenceConfig() const {
    switch (tier) {
      case ReferenceTier::Easy:
        return ReferenceBotConfig::easy();
      case ReferenceTier::Medium:
        return ReferenceBotConfig::medium();
      case ReferenceTier::Hard:
        break;
    }
    return ReferenceBotConfig::hard();
  }
  bool operator==(const BotSpec&) const = default;
};

// "ref", "ref:<tier>" or a path ending in ".wasm"; nullopt otherwise.
[[nodiscard]] std::optional<BotSpec> parseBotSpec(std::string_view text);
// The text parseBotSpec() accepts for a spec ("ref:hard", the path).
[[nodiscard]] std::string toString(const BotSpec& spec);
[[nodiscard]] std::string_view toString(ReferenceTier tier);
[[nodiscard]] std::optional<ReferenceTier> parseReferenceTier(std::string_view text);

}  // namespace kfm::ai
