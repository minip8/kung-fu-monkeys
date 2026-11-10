// BotFactory (architecture §8): BotSpec -> IAgent for every host. Reference
// specs everywhere; module specs over tests/fixtures/bots when the build runs
// modules (KFM_WASM_BOTS), an Unsupported error without opening a file when
// it does not.
#include "kfm/ai/bot_factory.hpp"

#include "kfm/ai/bot_spec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace kfm;
using namespace kfm::ai;

namespace {

[[nodiscard]] BotSpec moduleSpec(const std::string& name) {
  return BotSpec{BotKind::Module, ReferenceTier::Hard,
                 std::string(KFM_BOT_FIXTURES_DIR) + "/" + name};
}

}  // namespace

TEST_CASE("BotFactory makes a fresh reference bot per call at every tier", "[ai][factory]") {
  BotFactory factory;
  for (const ReferenceTier tier :
       {ReferenceTier::Easy, ReferenceTier::Medium, ReferenceTier::Hard}) {
    const BotSpec spec{BotKind::Reference, tier, {}};
    CHECK(factory.prepare(spec).has_value());
    auto a = factory.make(spec);
    auto b = factory.make(spec);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->agent != nullptr);
    CHECK(a->agent != b->agent);
    CHECK(a->wasm == nullptr);
  }
}

#ifdef KFM_AI_TEST_WASM_BOTS
TEST_CASE("BotFactory loads a module once and makes one instance per call", "[ai][factory][wasm]") {
  REQUIRE(botModulesAvailable());
  BotFactory factory(1234);
  const BotSpec echo = moduleSpec("echo.wasm");
  REQUIRE(factory.prepare(echo).has_value());
  auto a = factory.make(echo);
  auto b = factory.make(echo);
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK(a->agent != nullptr);
  CHECK(a->wasm != nullptr);
  CHECK(a->wasm != b->wasm);

  const auto missing = factory.make(moduleSpec("does_not_exist.wasm"));
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().kind == BotErrorKind::Io);
  CHECK(missing.error().message.contains("does_not_exist.wasm"));

  const auto refused = factory.prepare(moduleSpec("abi_mismatch.wasm"));
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().kind == BotErrorKind::Load);
  CHECK(refused.error().message.contains("abi_mismatch.wasm"));
  CHECK(refused.error().message.contains("(abi-mismatch)"));
}
#else
TEST_CASE("BotFactory without the runtime refuses a module spec", "[ai][factory]") {
  CHECK_FALSE(botModulesAvailable());
  BotFactory factory;
  const auto made = factory.make(moduleSpec("echo.wasm"));
  REQUIRE_FALSE(made.has_value());
  CHECK(made.error().kind == BotErrorKind::Unsupported);
  CHECK(made.error().message.contains("echo.wasm"));
}
#endif
