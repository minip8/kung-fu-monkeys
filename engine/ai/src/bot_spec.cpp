#include "kfm/ai/bot_spec.hpp"

namespace kfm::ai {

std::string_view toString(ReferenceTier tier) {
  switch (tier) {
    case ReferenceTier::Easy:
      return "easy";
    case ReferenceTier::Medium:
      return "medium";
    case ReferenceTier::Hard:
      break;
  }
  return "hard";
}

std::optional<ReferenceTier> parseReferenceTier(std::string_view text) {
  if (text == "easy") return ReferenceTier::Easy;
  if (text == "medium") return ReferenceTier::Medium;
  if (text == "hard") return ReferenceTier::Hard;
  return std::nullopt;
}

std::optional<BotSpec> parseBotSpec(std::string_view text) {
  if (text.empty()) return std::nullopt;
  if (text == "ref") return BotSpec{};
  constexpr std::string_view kRefPrefix = "ref:";
  if (text.starts_with(kRefPrefix)) {
    const std::optional<ReferenceTier> tier = parseReferenceTier(text.substr(kRefPrefix.size()));
    if (!tier) return std::nullopt;
    return BotSpec{BotKind::Reference, *tier, {}};
  }
  constexpr std::string_view kSuffix = ".wasm";
  if (text.size() > kSuffix.size() && text.ends_with(kSuffix)) {
    return BotSpec{BotKind::Module, ReferenceTier::Hard, std::string(text)};
  }
  return std::nullopt;
}

std::string toString(const BotSpec& spec) {
  if (spec.kind == BotKind::Module) return spec.path;
  return "ref:" + std::string(toString(spec.tier));
}

}  // namespace kfm::ai
