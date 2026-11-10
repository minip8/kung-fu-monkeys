// The scenario tests (tests/scenario) link only the sim, so the shipped defs
// they exercise are hand copies of content/ (loadout_util.hpp,
// powerup_util.hpp). This test loads the shipped TOML and checks every such
// mirror still equals its def byte for byte through the visitor, so a
// tuning change in content/ that the copies miss fails here rather than
// leaving the scenarios testing numbers nobody ships. Indices into the
// content (a grant's ability, a loadout's slots) are compared by the name
// they resolve to, since a mirror built on defaultContent() numbers its
// defs differently.
#include "../../scenario/loadout_util.hpp"
#include "../../scenario/powerup_util.hpp"
#include "kfm/content/loader.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/visit.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

using namespace kfm;
using sim::ContentDB;

namespace {

template <class T>
std::vector<std::byte> bytesOf(const T& def) {
  std::vector<std::byte> buf(sizeof(T) * 2 + 16);
  BitWriter w(buf);
  REQUIRE(sim::writeTo(w, def));
  buf.resize(w.bytesWritten());
  return buf;
}

const ContentDB& shipped() {
  static const ContentDB db = [] {
    ContentDB c{};
    content::Diagnostics diag;
    REQUIRE(content::loadContentDir(KFM_CONTENT_DIR, c, diag));
    return c;
  }();
  return db;
}

std::optional<uint8_t> findAbility(const ContentDB& c, std::string_view name) {
  for (uint8_t i = 0; i < c.abilityCount; ++i) {
    if (c.abilities[i].name.view() == name) return i;
  }
  return std::nullopt;
}

const sim::AbilityDef& shippedAbility(std::string_view name) {
  const std::optional<uint8_t> i = findAbility(shipped(), name);
  INFO("ability " << name);
  REQUIRE(i.has_value());
  return shipped().abilities[i.value_or(0)];
}

// `index` into `from`'s abilities, renumbered to the shipped def of the same name.
uint8_t shippedIndexOf(const ContentDB& from, uint8_t index) {
  REQUIRE(index < from.abilityCount);
  const std::string_view name = from.abilities[index].name.view();
  const std::optional<uint8_t> i = findAbility(shipped(), name);
  INFO("ability " << name);
  REQUIRE(i.has_value());
  return i.value_or(0);
}

}  // namespace

TEST_CASE("the scenarios' ability defs equal the shipped ones", "[content][mirror]") {
  using namespace sim::test;
  CHECK(bytesOf(blinkDef()) == bytesOf(shippedAbility("blink")));
  CHECK(bytesOf(slamDef()) == bytesOf(shippedAbility("slam")));
  CHECK(bytesOf(parryDef()) == bytesOf(shippedAbility("parry")));
  CHECK(bytesOf(callDef()) == bytesOf(shippedAbility("grid_call")));

  const ContentDB p = powerUpContent();
  const std::optional<uint8_t> fast = findAbility(p, "throw_fast");
  REQUIRE(fast.has_value());
  CHECK(bytesOf(p.abilities[fast.value_or(0)]) == bytesOf(shippedAbility("throw_fast")));
}

TEST_CASE("the scenarios' power-ups equal the shipped ones", "[content][mirror]") {
  using namespace sim::test;
  const ContentDB p = powerUpContent();
  const std::array<std::pair<uint8_t, std::string_view>, 5> expected{{
      {kSpeed, "speed_boost"},
      {kBig, "big_banana"},
      {kTriple, "triple_banana"},
      {kShield, "shield"},
      {kFastHands, "fast_hands"},
  }};
  REQUIRE(p.powerUpCount == expected.size());
  for (const auto& [index, name] : expected) {
    INFO("power-up " << name);
    // The scenario's index is the shipped one too (powerups.toml order).
    REQUIRE(shipped().findPowerUp(name) == std::optional<uint8_t>{index});
    sim::PowerUpDef mirror = p.powerUps[index];
    for (uint8_t g = 0; g < mirror.grantCount; ++g) {
      if (mirror.grants[g].kind == sim::GrantKind::AbilityOverride) {
        mirror.grants[g].ability = shippedIndexOf(p, mirror.grants[g].ability);
      }
    }
    CHECK(bytesOf(mirror) == bytesOf(shipped().powerUps[index]));
  }
}

TEST_CASE("the scenarios' ability loadouts equal the shipped ones", "[content][mirror]") {
  using namespace sim::test;
  for (const sim::AbilityDef& def : {blinkDef(), slamDef(), parryDef()}) {
    const std::string_view name = def.name.view();
    INFO("loadout " << name);
    uint8_t extra = 0;
    ContentDB c = withAbility(def, extra);
    const uint8_t index = addLoadout(c, name, extra);  // what the scenarios build
    sim::LoadoutDef mirror = c.loadouts[index];
    for (uint8_t& a : mirror.abilities)
      a = shippedIndexOf(c, a);
    const std::optional<uint8_t> shippedIndex = shipped().findLoadout(name);
    REQUIRE(shippedIndex.has_value());
    CHECK(bytesOf(mirror) == bytesOf(shipped().loadouts[shippedIndex.value_or(0)]));
  }
}
