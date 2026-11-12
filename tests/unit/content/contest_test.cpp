// The contest's content as shipped (architecture §11 "Contest design", v0.79):
// the _1t power-up twins and the contest_base baseline.
#include "kfm/content/loader.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/visit.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <tuple>
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

}  // namespace

TEST_CASE("the _1t power-up twins are their originals counted in 1-tick turns (v0.79)",
          "[content][contest][powerup]") {
  const ContentDB& db = shipped();
  const std::array<std::tuple<std::string_view, std::string_view, uint16_t>, 4> twins{{
      {"big_banana_1t", "big_banana", 60},
      {"triplebanana_1t", "triple_banana", 90},
      {"quick_hands_1t", "quick_hands", 60},
      {"shield_1t", "shield", 90},
  }};
  const uint8_t gridBase = db.findPowerUp("grid_base_1t").value_or(0xFF);
  for (const auto& [name, original, turns] : twins) {
    INFO(name);
    const std::optional<uint8_t> t = db.findPowerUp(name);
    const std::optional<uint8_t> o = db.findPowerUp(original);
    REQUIRE(t);
    REQUIRE(o);
    CHECK(t.value_or(0) > gridBase);  // appended: the earlier indices stay
    const sim::PowerUpDef& orig = db.powerUps[o.value_or(0)];
    sim::PowerUpDef twin = db.powerUps[t.value_or(0)];
    CHECK(twin.durationTicks == turns);
    // Everything else, grants and looks, is the original's.
    twin.name = orig.name;
    twin.durationTicks = orig.durationTicks;
    CHECK(bytesOf(twin) == bytesOf(orig));
  }
}

TEST_CASE("contest_base is grid_base_1t with the banana at 2 cells a turn (v0.79)",
          "[content][contest][powerup]") {
  const ContentDB& db = shipped();
  const std::optional<uint8_t> base = db.findPowerUp("contest_base");
  const std::optional<uint8_t> grid = db.findPowerUp("grid_base_1t");
  REQUIRE(base);
  REQUIRE(grid);
  const sim::PowerUpDef& g = db.powerUps[grid.value_or(0)];
  sim::PowerUpDef twin = db.powerUps[base.value_or(0)];
  REQUIRE(twin.grantCount == 3);
  REQUIRE(twin.grants[2].kind == sim::GrantKind::Projectile);
  CHECK(twin.grants[2].projectile.speedMul == Fixed::fromRatio(15, 2));
  // 2 cells per 1-tick turn, within the per-tick truncation of 16 u/s.
  const Fixed perTurn = db.boomerang.speed * twin.grants[2].projectile.speedMul;
  CHECK(abs(perTurn - Fixed::fromInt(2)) < Fixed::fromRatio(1, 1000));
  // grid_rounds_1t's stays at 4.
  CHECK(g.grants[2].projectile.speedMul == Fixed::fromInt(4));
  twin.name = g.name;
  twin.grants[2].projectile.speedMul = g.grants[2].projectile.speedMul;
  CHECK(bytesOf(twin) == bytesOf(g));
}
