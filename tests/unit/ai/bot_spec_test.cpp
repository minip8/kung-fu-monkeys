#include "kfm/ai/bot_spec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using namespace kfm::ai;

namespace {

// Fails the test case when the text does not parse. value_or rather than a
// dereference after the check: clang-tidy cannot see that FAIL throws, and
// MSVC can, so a return after it is unreachable code (C4702).
BotSpec parsed(std::string_view text) {
  const std::optional<BotSpec> spec = parseBotSpec(text);
  if (!spec) FAIL("did not parse: " << text);
  return spec.value_or(BotSpec{});
}

}  // namespace

TEST_CASE("BotSpec parses the reference bot and its tiers", "[ai][spec]") {
  const BotSpec plain = parsed("ref");
  CHECK(plain.kind == BotKind::Reference);
  CHECK(plain.tier == ReferenceTier::Hard);
  CHECK(plain.referenceConfig() == ReferenceBotConfig::hard());

  const BotSpec easy = parsed("ref:easy");
  CHECK(easy.tier == ReferenceTier::Easy);
  CHECK(easy.referenceConfig() == ReferenceBotConfig::easy());
  CHECK(parsed("ref:medium").referenceConfig() == ReferenceBotConfig::medium());
  CHECK(toString(easy) == "ref:easy");
  CHECK(toString(plain) == "ref:hard");
}

TEST_CASE("BotSpec parses a module path and rejects the rest", "[ai][spec]") {
  const BotSpec module = parsed("bots/chaser.wasm");
  CHECK(module.kind == BotKind::Module);
  CHECK(module.path == "bots/chaser.wasm");
  CHECK(toString(module) == "bots/chaser.wasm");
  CHECK_FALSE(parseBotSpec(""));
  CHECK_FALSE(parseBotSpec("ref:"));
  CHECK_FALSE(parseBotSpec("ref:impossible"));
  CHECK_FALSE(parseBotSpec(".wasm"));
  CHECK_FALSE(parseBotSpec("chaser.wat"));
  CHECK_FALSE(parseBotSpec("scripted"));
}
