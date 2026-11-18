// The large free-for-all arenas (architecture §5.10, v0.91): meadow,
// archipelago, compound and marsh, appended after the contest pool. Each is
// clearly bigger than the dojo's 32 x 18, has ten spawn points at least a
// throw's reach (10 units) apart with a wall across the line between every
// pair of neighbours (closer than 12.5 units), and is symmetric so that no
// spawn is favoured; archipelago and compound carry two spawn groups for the
// team modes.
#include "kfm/content/loader.hpp"
#include "kfm/sim/content.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

using namespace kfm;
using content::Diagnostics;
using content::loadContentDir;
using sim::ContentDB;

namespace {

const ContentDB& shipped() {
  static const ContentDB db = [] {
    ContentDB c{};
    Diagnostics diag;
    REQUIRE(loadContentDir(KFM_CONTENT_DIR, c, diag));
    return c;
  }();
  return db;
}

struct BigArena {
  std::string_view name;
  uint8_t index;
  int32_t halfX2;  // half extents, doubled (whole units)
  int32_t halfY2;
  uint8_t groups;
};
constexpr std::array<BigArena, 4> kBig{{
    {"meadow", 15, 44, 25, 1},
    {"archipelago", 16, 52, 29, 2},
    {"compound", 17, 48, 27, 2},
    {"marsh", 18, 60, 34, 1},
}};

const sim::ArenaDef& arenaNamed(std::string_view name) {
  const std::optional<uint8_t> i = shipped().findArena(name);
  REQUIRE(i);
  return shipped().arenas[i.value_or(0)];
}

[[nodiscard]] Fixed halfOf(int32_t doubled) {
  return Fixed::fromRaw(doubled * (Fixed::one().raw / 2));
}

// Whether the segment p-q crosses the closed box: sampled at 1/256 of its
// length, finer than any wall here (the thinnest is 1 unit, the longest
// line checked 12.5).
[[nodiscard]] bool segmentHitsBox(Vec2 p, Vec2 q, const sim::Wall& w) {
  const int64_t dx = static_cast<int64_t>(q.x.raw) - p.x.raw;
  const int64_t dy = static_cast<int64_t>(q.y.raw) - p.y.raw;
  for (int64_t k = 0; k <= 256; ++k) {
    const Vec2 at{Fixed::fromRaw(static_cast<int32_t>(p.x.raw + (dx * k) / 256)),
                  Fixed::fromRaw(static_cast<int32_t>(p.y.raw + (dy * k) / 256))};
    if (sim::boxContains(w.min, w.max, at)) return true;
  }
  return false;
}

[[nodiscard]] Vec2 mirrorX(Vec2 v) {
  return Vec2{-v.x, v.y};
}
[[nodiscard]] Vec2 mirrorY(Vec2 v) {
  return Vec2{v.x, -v.y};
}
[[nodiscard]] Vec2 turned(Vec2 v) {
  return Vec2{-v.x, -v.y};
}

// Whether some box of the list is `b` under the point map f (min/max
// re-sorted).
template <class Box, class F>
[[nodiscard]] bool hasTwin(const Box* boxes, uint8_t count, const Box& b, F f) {
  const Vec2 a = f(b.min);
  const Vec2 c = f(b.max);
  const Vec2 lo{std::min(a.x, c.x), std::min(a.y, c.y)};
  const Vec2 hi{std::max(a.x, c.x), std::max(a.y, c.y)};
  for (uint8_t i = 0; i < count; ++i) {
    if (boxes[i].min == lo && boxes[i].max == hi) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("the large arenas are appended, bigger than the dojo, with ten spread spawns (v0.91)",
          "[content][arena][big]") {
  const ContentDB& db = shipped();
  const BigArena& want = GENERATE(from_range(kBig));
  INFO(want.name);
  REQUIRE(db.findArena(want.name) == std::optional<uint8_t>{want.index});
  const sim::ArenaDef& a = db.arenas[want.index];
  const sim::ArenaDef& dojo = db.arenas[0];
  CHECK(a.halfExtents == Vec2{halfOf(want.halfX2), halfOf(want.halfY2)});
  CHECK(a.halfExtents.x > dojo.halfExtents.x + Fixed::fromInt(5));
  CHECK(a.halfExtents.y > dojo.halfExtents.y + Fixed::fromInt(3));
  CHECK(a.spawnGroups == want.groups);
  CHECK(a.cellSize == Fixed::zero());  // continuous arenas: no grid mode plays them
  REQUIRE(a.spawnPointCount == sim::kMaxPlayers);

  const uint64_t reach = lengthSqRaw(Vec2{Fixed::fromInt(10), Fixed::zero()});
  const uint64_t neighbour = lengthSqRaw(Vec2{Fixed::fromRatio(25, 2), Fixed::zero()});
  for (uint8_t i = 0; i < a.spawnPointCount; ++i) {
    for (uint8_t j = static_cast<uint8_t>(i + 1); j < a.spawnPointCount; ++j) {
      INFO("spawns " << int{i} << " and " << int{j});
      const uint64_t d = lengthSqRaw(a.spawnPoints[i] - a.spawnPoints[j]);
      CHECK(d >= reach);
      if (d >= neighbour) continue;
      bool covered = false;
      for (uint8_t w = 0; w < a.wallCount; ++w)
        covered = covered || segmentHitsBox(a.spawnPoints[i], a.spawnPoints[j], a.walls[w]);
      CHECK(covered);  // no throw on the first tick reaches a neighbour
    }
  }
}

TEST_CASE("meadow and marsh are symmetric in both axes (v0.91)", "[content][arena][big]") {
  for (const std::string_view name : {"meadow", "marsh"}) {
    INFO(name);
    const sim::ArenaDef& a = arenaNamed(name);
    for (uint8_t i = 0; i < a.wallCount; ++i) {
      INFO("wall " << int{i});
      CHECK(hasTwin(a.walls.data(), a.wallCount, a.walls[i], mirrorX));
      CHECK(hasTwin(a.walls.data(), a.wallCount, a.walls[i], mirrorY));
    }
    for (uint8_t i = 0; i < a.hazardCount; ++i) {
      INFO("hazard " << int{i});
      CHECK(hasTwin(a.hazards.data(), a.hazardCount, a.hazards[i], mirrorX));
      CHECK(hasTwin(a.hazards.data(), a.hazardCount, a.hazards[i], mirrorY));
    }
    for (uint8_t i = 0; i < a.spawnPointCount; ++i) {
      bool x = false;
      bool y = false;
      for (uint8_t j = 0; j < a.spawnPointCount; ++j) {
        x = x || a.spawnPoints[j] == mirrorX(a.spawnPoints[i]);
        y = y || a.spawnPoints[j] == mirrorY(a.spawnPoints[i]);
      }
      CHECK(x);
      CHECK(y);
    }
    for (uint8_t i = 0; i < a.pickupSpawnerCount; ++i) {
      bool x = false;
      for (uint8_t j = 0; j < a.pickupSpawnerCount; ++j)
        x = x || a.pickupSpawners[j].pos == mirrorX(a.pickupSpawners[i].pos) ||
            a.pickupSpawners[j].pos == mirrorY(a.pickupSpawners[i].pos);
      CHECK(x);
    }
  }
  CHECK(arenaNamed("marsh").pickupSpawnerCount == 4);  // power-ups on the open ground
}

TEST_CASE("archipelago and compound: each team's side the other turned half a turn (v0.91)",
          "[content][arena][big]") {
  const ContentDB& db = shipped();
  for (const std::string_view name : {"archipelago", "compound"}) {
    INFO(name);
    const sim::ArenaDef& a = arenaNamed(name);
    for (uint8_t i = 0; i < a.wallCount; ++i) {
      INFO("wall " << int{i});
      CHECK(hasTwin(a.walls.data(), a.wallCount, a.walls[i], turned));
    }
    for (uint8_t i = 0; i < a.hazardCount; ++i) {
      INFO("hazard " << int{i});
      CHECK(hasTwin(a.hazards.data(), a.hazardCount, a.hazards[i], turned));
    }
    REQUIRE(a.spawnGroups == 2);
    const sim::SpawnRange west = sim::spawnGroupRange(a, 0);
    const sim::SpawnRange east = sim::spawnGroupRange(a, 1);
    REQUIRE(west.end - west.begin == 5);
    for (uint8_t k = 0; k < 5; ++k) {
      INFO("spawn " << int{k} << " of each group");
      CHECK(turned(a.spawnPoints[west.begin + k]) == a.spawnPoints[east.begin + k]);
      CHECK(a.spawnPoints[west.begin + k].x <= Fixed::zero());  // group 0 holds the west
    }
    const uint8_t index = db.findArena(name).value_or(0);
    for (const std::string_view mode :
         {"rounds", "deathmatch", "kill_rounds", "team_rounds", "team_deathmatch"}) {
      INFO(mode);
      CHECK(db.compatible(index, db.findMode(mode).value_or(0)));
    }
  }
  // Archipelago's centre hub is its hill.
  const sim::ArenaDef& arch = arenaNamed("archipelago");
  REQUIRE(arch.zoneCount == 1);
  CHECK(arch.zones[0].min == turned(arch.zones[0].max));
  CHECK(db.compatible(db.findArena("archipelago").value_or(0), db.findMode("hill").value_or(0)));
  CHECK(arch.hazardCount == 14);  // the channels: fourteen pit boxes of the sixteen
}

TEST_CASE("meadow and marsh play the free-for-all modes, not the team ones (v0.91)",
          "[content][arena][big]") {
  const ContentDB& db = shipped();
  for (const std::string_view name : {"meadow", "marsh"}) {
    INFO(name);
    const uint8_t index = db.findArena(name).value_or(0);
    for (const std::string_view mode : {"rounds", "deathmatch", "survival", "kill_rounds"})
      CHECK(db.compatible(index, db.findMode(mode).value_or(0)));
    CHECK_FALSE(db.compatible(index, db.findMode("team_rounds").value_or(0)));
  }
}
