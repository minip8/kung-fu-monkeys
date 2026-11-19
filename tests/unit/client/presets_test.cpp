// Game presets (architecture §7, v0.97): every preset resolves against the
// shipped content to a compatible arena and mode, the table covers every
// mode a human lobby offers, and the active preset is derived from the
// settings (Custom when nothing matches).
#include "core/presets.hpp"

#include "core/content_labels.hpp"
#include "core/lobby_settings.hpp"
#include "kfm/content/loader.hpp"
#include "kfm/sim/content.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>

using namespace kfm;
using client::GamePreset;
using client::LobbySettings;
using client::ResolvedPreset;

namespace {

const sim::ContentDB& shipped() {
  static const sim::ContentDB db = [] {
    sim::ContentDB d{};
    content::Diagnostics diag;
    if (!content::loadEmbeddedContent(d, diag)) d = sim::defaultContent();
    return d;
  }();
  return db;
}

[[nodiscard]] std::size_t presetIndex(std::string_view id) {
  const client::PresetList list = client::resolvePresets(shipped());
  for (std::size_t i = 0; i < list.count; ++i) {
    if (list.items[i].def->id == id) return i;
  }
  FAIL("no such preset");  // MSVC: no unreachable return after FAIL
  return list.count;
}

}  // namespace

TEST_CASE("every preset resolves against the shipped content to a compatible pair",
          "[client][presets]") {
  const sim::ContentDB& db = shipped();
  REQUIRE(db.modeCount > 1);
  std::set<std::string_view> ids;
  std::set<std::string_view> titles;
  for (const GamePreset& p : client::gamePresets()) {
    INFO(p.id);
    std::string error;
    const std::optional<ResolvedPreset> r = client::resolvePreset(db, p, error);
    REQUIRE(r.has_value());
    CHECK(error.empty());
    if (!r) continue;
    CHECK(db.compatible(r->arena, r->mode));
    CHECK_FALSE(client::isBotMode(db.modeAt(r->mode)));
    CHECK_FALSE(client::isRoleLoadout(db, r->loadout));
    // A mode with its own arena (grid_rounds' courtyard) is played there.
    const sim::ModeDef& mode = db.modeAt(r->mode);
    if (mode.hasArena()) CHECK(r->arena == mode.arena);
    CHECK(p.players >= 2);
    CHECK(p.players <= sim::kMaxPlayers);
    if (mode.teams > 0) CHECK(p.players >= 2 * mode.teams);
    CHECK_FALSE(p.title.empty());
    CHECK_FALSE(p.blurb.empty());
    CHECK(ids.insert(p.id).second);
    CHECK(titles.insert(p.title).second);
    // A preset is a whole match: it is its own active preset, and no
    // earlier one shadows it.
    LobbySettings s;
    (void)client::applyPreset(*r, 1, sim::kMaxPlayers - 1, s);
    const client::PresetList list = client::resolvePresets(db);
    const std::optional<std::size_t> found = client::findPreset(db, list.view(), s);
    REQUIRE(found.has_value());
    CHECK(list.items[found.value_or(0)].def->id == p.id);
  }
  CHECK(client::resolvePresets(db).count == client::gamePresets().size());
}

TEST_CASE("the presets cover every mode a human lobby offers", "[client][presets]") {
  const sim::ContentDB& db = shipped();
  const client::PresetList list = client::resolvePresets(db);
  std::set<uint8_t> covered;
  for (const ResolvedPreset& p : list.view())
    covered.insert(p.mode);
  for (uint8_t m = 0; m < db.modeCount; ++m) {
    INFO(db.modes[m].name.view());
    if (client::isBotMode(db.modes[m])) continue;
    CHECK(covered.contains(m));
  }
  // The first-run default is the first, and it has power-ups to show off.
  REQUIRE(list.count > 0);
  CHECK(list.items[0].def->id == client::kDefaultPreset);
  CHECK(client::arenaPowerUps(db, db.arenaAt(list.items[0].arena)) != 0);
}

TEST_CASE("a preset that the content cannot play is left out", "[client][presets]") {
  // The default fixture: one arena (the dojo), one mode (rounds).
  const sim::ContentDB& fixture = sim::defaultContent();
  const client::PresetList list = client::resolvePresets(fixture);
  for (const ResolvedPreset& p : list.view()) {
    CHECK(fixture.compatible(p.arena, p.mode));
  }
  std::string error;
  const GamePreset unknown{.id = "x",
                           .title = "X",
                           .blurb = "x",
                           .mode = "rounds",
                           .arena = "lava",
                           .loadout = "base",
                           .powerUps = "all"};
  CHECK_FALSE(client::resolvePreset(shipped(), unknown, error).has_value());
  CHECK(error.find("lava") != std::string::npos);
  const GamePreset unsuited{.id = "y",
                            .title = "Y",
                            .blurb = "y",
                            .mode = "team_rounds",
                            .arena = "dojo",
                            .loadout = "base",
                            .powerUps = "all"};
  CHECK_FALSE(client::resolvePreset(shipped(), unsuited, error).has_value());
  CHECK(error.find("does not suit") != std::string::npos);
}

TEST_CASE("the active preset is derived from the settings", "[client][presets]") {
  const sim::ContentDB& db = shipped();
  const client::PresetList list = client::resolvePresets(db);
  const ResolvedPreset& classic = list.items[presetIndex("classic")];
  LobbySettings s;
  (void)client::applyPreset(classic, 1, sim::kMaxPlayers - 1, s);
  REQUIRE(client::presetMatches(db, classic, s));

  SECTION("bots and their difficulty never make a preset Custom") {
    s.bots = 7;
    s.botTier = ai::ReferenceTier::Easy;
    CHECK(client::presetMatches(db, classic, s));
  }
  SECTION("a changed kit, arena or mode does") {
    LobbySettings kit = s;
    kit.loadout = db.findLoadout("slam").value_or(0);
    CHECK_FALSE(client::presetMatches(db, classic, kit));
    LobbySettings arena = s;
    arena.arena = db.findArena("pillars").value_or(0);
    CHECK_FALSE(client::presetMatches(db, classic, arena));
    CHECK_FALSE(client::findPreset(db, list.view(), arena).has_value());
  }
  SECTION("power-ups count only where the arena's spawners can draw them") {
    LobbySettings off = s;
    off.powerUps = 0;
    CHECK_FALSE(client::presetMatches(db, classic, off));  // the dojo with power-ups draws them
    // Bits the arena never draws do not matter.
    LobbySettings extra = s;
    const auto drawn = client::arenaPowerUps(db, db.arenaAt(classic.arena));
    extra.powerUps = drawn;
    CHECK(client::presetMatches(db, classic, extra));
    // On an arena without spawners any set is the preset's.
    const ResolvedPreset& brawl = list.items[presetIndex("brawl")];
    REQUIRE(client::arenaPowerUps(db, db.arenaAt(brawl.arena)) == 0);
    LobbySettings b;
    (void)client::applyPreset(brawl, 1, sim::kMaxPlayers - 1, b);
    b.powerUps = 0;
    CHECK(client::presetMatches(db, brawl, b));
  }
  SECTION("a mode that hands kits out by role ignores the kit") {
    const ResolvedPreset& grid = list.items[presetIndex("grid")];
    REQUIRE(db.modeAt(grid.mode).roleCount > 0);
    LobbySettings g;
    (void)client::applyPreset(grid, 1, sim::kMaxPlayers - 1, g);
    g.loadout = db.findLoadout("blink").value_or(0);
    CHECK(client::presetMatches(db, grid, g));
  }
}

TEST_CASE("a preset raises the bots to its players, never lowers them", "[client][presets]") {
  const sim::ContentDB& db = shipped();
  const client::PresetList list = client::resolvePresets(db);
  const ResolvedPreset& classic = list.items[presetIndex("classic")];  // 4 monkeys
  REQUIRE(classic.def->players == 4);
  LobbySettings s;
  CHECK(client::applyPreset(classic, 1, 9, s));
  CHECK(s.bots == 3);
  CHECK_FALSE(client::applyPreset(classic, 2, 9, s));  // 2 humans + 3 bots: enough
  CHECK(s.bots == 3);
  LobbySettings few;
  CHECK(client::applyPreset(classic, 1, 2, few));  // at most the room the cap leaves
  CHECK(few.bots == 2);
  LobbySettings crowd;
  CHECK_FALSE(client::applyPreset(classic, 6, 9, crowd));
  CHECK(crowd.bots == 0);
}
