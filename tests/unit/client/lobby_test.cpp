// The local lobby's settings panel (architecture §7, v0.24): the LobbyMenu
// state machine, the generic menu input, the settings persistence and the
// flag / URL parsing, headless over the shipped (embedded) content.
#include "core/client_options.hpp"
#include "core/content_labels.hpp"
#include "core/lobby_menu.hpp"
#include "core/lobby_settings.hpp"
#include "core/match_session.hpp"
#include "kfm/content/loader.hpp"
#include "kfm/net/messages.hpp"
#include "kfm/platform/input_event.hpp"
#include "kfm/presentation/tuning.hpp"
#include "kfm/sim/content.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

using namespace kfm;
using client::ClientOptions;
using client::LobbyMenu;
using client::LobbyRowKind;
using client::LobbySettings;
using client::MenuAction;
using client::MenuInput;
using platform::InputEvent;
using platform::InputEventKind;
using platform::Key;
using platform::PadAxis;
using platform::PadButton;

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

[[nodiscard]] uint8_t arenaIndex(std::string_view name) {
  return shipped().findArena(name).value_or(0xFF);
}

[[nodiscard]] std::size_t rowOf(LobbyRowKind kind) {
  const auto rows = LobbyMenu::rows();
  return static_cast<std::size_t>(std::ranges::find(rows, kind) - rows.begin());
}

[[nodiscard]] InputEvent key(Key k) {
  InputEvent e;
  e.kind = InputEventKind::KeyDown;
  e.key = k;
  return e;
}

[[nodiscard]] InputEvent padButton(uint32_t pad, PadButton b) {
  InputEvent e;
  e.kind = InputEventKind::PadButtonDown;
  e.deviceId = pad;
  e.button = b;
  return e;
}

[[nodiscard]] InputEvent stick(uint32_t pad, PadAxis axis, float v) {
  InputEvent e;
  e.kind = InputEventKind::PadAxis;
  e.deviceId = pad;
  e.axis = axis;
  e.value = v;
  return e;
}

[[nodiscard]] bool parse(std::vector<const char*> argv, ClientOptions& o, std::string& error) {
  argv.insert(argv.begin(), "kfm_client");
  return client::parseClientOptions(std::span<const char* const>(argv.data(), argv.size()), o,
                                    error);
}

}  // namespace

TEST_CASE("the setup panel has the same seven rows whatever the values (v0.97)",
          "[client][lobby]") {
  const sim::ContentDB& db = shipped();
  LobbyMenu m;
  m.setContent(db);
  const std::array<LobbyRowKind, 7> expected{
      LobbyRowKind::Game,     LobbyRowKind::Mode, LobbyRowKind::Arena,        LobbyRowKind::Loadout,
      LobbyRowKind::PowerUps, LobbyRowKind::Bots, LobbyRowKind::BotDifficulty};
  const auto same = [&] {
    const auto rows = LobbyMenu::rows();
    REQUIRE(rows.size() == expected.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
      CHECK(rows[i] == expected[i]);
  };
  same();
  CHECK(std::string(LobbyMenu::rowLabel(LobbyRowKind::Game)) == "Game");
  CHECK(std::string(LobbyMenu::rowLabel(LobbyRowKind::Loadout)) == "Kit");
  CHECK(std::string(LobbyMenu::rowLabel(LobbyRowKind::PowerUps)) == "Power-ups");
  // Every arena, every mode: the rows stay; only whether they apply changes.
  for (uint8_t mode = 0; mode < db.modeCount; ++mode) {
    for (uint8_t arena = 0; arena < db.arenaCount; ++arena) {
      m.settings().mode = mode;
      m.settings().arena = arena;
      same();
      const bool spawners = client::arenaPowerUps(db, db.arenas[arena]) != 0;
      CHECK(m.rowEnabled(LobbyRowKind::PowerUps) == spawners);
      CHECK(m.rowEnabled(LobbyRowKind::Loadout) == (db.modes[mode].roleCount == 0));
      CHECK(m.rowEnabled(LobbyRowKind::Game));
      CHECK(m.rowEnabled(LobbyRowKind::Mode));
      CHECK(m.rowEnabled(LobbyRowKind::Arena));
      CHECK(m.rowEnabled(LobbyRowKind::Bots));
      CHECK(m.rowEnabled(LobbyRowKind::BotDifficulty));
    }
  }
  // The room's panel is the same rows: Bot difficulty greyed (the server's).
  m.setRoomRows(true);
  same();
  CHECK_FALSE(m.rowEnabled(LobbyRowKind::BotDifficulty));
  CHECK(m.rowValue(LobbyRowKind::BotDifficulty) == "Server's choice");
  CHECK_FALSE(m.rowReason(LobbyRowKind::BotDifficulty).empty());
  m.setRoomRows(false);
  CHECK(m.rowEnabled(LobbyRowKind::BotDifficulty));
  CHECK(m.rowReason(LobbyRowKind::BotDifficulty).empty());
  // A menu that never saw content has them too.
  const LobbyMenu bare;
  CHECK(bare.rowEnabled(LobbyRowKind::Game));
}

TEST_CASE("a row that does not apply stays, greyed, says why and is skipped (v0.97)",
          "[client][lobby]") {
  const sim::ContentDB& db = shipped();
  LobbyMenu m;
  m.setContent(db);
  SECTION("an arena without power-up spawners greys Power-ups") {
    m.settings().arena = arenaIndex("dojo");
    REQUIRE(client::arenaPowerUps(db, db.arenaAt(m.settings().arena)) == 0);
    CHECK_FALSE(m.rowEnabled(LobbyRowKind::PowerUps));
    CHECK(m.rowValue(LobbyRowKind::PowerUps) == "None in this arena");
    CHECK(m.rowReason(LobbyRowKind::PowerUps).find("no power-up spawners") != std::string::npos);
    // Down from Kit steps over it to Bots; Up from Bots back to Kit.
    m.setFocus(rowOf(LobbyRowKind::Loadout));
    CHECK_FALSE(m.apply(MenuAction::Down));
    CHECK(m.focusedRow() == LobbyRowKind::Bots);
    CHECK_FALSE(m.apply(MenuAction::Up));
    CHECK(m.focusedRow() == LobbyRowKind::Loadout);
    // Left/Right on it (a click on its flat arrows) do nothing, nor the picker.
    const LobbySettings before = m.settings();
    CHECK_FALSE(m.change(rowOf(LobbyRowKind::PowerUps), +1));
    m.openPicker();
    CHECK_FALSE(m.pickerOpen());
    CHECK(m.settings() == before);
  }
  SECTION("a mode that hands kits out by role greys Kit") {
    m.settings().mode = db.findMode("grid_rounds").value_or(0);
    m.settings().arena = arenaIndex("courtyard");
    CHECK_FALSE(m.rowEnabled(LobbyRowKind::Loadout));
    CHECK(m.rowValue(LobbyRowKind::Loadout) == "By role");
    CHECK(m.rowReason(LobbyRowKind::Loadout).find("Grid Rounds") != std::string::npos);
    CHECK_FALSE(m.change(rowOf(LobbyRowKind::Loadout), +1));
    m.setFocus(rowOf(LobbyRowKind::Arena));
    CHECK_FALSE(m.apply(MenuAction::Down));
    CHECK(m.focusedRow() == LobbyRowKind::PowerUps);  // the courtyard drops Quick Hands
  }
  SECTION("Up from Game wraps to the last row that applies") {
    m.setFocus(0);
    CHECK_FALSE(m.apply(MenuAction::Up));
    CHECK(m.focusedRow() == LobbyRowKind::BotDifficulty);
    m.setRoomRows(true);
    m.setFocus(0);
    CHECK_FALSE(m.apply(MenuAction::Up));
    CHECK(m.focusedRow() == LobbyRowKind::Bots);
    CHECK_FALSE(m.apply(MenuAction::Down));
    CHECK(m.focusedRow() == LobbyRowKind::Game);
  }
}

TEST_CASE("the Game row picks presets, and says Custom once edited (v0.97)",
          "[client][lobby][presets]") {
  const sim::ContentDB& db = shipped();
  LobbyMenu m;
  m.setContent(db);
  m.setHumans(1);
  const auto presets = m.presets();
  REQUIRE(presets.size() >= 3);
  const std::size_t game = rowOf(LobbyRowKind::Game);
  // The defaults (rounds on the dojo) are no preset.
  CHECK_FALSE(m.activePreset().has_value());
  CHECK(m.rowValue(LobbyRowKind::Game) == "Custom");

  // Right from Custom starts the list (Left would end it); each step
  // applies a whole preset.
  LobbyMenu left = m;
  CHECK(left.change(game, -1));
  CHECK(left.activePreset().value_or(99) == presets.size() - 1);
  CHECK(m.change(game, +1));
  REQUIRE(m.activePreset().has_value());
  CHECK(m.activePreset().value_or(99) == 0);
  for (std::size_t i = 1; i < presets.size() + 1; ++i) {
    (void)m.change(game, +1);
    const std::size_t at = i % presets.size();
    CHECK(m.activePreset().value_or(99) == at);
    CHECK(m.rowValue(LobbyRowKind::Game) == presets[at].def->title);
    CHECK(m.settings().mode == presets[at].mode);
    CHECK(m.settings().arena == presets[at].arena);
    CHECK(db.compatible(m.settings().arena, m.settings().mode));
  }
  // Classic (the first) is for 4: the bots were raised to 3 beside 1 human.
  CHECK(m.applyPreset(0) == false);  // already it
  m.settings().bots = 0;
  CHECK(m.applyPreset(0));
  CHECK(m.settings().bots == 3);
  CHECK(m.notice() == "Added 3 bots: Classic is for 4 monkeys.");

  // A changed kit makes it Custom; Left goes back to Classic, Right on.
  CHECK(m.change(rowOf(LobbyRowKind::Loadout), +1));
  CHECK(m.notice().empty());
  CHECK(m.rowValue(LobbyRowKind::Game) == "Custom");
  LobbyMenu copy = m;
  CHECK(copy.change(game, -1));
  CHECK(copy.activePreset().value_or(99) == 0);
  CHECK(m.change(game, +1));
  CHECK(m.activePreset().value_or(99) == 1);
  // Bots and their difficulty never make a preset Custom.
  CHECK(m.change(rowOf(LobbyRowKind::Bots), +1));
  CHECK(m.change(rowOf(LobbyRowKind::BotDifficulty), +1));
  CHECK(m.activePreset().value_or(99) == 1);
  // Picking a preset never lowers the bots.
  m.settings().bots = 8;
  (void)m.applyPreset(0);
  CHECK(m.settings().bots == 8);
}

TEST_CASE("a mode change that moves the arena says so (v0.97)", "[client][lobby][teams]") {
  const sim::ContentDB& db = shipped();
  const uint8_t teamRounds = db.findMode("team_rounds").value_or(0);
  const uint8_t standoff = arenaIndex("standoff");
  REQUIRE(teamRounds != 0);
  LobbyMenu m;
  m.setContent(db);
  const std::size_t modeRow = rowOf(LobbyRowKind::Mode);
  // Cycling the mode into team_rounds moves the arena off the dojo.
  while (m.settings().mode != teamRounds)
    (void)m.change(modeRow, +1);
  CHECK(m.settings().arena == standoff);
  CHECK(m.notice() == "Arena set to Standoff: Team Rounds needs a starting side for each team.");
  // The arena row only offers arenas that suit the mode, and says nothing.
  for (int i = 0; i < 2 * db.arenaCount; ++i) {
    (void)m.change(rowOf(LobbyRowKind::Arena), +1);
    CHECK(db.compatible(m.settings().arena, m.settings().mode));
    CHECK(m.notice().empty());
  }
  // A mode with its own arena brings it, and says so.
  const uint8_t grid = db.findMode("grid_rounds").value_or(0);
  while (m.settings().mode != grid)
    (void)m.change(modeRow, +1);
  CHECK(m.settings().arena == arenaIndex("courtyard"));
  CHECK(m.notice().find("Arena set to Courtyard") == 0);
  // A mode the arena already suits keeps it, without a word.
  m.settings().mode = 0;
  m.settings().arena = arenaIndex("pillars");
  (void)m.change(modeRow, +1);
  CHECK(m.settings().arena == arenaIndex("pillars"));
  CHECK(m.notice().empty());
  // Content that loads with an unsuitable pair is clamped too.
  LobbyMenu fresh;
  fresh.settings().mode = teamRounds;
  fresh.setContent(db);
  CHECK(fresh.settings().arena == standoff);
  // A saved arena gives way to the mode named by a flag.
  ClientOptions o;
  o.arena = "dojo";
  o.mode = "team_rounds";
  LobbySettings ls;
  std::string error;
  REQUIRE(lobbySettingsFrom(o, db, ls, error));
  CHECK(ls.arena == standoff);
  o.arena = "lava";
  CHECK_FALSE(lobbySettingsFrom(o, db, ls, error));
  CHECK_FALSE(client::arenaNeed(db.modes[teamRounds]).empty());
  CHECK(client::arenaNeed(db.modes[0]).empty());  // rounds: any arena
}

TEST_CASE("lobby menu: a grid mode opens on its own arena and keeps arenas with cells (v0.34)",
          "[client][lobby][grid]") {
  const sim::ContentDB& db = shipped();
  const uint8_t grid = db.findMode("grid_rounds").value_or(0);
  const uint8_t courtyard = db.findArena("courtyard").value_or(0);
  REQUIRE(grid != 0);
  REQUIRE(courtyard != 0);
  LobbyMenu m;
  m.setContent(db);
  const std::size_t modeRow = rowOf(LobbyRowKind::Mode);
  while (m.settings().mode != grid)
    (void)m.change(modeRow, +1);
  CHECK(m.settings().arena == courtyard);  // ModeDef::arena, not the first tilemap
  for (int i = 0; i < 2 * db.arenaCount; ++i) {
    (void)m.change(rowOf(LobbyRowKind::Arena), +1);
    CHECK(db.compatible(m.settings().arena, m.settings().mode));
    CHECK(db.arenas[m.settings().arena].cellSize > Fixed::zero());
  }
  ClientOptions o;
  o.mode = "grid_rounds";
  LobbySettings ls;
  std::string error;
  REQUIRE(lobbySettingsFrom(o, db, ls, error));
  CHECK(ls.arena == courtyard);
  o.arena = "dojo";  // a saved or named arena without cells gives way
  REQUIRE(lobbySettingsFrom(o, db, ls, error));
  CHECK(ls.arena == courtyard);
}

TEST_CASE("lobby menu: focus wraps, values cycle and clamp", "[client][lobby]") {
  const sim::ContentDB& db = shipped();
  LobbyMenu m;
  m.setContent(db);
  m.setFocus(rowOf(LobbyRowKind::Arena));

  SECTION("arena cycles both ways with wrap-around") {
    CHECK(m.rowValue(LobbyRowKind::Arena) == "Dojo");  // v0.94: the arena's title
    CHECK(m.apply(MenuAction::Right));
    CHECK(m.settings().arena == 1);
    CHECK(m.rowValue(LobbyRowKind::Arena) == client::arenaTitle(db.arenas[1].name.view()));
    CHECK(m.apply(MenuAction::Left));
    CHECK(m.apply(MenuAction::Left));
    CHECK(m.settings().arena == db.arenaCount - 1);
    CHECK(m.apply(MenuAction::Right));
    CHECK(m.settings().arena == 0);
  }
  SECTION("mode cycles over the modes for people (v0.94: not the bot contests)") {
    m.setFocus(rowOf(LobbyRowKind::Mode));
    std::vector<uint8_t> human;
    for (uint8_t i = 0; i < db.modeCount; ++i) {
      if (!client::isBotMode(db.modes[i])) human.push_back(i);
    }
    REQUIRE(human.size() < db.modeCount);  // the shipped content has bot contests
    for (std::size_t i = 1; i <= human.size(); ++i) {
      CHECK(m.apply(MenuAction::Right));
      CHECK(m.settings().mode == human[i % human.size()]);
      CHECK_FALSE(client::isBotMode(db.modeAt(m.settings().mode)));
    }
  }
  SECTION("kit cycles over the kits every monkey can play (v0.97: not the role kits)") {
    REQUIRE(db.loadoutCount >= 2);
    m.setFocus(rowOf(LobbyRowKind::Loadout));
    std::vector<uint8_t> kits;
    for (uint8_t i = 0; i < db.loadoutCount; ++i) {
      if (!client::isRoleLoadout(db, i)) kits.push_back(i);
    }
    REQUIRE(kits.size() == 4u);  // base, blink, slam, parry
    for (std::size_t i = 1; i <= kits.size(); ++i) {
      CHECK(m.apply(MenuAction::Right));
      CHECK(m.settings().loadout == kits[i % kits.size()]);
      CHECK(m.rowValue(LobbyRowKind::Loadout) ==
            client::loadoutTitle(db.loadoutAt(m.settings().loadout).name.view()));
    }
    CHECK(m.apply(MenuAction::Left));
    CHECK(m.settings().loadout == kits.back());
  }
  SECTION("power-ups cycle All, None and Custom, which opens the picker (v0.97)") {
    m.settings().arena = arenaIndex("dojo_powerups");
    const std::size_t row = rowOf(LobbyRowKind::PowerUps);
    REQUIRE(m.rowEnabled(LobbyRowKind::PowerUps));
    CHECK(m.powerUpChoice() == client::PowerUpChoice::All);
    CHECK(m.rowValue(LobbyRowKind::PowerUps) == "All");
    CHECK(m.change(row, +1));
    CHECK(m.settings().powerUps == 0);
    CHECK(m.rowValue(LobbyRowKind::PowerUps) == "None");
    CHECK_FALSE(m.pickerOpen());
    CHECK_FALSE(m.change(row, +1));  // Custom: nothing changes yet
    CHECK(m.pickerOpen());
    m.picker().close();
    CHECK(m.change(row, -1));  // from None, Left is All
    CHECK(m.settings().powerUps == sim::kAllPowerUps);
    CHECK_FALSE(m.change(row, -1));  // from All, Left is Custom
    CHECK(m.pickerOpen());
    // Two of the five off: "3 of 5", and Right goes on to All.
    m.picker().close();
    m.settings().powerUps = static_cast<uint16_t>(sim::kAllPowerUps & ~0b11u);
    CHECK(m.powerUpChoice() == client::PowerUpChoice::Custom);
    CHECK(m.rowValue(LobbyRowKind::PowerUps) == "3 of 5");
    CHECK(m.change(row, +1));
    CHECK(m.powerUpChoice() == client::PowerUpChoice::All);
  }
  SECTION("bots step within 0..9 and clamp") {
    const std::size_t bots = rowOf(LobbyRowKind::Bots);
    CHECK_FALSE(m.change(bots, -1));  // already 0: nothing changes
    for (int i = 0; i < 12; ++i)
      (void)m.change(bots, +1);
    CHECK(m.settings().bots == sim::kMaxPlayers - 1);
    CHECK_FALSE(m.change(bots, +1));
    CHECK(m.rowValue(LobbyRowKind::Bots) == "9");
  }
  SECTION("bot difficulty cycles easy, medium, hard with wrap-around") {
    m.setFocus(rowOf(LobbyRowKind::BotDifficulty));
    CHECK(m.settings().botTier == ai::ReferenceTier::Medium);  // the casual default
    CHECK(m.rowValue(LobbyRowKind::BotDifficulty) == "medium");
    CHECK(m.apply(MenuAction::Right));
    CHECK(m.settings().botTier == ai::ReferenceTier::Hard);
    CHECK(m.apply(MenuAction::Right));
    CHECK(m.settings().botTier == ai::ReferenceTier::Easy);
    CHECK(m.apply(MenuAction::Left));
    CHECK(m.apply(MenuAction::Left));
    CHECK(m.settings().botTier == ai::ReferenceTier::Medium);
  }
  SECTION("out-of-range rows and directions do nothing") {
    CHECK_FALSE(m.change(LobbyMenu::rows().size(), +1));
    CHECK_FALSE(m.change(0, 0));
    m.setFocus(LobbyMenu::rows().size() + 3);
    CHECK(m.focusedRow() == LobbyRowKind::Arena);
  }
}

TEST_CASE("the power-up picker: the arena's power-ups in a fixed grid (v0.97)",
          "[client][lobby][picker]") {
  const sim::ContentDB& db = shipped();
  LobbyMenu m;
  m.setContent(db);
  m.settings().arena = arenaIndex("dojo_powerups");
  m.openPicker();
  REQUIRE(m.pickerOpen());
  client::PowerUpPicker& p = m.picker();
  using Kind = client::PowerUpPicker::ItemKind;
  const uint16_t drawn = client::arenaPowerUps(db, db.arenaAt(m.settings().arena));
  REQUIRE(p.toggleCount() == static_cast<std::size_t>(std::popcount(unsigned{drawn})));
  REQUIRE(p.toggleCount() == 5u);  // speed, big, triple, shield, fast hands
  const auto items = p.items();
  REQUIRE(items.size() == 8u);
  for (std::size_t i = 0; i < p.toggleCount(); ++i) {
    CHECK(items[i].kind == Kind::Toggle);
    CHECK((drawn & (1u << items[i].powerUp)) != 0);
    if (i > 0) CHECK(items[i].powerUp > items[i - 1].powerUp);  // content order
  }
  CHECK(items[5].kind == Kind::All);
  CHECK(items[6].kind == Kind::None);
  CHECK(items[7].kind == Kind::Done);
  // Rows of two toggles (the last one alone), then the buttons.
  CHECK(p.rowCount() == 4u);
  CHECK(p.rowOf(4) == 2u);
  CHECK(p.rowOf(6) == 3u);
  CHECK(p.columnOf(7) == 2u);

  SECTION("grid navigation keeps the column, wraps along rows and between them") {
    CHECK(p.focus() == 0);
    p.apply(MenuAction::Right);
    CHECK(p.focus() == 1);
    p.apply(MenuAction::Right);  // wraps along the row
    CHECK(p.focus() == 0);
    p.apply(MenuAction::Down);
    CHECK(p.focus() == 2);
    p.apply(MenuAction::Right);
    p.apply(MenuAction::Down);  // column 1 of a one-item row: its only item
    CHECK(p.focus() == 4);
    p.apply(MenuAction::Down);
    CHECK(p.focusedItem().kind == Kind::All);
    p.apply(MenuAction::Left);  // wraps to Done
    CHECK(p.focusedItem().kind == Kind::Done);
    p.apply(MenuAction::Up);  // Done is column 2: the last of the row above
    CHECK(p.focus() == 4);
    p.setFocus(0);
    p.apply(MenuAction::Up);  // from the top to the buttons
    CHECK(p.focusedItem().kind == Kind::All);
  }
  SECTION("toggles flip one, All and None the set, Done closes") {
    const uint8_t first = items[0].powerUp;
    CHECK(p.activate(0, m.settings()));
    CHECK((m.settings().powerUps & (1u << first)) == 0);
    CHECK(m.powerUpChoice() == client::PowerUpChoice::Custom);
    CHECK(m.rowValue(LobbyRowKind::PowerUps) == "4 of 5");
    CHECK(p.activate(6, m.settings()));  // None
    CHECK(m.settings().powerUps == 0);
    CHECK(p.activate(5, m.settings()));  // All
    CHECK(m.settings().powerUps == sim::kAllPowerUps);
    CHECK_FALSE(p.activate(7, m.settings()));  // Done
    CHECK_FALSE(m.pickerOpen());
    CHECK_FALSE(p.activate(0, m.settings()));  // closed: nothing
  }
  SECTION("new content closes it") {
    m.setContent(db);
    CHECK_FALSE(m.pickerOpen());
  }
}

TEST_CASE("lobby menu: new content clamps the settings", "[client][lobby]") {
  const sim::ContentDB& db = shipped();
  LobbyMenu m;
  m.setContent(db);
  m.settings().arena = static_cast<uint8_t>(db.arenaCount - 1);
  m.settings().mode = static_cast<uint8_t>(db.modeCount - 1);
  m.settings().loadout = static_cast<uint8_t>(db.loadoutCount - 1);
  m.setFocus(rowOf(LobbyRowKind::Bots));
  m.setContent(sim::defaultContent());  // one arena, one mode, one loadout
  CHECK(m.settings().arena == 0);
  CHECK(m.settings().mode == 0);
  CHECK(m.settings().loadout == 0);
  CHECK(m.focusedRow() == LobbyRowKind::Bots);
  m.setFocus(rowOf(LobbyRowKind::Arena));
  CHECK_FALSE(m.apply(MenuAction::Right));  // a single arena cycles onto itself
}

TEST_CASE("lobby menu: a mode with an arena pool brings its first member (v0.82)",
          "[client][lobby]") {
  // Hand-built: three copies of the dojo; mode 1 pools arenas 2 and 1.
  sim::ContentDB db = sim::defaultContent();
  for (uint8_t i = 1; i <= 2; ++i) {
    db.arenas[i] = db.arenas[0];
    db.arenas[i].name = sim::ContentName::of(i == 1 ? "pool_a" : "pool_b");
  }
  db.arenaCount = 3;
  db.modes[1] = db.modes[0];
  db.modes[1].name = sim::ContentName::of("pooled");
  db.modes[1].arenaSet = (1u << 1) | (1u << 2);
  db.modeCount = 2;
  LobbyMenu m;
  m.setContent(db);
  const std::size_t mode = rowOf(LobbyRowKind::Mode);
  REQUIRE(m.settings().arena == 0);
  CHECK(m.change(mode, +1));  // to the pooled mode: arena 0 is not a member
  CHECK(m.settings().mode == 1);
  CHECK(m.settings().arena == 1);  // the pool's first suitable member
  CHECK(m.notice() == "Arena set to Pool A, the first of Pooled's arenas.");
  CHECK(m.change(mode, +1));  // back: the rounds mode keeps the arena
  CHECK(m.settings().arena == 1);
  m.settings().arena = 2;
  CHECK(m.change(mode, +1));  // a member already: kept
  CHECK(m.settings().arena == 2);
}

// v8 (issue #83): the online room's Bots row stops where the server's
// validRoomSettings() does under its player cap (RoomStateMsg::maxPlayers):
// at most cap − 1 bots, so the host never asks for bots Start cannot place.
// Local play keeps the default cap, every slot (the case above).
TEST_CASE("lobby menu: the room's Bots row stops under the server's player cap",
          "[client][lobby][room]") {
  LobbyMenu m;
  m.setContent(shipped());
  m.setRoomRows(true);
  const std::size_t bots = rowOf(LobbyRowKind::Bots);
  const auto most = [&] {
    for (int i = 0; i < 12; ++i)
      (void)m.change(bots, +1);
    return m.settings().bots;
  };
  CHECK(m.maxBots() == sim::kMaxPlayers - 1);  // the default: no cap
  CHECK(most() == sim::kMaxPlayers - 1);

  // A cap below the bots asked for: the count drops to the limit.
  m.setPlayerCap(3);
  CHECK(m.maxBots() == 2);
  CHECK(m.settings().bots == 2);
  CHECK(m.rowValue(LobbyRowKind::Bots) == "2");
  CHECK_FALSE(m.change(bots, +1));  // already at the limit
  CHECK(m.change(bots, -1));
  CHECK(m.settings().bots == 1);

  m.setPlayerCap(4);
  CHECK(m.maxBots() == 3);
  CHECK(m.settings().bots == 1);  // under the limit: kept
  CHECK(most() == 3);

  m.setPlayerCap(sim::kMaxPlayers);
  CHECK(m.settings().bots == 3);
  CHECK(most() == sim::kMaxPlayers - 1);

  // One slot: no bot fits beside the human.
  m.setPlayerCap(1);
  CHECK(m.maxBots() == 0);
  CHECK(m.settings().bots == 0);
  CHECK_FALSE(m.change(bots, +1));

  // Out of range: clamped to 1..kMaxPlayers.
  m.setPlayerCap(0);
  CHECK(m.maxBots() == 0);
  m.setPlayerCap(static_cast<uint8_t>(sim::kMaxPlayers + 5));
  CHECK(m.maxBots() == sim::kMaxPlayers - 1);
}

TEST_CASE("menu input: keys, a bound pad's d-pad and stick, never an unbound pad",
          "[client][lobby][input]") {
  MenuInput in;
  CHECK(in.translate(key(Key::Up), true) == MenuAction::Up);
  CHECK(in.translate(key(Key::W), false) == MenuAction::Up);
  CHECK(in.translate(key(Key::S), true) == MenuAction::Down);
  CHECK(in.translate(key(Key::Left), true) == MenuAction::Left);
  CHECK(in.translate(key(Key::D), true) == MenuAction::Right);
  // Game buttons and Enter are not menu keys: joins and Start stay theirs.
  CHECK(in.translate(key(Key::J), true) == MenuAction::None);
  CHECK(in.translate(key(Key::Space), true) == MenuAction::None);
  CHECK(in.translate(key(Key::Enter), true) == MenuAction::None);

  constexpr uint32_t kPad = 7;
  CHECK(in.translate(padButton(kPad, PadButton::DpadDown), false) == MenuAction::None);
  CHECK(in.translate(padButton(kPad, PadButton::DpadDown), true) == MenuAction::Down);
  CHECK(in.translate(padButton(kPad, PadButton::DpadLeft), true) == MenuAction::Left);
  CHECK(in.translate(padButton(kPad, PadButton::South), true) == MenuAction::None);
  CHECK(in.translate(padButton(kPad, PadButton::Start), true) == MenuAction::None);

  // The stick fires once per push and re-arms below the release threshold.
  CHECK(in.translate(stick(kPad, PadAxis::LeftX, 0.9f), false) == MenuAction::None);
  CHECK(in.translate(stick(kPad, PadAxis::LeftX, 0.3f), true) == MenuAction::None);
  CHECK(in.translate(stick(kPad, PadAxis::LeftX, 0.7f), true) == MenuAction::Right);
  CHECK(in.translate(stick(kPad, PadAxis::LeftX, 1.0f), true) == MenuAction::None);
  CHECK(in.translate(stick(kPad, PadAxis::LeftX, 0.5f), true) == MenuAction::None);  // hysteresis
  CHECK(in.translate(stick(kPad, PadAxis::LeftX, 0.1f), true) == MenuAction::None);
  CHECK(in.translate(stick(kPad, PadAxis::LeftX, -0.8f), true) == MenuAction::Left);
  // +Y is down (SDL's sign); the axes latch independently.
  CHECK(in.translate(stick(kPad, PadAxis::LeftY, -0.8f), true) == MenuAction::Up);
  CHECK(in.translate(stick(kPad, PadAxis::LeftY, 0.0f), true) == MenuAction::None);
  CHECK(in.translate(stick(kPad, PadAxis::LeftY, 0.8f), true) == MenuAction::Down);
  // Another pad has its own latch; the right stick does not navigate.
  CHECK(in.translate(stick(kPad + 1, PadAxis::LeftY, 0.8f), true) == MenuAction::Down);
  CHECK(in.translate(stick(kPad, PadAxis::RightX, 1.0f), true) == MenuAction::None);
  // A removed pad forgets its latch.
  InputEvent removed;
  removed.kind = InputEventKind::PadRemoved;
  removed.deviceId = kPad;
  CHECK(in.translate(removed, true) == MenuAction::None);
  CHECK(in.translate(stick(kPad, PadAxis::LeftY, 0.8f), true) == MenuAction::Down);
}

TEST_CASE("menu input: only pads bound to a slot count as bound", "[client][lobby][input]") {
  platform::InputSampler sampler;
  constexpr uint32_t kPad = 11;
  InputEvent added;
  added.kind = InputEventKind::PadAdded;
  added.deviceId = kPad;
  sampler.push(added);
  CHECK(client::eventDeviceBound(sampler, key(Key::Up)));  // the keyboard always
  CHECK_FALSE(client::eventDeviceBound(sampler, padButton(kPad, PadButton::DpadUp)));
  REQUIRE(sampler.bind(PlayerId{3}, platform::DeviceBinding::pad(kPad)));
  CHECK(client::eventDeviceBound(sampler, padButton(kPad, PadButton::DpadUp)));
  CHECK_FALSE(client::eventDeviceBound(sampler, padButton(kPad + 1, PadButton::DpadUp)));
}

TEST_CASE("menu input: the UI keeps presses and clicks it captured (v0.45)",
          "[client][lobby][input][mouse]") {
  InputEvent click;
  click.kind = InputEventKind::MouseButtonDown;
  click.key = Key::MouseLeft;
  InputEvent release = click;
  release.kind = InputEventKind::MouseButtonUp;
  InputEvent motion;
  motion.kind = InputEventKind::MouseMotion;
  CHECK(client::uiCapturesEvent(click, false, true));
  CHECK_FALSE(client::uiCapturesEvent(click, true, false));
  CHECK_FALSE(client::uiCapturesEvent(release, true, true));  // releases always reach the game
  CHECK_FALSE(client::uiCapturesEvent(motion, true, true));
  CHECK(client::uiCapturesEvent(key(Key::J), true, false));
  CHECK_FALSE(client::uiCapturesEvent(key(Key::J), false, true));
  InputEvent keyUp = key(Key::J);
  keyUp.kind = InputEventKind::KeyUp;
  CHECK_FALSE(client::uiCapturesEvent(keyUp, true, true));
  CHECK_FALSE(client::uiCapturesEvent(padButton(5, PadButton::South), true, true));
  // The mouse never navigates a menu.
  MenuInput in;
  CHECK(in.translate(click, true) == MenuAction::None);
  CHECK(in.translate(motion, true) == MenuAction::None);
}

TEST_CASE("lobby settings: options names <-> indices", "[client][lobby][settings]") {
  const sim::ContentDB& db = shipped();
  ClientOptions o;
  o.arena = "pillars";
  o.mode = "deathmatch";
  o.loadout = "slam";
  o.powerUps = "shield,speed_boost";
  o.bots = 3;
  o.botDifficulty = ai::ReferenceTier::Easy;
  o.volume = 40;
  o.mute = true;
  LobbySettings s;
  std::string error;
  REQUIRE(client::lobbySettingsFrom(o, db, s, error));
  CHECK(s.arena == arenaIndex("pillars"));
  CHECK(s.mode == db.findMode("deathmatch").value_or(0xFF));
  CHECK(s.loadout == db.findLoadout("slam").value_or(0xFF));
  CHECK(s.powerUps == ((1u << db.findPowerUp("shield").value_or(0)) |
                       (1u << db.findPowerUp("speed_boost").value_or(0))));
  CHECK(s.bots == 3);
  CHECK(s.botTier == ai::ReferenceTier::Easy);
  CHECK(s.volume == 40);
  CHECK(s.mute);

  ClientOptions back;
  client::storeLobbySettings(s, db, back);
  CHECK(back.arena == "pillars");
  CHECK(back.mode == "deathmatch");
  CHECK(back.loadout == "slam");
  CHECK(back.powerUps == "speed_boost,shield");  // content order
  CHECK(back.botDifficulty == ai::ReferenceTier::Easy);
  LobbySettings again;
  REQUIRE(client::lobbySettingsFrom(back, db, again, error));
  CHECK(again == s);

  // Defaults: the first arena, mode and loadout, every power-up.
  REQUIRE(client::lobbySettingsFrom(ClientOptions{}, db, again, error));
  CHECK(again == LobbySettings{});
  o.loadout = "zap";
  CHECK_FALSE(client::lobbySettingsFrom(o, db, again, error));
  CHECK(error.find("unknown loadout 'zap'") != std::string::npos);
  o.loadout = "slam";
  o.powerUps = "lava";
  CHECK_FALSE(client::lobbySettingsFrom(o, db, again, error));
  CHECK(error.find("unknown power-up 'lava'") != std::string::npos);
}

TEST_CASE("lobby settings persist as text; stale lines are skipped, flags win",
          "[client][lobby][settings]") {
  const sim::ContentDB& db = shipped();
  ClientOptions saved;
  saved.arena = "island";
  saved.mode = "survival";
  saved.loadout = "parry";
  saved.powerUps = "none";
  saved.bots = 4;
  saved.botDifficulty = ai::ReferenceTier::Hard;
  saved.volume = 30;
  saved.mute = true;
  saved.region = "us";
  saved.theme = presentation::ThemeChoice::Temple;
  const std::string text = client::settingsText(saved);
  CHECK(text ==
        "settings_version=3\narena=island\nmode=survival\nloadout=parry\npowerups=none\nbots="
        "4\nbot_difficulty=hard\n"
        "volume=30\nmute=1\nregion=us\ntheme=temple\nmouse_aim=on\n");

  ClientOptions loaded;
  std::vector<std::string> warnings;
  client::applySettingsText(text, db, loaded, warnings);
  CHECK(warnings.empty());
  CHECK(client::settingsText(loaded) == text);
  CHECK(loaded.botDifficulty == ai::ReferenceTier::Hard);
  CHECK(loaded.theme == presentation::ThemeChoice::Temple);

  SECTION("names the content lost, bad numbers and unknown keys warn and are skipped") {
    ClientOptions o;
    warnings.clear();
    client::applySettingsText(
        "# kfm\n\narena=volcano\nmode = deathmatch \r\npowerups=shield,lava\nbots=12\n"
        "volume=loud\nmute=2\ncolour=red\njunk\nbot_difficulty=insane\nloadout=rounds_slam\n"
        "loadout=slam\n",
        db, o, warnings);
    CHECK(warnings.size() == 9);
    CHECK(o.loadout == "slam");  // the stale v0.24 mode name warned, the loadout applied
    CHECK(o.botDifficulty == ai::ReferenceTier::Medium);
    CHECK(o.arena.empty());
    CHECK(o.mode == "deathmatch");
    CHECK(o.powerUps.empty());
    CHECK(o.bots == 0);
    CHECK(o.volume == client::kDefaultVolume);
    CHECK_FALSE(o.mute);
    CHECK(warnings[0].find("line 3") != std::string::npos);
    CHECK(warnings[0].find("volcano") != std::string::npos);
  }
  SECTION("the command line (or the browser URL) overrides the saved settings") {
    ClientOptions o;
    warnings.clear();
    client::applySettingsText(text, db, o, warnings);
    std::string error;
    REQUIRE(parse({"--arena", "corridors", "--powerups", "shield", "--volume", "90",
                   "--bot-difficulty", "easy"},
                  o, error));
    CHECK(o.botDifficulty == ai::ReferenceTier::Easy);  // flag over the saved hard
    CHECK(o.arena == "corridors");                      // flag
    CHECK(o.mode == "survival");                        // saved
    CHECK(o.loadout == "parry");                        // saved
    CHECK(o.powerUps == "shield");                      // flag
    CHECK(o.bots == 4);                                 // saved
    CHECK(o.volume == 90);                              // flag
    CHECK(o.mute);                                      // saved
  }
}

TEST_CASE("client flags: --powerups, --no-settings, URL-style lists", "[client][lobby][options]") {
  ClientOptions o;
  std::string error;
  CHECK(o.settings);
  CHECK(o.powerUps.empty());
  // The browser page turns ?arena=pillars&mode=deathmatch&loadout=slam&powerups=a,b into these.
  REQUIRE(parse({"--arena", "pillars", "--mode", "deathmatch", "--loadout", "slam", "--powerups",
                 "shield,big_banana", "--no-settings"},
                o, error));
  CHECK(o.arena == "pillars");
  CHECK(o.mode == "deathmatch");
  CHECK(o.loadout == "slam");
  CHECK(o.powerUps == "shield,big_banana");
  CHECK_FALSE(o.settings);
  ClientOptions bad;
  CHECK_FALSE(parse({"--powerups"}, bad, error));
  CHECK(error == "--powerups needs a value");
  CHECK_FALSE(parse({"--loadout"}, bad, error));
  CHECK(error == "--loadout needs a value");
}

TEST_CASE("the session's power-up set reaches the lobby world and the match",
          "[client][lobby][session]") {
  const sim::ContentDB& db = shipped();
  client::MatchSession s(client::SessionConfig{
      .seed = 3, .arena = arenaIndex("dojo_powerups"), .content = &db, .bots = 1});
  CHECK(s.powerUps() == sim::kAllPowerUps);
  CHECK(s.matchConfig().powerUps == sim::kAllPowerUps);
  s.setPowerUps(0b10010);
  CHECK(s.matchConfig().powerUps == 0b10010);  // at once in the lobby
  CHECK(s.matchContent().powerUps == 0b10010);
  REQUIRE(s.startMatch(1'000'000'000ULL));
  CHECK(s.matchConfig().powerUps == 0b10010);
  s.setPowerUps(0);
  CHECK(s.matchConfig().powerUps == 0b10010);  // the running match keeps its set
  s.restart(2'000'000'000ULL, false);
  CHECK(s.matchConfig().powerUps == 0);
}

TEST_CASE("the session's loadout reaches the lobby world and the match (v0.31)",
          "[client][lobby][session]") {
  const sim::ContentDB& db = shipped();
  const uint8_t slam = db.findLoadout("slam").value_or(0);
  REQUIRE(slam != 0);
  client::MatchSession s(
      client::SessionConfig{.seed = 3, .loadout = slam, .content = &db, .bots = 1});
  CHECK(s.loadout() == slam);
  CHECK(s.matchConfig().loadout == slam);
  CHECK(s.matchContent().loadout() == db.loadouts[slam].abilities);
  REQUIRE(s.startMatch(1'000'000'000ULL));
  s.setLoadout(0);
  CHECK(s.matchConfig().loadout == slam);  // the running match keeps its loadout
  s.restart(2'000'000'000ULL, false);
  CHECK(s.matchConfig().loadout == 0);
  s.setLoadout(200);  // out of range falls back to the base kit
  CHECK(s.loadout() == 0);
  CHECK(client::loadoutSummary(db, slam) == "dash, throw, slam");
  CHECK(client::loadoutSummary(db, 0) == "dash, throw");
}

TEST_CASE("the session splits a team mode's slots into balanced teams (v0.32)",
          "[client][lobby][session][teams]") {
  const sim::ContentDB& db = shipped();
  const uint8_t teamRounds = db.findMode("team_rounds").value_or(0);
  const uint8_t standoff = arenaIndex("standoff");
  REQUIRE(teamRounds != 0);
  client::MatchSession s(client::SessionConfig{
      .seed = 3, .arena = standoff, .mode = teamRounds, .content = &db, .bots = 3});
  // Keyboard 0 on slot 0 plus three bots: 0 and 2 against 1 and 3.
  CHECK(s.teams() == sim::balancedTeams(2, 0b1111));
  REQUIRE(s.startMatch(1'000'000'000ULL));
  CHECK(s.matchConfig().teams == sim::balancedTeams(2, 0b1111));
  CHECK(s.matchContent().relation(PlayerId{0}, PlayerId{2}) == sim::Relation::Ally);
  CHECK(s.matchContent().relation(PlayerId{0}, PlayerId{1}) == sim::Relation::Enemy);
  // A free-for-all mode keeps slot i on team i.
  s.setMode(0);
  s.restart(2'000'000'000ULL, false);
  CHECK(s.matchConfig().teams == sim::kFreeForAllTeams);
}

TEST_CASE("mode summaries show the key numbers", "[client][lobby]") {
  sim::ModeDef rounds;
  rounds.roundsToWin = 3;
  CHECK(client::modeSummary(rounds) == "first to 3 rounds, last monkey standing");
  sim::ModeDef dm;
  dm.kind = sim::ModeKind::Deathmatch;
  dm.scoreToWin = 10;
  dm.timeLimitTicks = 10800;
  dm.respawnTicks = 120;
  dm.respawnInvulnTicks = 90;
  CHECK(client::modeSummary(dm) ==
        "first to 10 eliminations, 3:00, respawn 2.0 s (1.5 s protected)");
  dm.timeLimitTicks = 0;
  dm.respawnInvulnTicks = 0;
  CHECK(client::modeSummary(dm) == "first to 10 eliminations, no clock, respawn 2.0 s");
  dm.catchScore = 1;  // v0.57: catch (or suicide) points make the target points
  CHECK(client::modeSummary(dm) == "first to 10 points, no clock, respawn 2.0 s");
  sim::ModeDef survival;
  survival.kind = sim::ModeKind::Survival;
  survival.roundsToWin = 3;
  survival.timeLimitTicks = 3600;
  CHECK(client::modeSummary(survival) == "first to 3 rounds of 1:00, last monkey standing");
  sim::ModeDef control;  // v0.80
  control.kind = sim::ModeKind::Control;
  control.teams = 2;
  control.scoreToWin = 5;
  control.captureTicks = 90;
  control.hillTicks = 300;
  control.timeLimitTicks = 10800;
  CHECK(client::modeSummary(control) ==
        "first team to 5 points, a zone taken in 1.5 s pays a point per 5.0 s, 3:00");
  control.timeLimitTicks = 0;
  CHECK(client::modeSummary(control) ==
        "first team to 5 points, a zone taken in 1.5 s pays a point per 5.0 s, no clock");
  sim::ModeDef kills;  // v0.90
  kills.kind = sim::ModeKind::KillRounds;
  kills.scoreToWin = 10;
  CHECK(client::modeSummary(kills) ==
        "first to 10 kills, judged at a round's end, last monkey standing");
  sim::ModeDef harvest;  // v0.82
  harvest.kind = sim::ModeKind::Harvest;
  harvest.teams = 2;
  harvest.scoreToWin = 30;
  harvest.bankTicks = 20;
  harvest.stealTicks = 30;
  harvest.respawnAmmo = 0;
  harvest.timeLimitTicks = 3600;
  CHECK(client::modeSummary(harvest) ==
        "first team to 30 bananas banked, one per 0.33 s at home, a lone raider steals one per "
        "0.50 s, respawn empty-handed, 1:00");
  harvest.stealTicks = 0;
  harvest.bankTicks = 90;
  harvest.respawnAmmo = sim::kRespawnAmmoStart;
  harvest.timeLimitTicks = 0;
  CHECK(client::modeSummary(harvest) ==
        "first team to 30 bananas banked, one per 1.5 s at home, no stealing, no clock");
  // v0.82: an arena's trees are not power-up spawners.
  sim::ArenaDef orchard;
  CHECK(client::arenaSpawners(orchard) == client::ArenaSpawners{});
  orchard.pickupSpawners[0].tree = 1;
  orchard.pickupSpawners[2].tree = 1;
  orchard.pickupSpawnerCount = 3;
  CHECK(client::arenaSpawners(orchard) == client::ArenaSpawners{.powerUps = 1, .trees = 2});
  CHECK(client::treesText(1) == "1 tree bears bananas");
  CHECK(client::treesText(2) == "2 trees bear bananas");
  CHECK(client::ticksClock(61) == "0:02");
  CHECK(client::ticksClock(0) == "0:00");
}

TEST_CASE("client flags and settings: the nickname (v0.42)", "[client][lobby][options]") {
  client::ClientOptions o;
  std::string error;
  const std::array<const char*, 3> args{"kfm_client", "--nick", "  Kong\x01 "};
  REQUIRE(client::parseClientOptions(args, o, error));
  CHECK(o.nick == "Kong");
  const std::array<const char*, 3> blank{"kfm_client", "--nick", " \t "};
  client::ClientOptions b;
  CHECK_FALSE(client::parseClientOptions(blank, b, error));
  CHECK(error.find("--nick") != std::string::npos);
  CHECK(client::netSessionConfig(o, true).nickname == "Kong");

  // Saved as nick=, read back; a bad line is skipped with a warning.
  const std::string text = client::settingsText(o);
  CHECK(text.find("nick=Kong\n") != std::string::npos);
  client::ClientOptions back;
  std::vector<std::string> warnings;
  client::applySettingsText(text, sim::defaultContent(), back, warnings);
  CHECK(warnings.empty());
  CHECK(back.nick == "Kong");
  client::applySettingsText("nick=\x02\n", sim::defaultContent(), back, warnings);
  CHECK(warnings.size() == 1);
  CHECK(back.nick == "Kong");
  CHECK(client::settingsText(client::ClientOptions{}).find("nick=") == std::string::npos);

  // A random monkey name: valid, deterministic per seed, varied across seeds.
  CHECK(client::randomMonkeyName(1) == client::randomMonkeyName(1));
  std::set<std::string> names;
  for (uint64_t seed = 0; seed < 64; ++seed) {
    const std::string n = client::randomMonkeyName(seed);
    CHECK(net::validNickname(n));
    CHECK(!n.empty());
    names.insert(n);
  }
  CHECK(names.size() > 32);
}

TEST_CASE("the arena theme: flag, settings and the lobby row (v0.52)", "[client][lobby][options]") {
  const sim::ContentDB& db = shipped();
  ClientOptions o;
  std::string error;
  CHECK(o.theme == presentation::ThemeChoice::Beach);  // v0.93: Banana Beach by default
  // The browser page turns ?theme=jungle into this.
  REQUIRE(parse({"--theme", "jungle"}, o, error));
  CHECK(o.theme == presentation::ThemeChoice::Jungle);
  REQUIRE(parse({"--theme", "beach"}, o, error));
  CHECK(o.theme == presentation::ThemeChoice::Beach);
  ClientOptions bad;
  CHECK_FALSE(parse({"--theme", "lava"}, bad, error));
  CHECK(error == "--theme must be auto, jungle, temple or beach, got 'lava'");
  CHECK_FALSE(parse({"--theme"}, bad, error));

  std::vector<std::string> warnings;
  client::applySettingsText("theme=volcano\n", db, o, warnings);
  CHECK(warnings.size() == 1);
  CHECK(o.theme == presentation::ThemeChoice::Beach);  // a bad line keeps the setting

  LobbySettings ls;
  REQUIRE(client::lobbySettingsFrom(o, db, ls, error));
  CHECK(ls.theme == presentation::ThemeChoice::Beach);

  // v0.97: the theme is Settings', not the setup panel's.
  for (const LobbyRowKind r : LobbyMenu::rows())
    CHECK(std::string(LobbyMenu::rowLabel(r)) != "Arena theme");
}

TEST_CASE("saved settings from before v0.93 move from auto to the beach once",
          "[client][lobby][options]") {
  const sim::ContentDB& db = shipped();
  std::vector<std::string> warnings;
  SECTION("an old text's theme=auto, the old default, reads as beach") {
    ClientOptions o;
    client::applySettingsText("arena=dojo\ntheme=auto\nmouse_aim=on\n", db, o, warnings);
    CHECK(warnings.empty());
    CHECK(o.theme == presentation::ThemeChoice::Beach);
    // Saved again it says so, and a later read keeps it.
    const std::string text = client::settingsText(o);
    CHECK(text.find("settings_version=3\n") == 0);
    CHECK(text.find("theme=beach\n") != std::string::npos);
  }
  SECTION("an old text's deliberate jungle or temple stays") {
    ClientOptions o;
    client::applySettingsText("theme=jungle\n", db, o, warnings);
    CHECK(o.theme == presentation::ThemeChoice::Jungle);
    client::applySettingsText("theme=temple\n", db, o, warnings);
    CHECK(o.theme == presentation::ThemeChoice::Temple);
  }
  SECTION("auto chosen after the move is kept, whatever the line order") {
    ClientOptions o;
    client::applySettingsText("theme=auto\nsettings_version=2\n", db, o, warnings);
    CHECK(warnings.empty());
    CHECK(o.theme == presentation::ThemeChoice::Auto);
    ClientOptions round;
    client::applySettingsText(client::settingsText(o), db, round, warnings);
    CHECK(round.theme == presentation::ThemeChoice::Auto);
  }
  SECTION("a bad version warns and counts as the old format") {
    ClientOptions o;
    client::applySettingsText("settings_version=x\ntheme=auto\n", db, o, warnings);
    CHECK(warnings.size() == 1);
    CHECK(o.theme == presentation::ThemeChoice::Beach);
  }
  SECTION("a flag still wins for the run") {
    ClientOptions o;
    client::applySettingsText("theme=auto\n", db, o, warnings);
    std::string error;
    REQUIRE(parse({"--theme", "auto"}, o, error));
    CHECK(o.theme == presentation::ThemeChoice::Auto);
  }
}

TEST_CASE("the default volume is 40 and an old text's volume=80 reads as it (v0.97)",
          "[client][lobby][options]") {
  const sim::ContentDB& db = shipped();
  std::vector<std::string> warnings;
  CHECK(ClientOptions{}.volume == 40);
  CHECK(LobbySettings{}.volume == 40);
  CHECK(std::lround(presentation::kDefaultMasterVolume * 100.0f) == 40);
  SECTION("volume=80 from a version 1 or 2 text was the old default: now 40") {
    for (const char* text :
         {"volume=80\n", "settings_version=2\nvolume=80\n", "volume=80\nsettings_version=2\n"}) {
      ClientOptions o;
      client::applySettingsText(text, db, o, warnings);
      CHECK(o.volume == 40);
    }
    CHECK(warnings.empty());
  }
  SECTION("another volume in an old text was chosen: kept") {
    ClientOptions o;
    client::applySettingsText("settings_version=2\nvolume=70\n", db, o, warnings);
    CHECK(o.volume == 70);
    client::applySettingsText("volume=0\n", db, o, warnings);
    CHECK(o.volume == 0);
  }
  SECTION("80 saved from version 3 on is the player's: kept, and round-trips") {
    ClientOptions o;
    o.volume = 80;
    const std::string text = client::settingsText(o);
    CHECK(text.find("settings_version=3\n") == 0);
    ClientOptions round;
    client::applySettingsText(text, db, round, warnings);
    CHECK(round.volume == 80);
  }
  SECTION("a flag wins for the run") {
    ClientOptions o;
    client::applySettingsText("volume=80\n", db, o, warnings);
    std::string error;
    REQUIRE(parse({"--volume", "80"}, o, error));
    CHECK(o.volume == 80);
  }
}

TEST_CASE("mouse aim: flag, browser query, settings round trip", "[client][lobby][options]") {
  const sim::ContentDB& db = shipped();
  ClientOptions o;
  std::string error;
  CHECK(o.mouseAim);  // on by default (v0.45's behaviour)
  REQUIRE(parse({"--mouse-aim", "off"}, o, error));
  CHECK_FALSE(o.mouseAim);
  // The browser page turns ?mouse_aim=on into --mouse_aim on.
  REQUIRE(parse({"--mouse_aim", "on"}, o, error));
  CHECK(o.mouseAim);
  ClientOptions bad;
  CHECK_FALSE(parse({"--mouse-aim", "yes"}, bad, error));
  CHECK(error == "--mouse-aim must be on or off, got 'yes'");
  CHECK_FALSE(parse({"--mouse-aim"}, bad, error));
  CHECK(error == "--mouse-aim needs on or off");

  // Saved as mouse_aim=on|off and read back; a bad value warns and keeps it.
  ClientOptions saved;
  saved.mouseAim = false;
  const std::string text = client::settingsText(saved);
  CHECK(text.find("\nmouse_aim=off\n") != std::string::npos);
  ClientOptions loaded;
  std::vector<std::string> warnings;
  client::applySettingsText(text, db, loaded, warnings);
  CHECK(warnings.empty());
  CHECK_FALSE(loaded.mouseAim);
  client::applySettingsText("mouse_aim=1\n", db, loaded, warnings);
  CHECK(warnings.size() == 1);
  CHECK_FALSE(loaded.mouseAim);

  // A flag wins over the saved value (main.cpp parses the flags again on top).
  REQUIRE(parse({"--mouse-aim", "on"}, loaded, error));
  CHECK(loaded.mouseAim);

  // Into the settings the Settings screen edits, and back into the options.
  LobbySettings ls;
  REQUIRE(client::lobbySettingsFrom(saved, db, ls, error));
  CHECK_FALSE(ls.mouseAim);
  ls.mouseAim = true;
  client::storeLobbySettings(ls, db, saved);
  CHECK(saved.mouseAim);
}

TEST_CASE("client flags: --match, --ladder and --t", "[client][lobby][options]") {
  std::string error;
  // The browser passes ?match=ID&t=TICK through as --match ID --t TICK.
  ClientOptions o;
  REQUIRE(parse({"--match", "0193f2a1-7c3e-7b1a-9d2e-5f6a7b8c9d0e", "--t", "4521"}, o, error));
  CHECK(o.replayMode());
  CHECK(o.replay ==
        "https://bots.kfm.minip8.com/matches/0193f2a1-7c3e-7b1a-9d2e-5f6a7b8c9d0e/replay");
  CHECK(o.startTick == 4521);
  ClientOptions dev;
  REQUIRE(parse({"--ladder", "http://127.0.0.1:8090/", "--match", "m1"}, dev, error));
  CHECK(dev.replay == "http://127.0.0.1:8090/matches/m1/replay");
  CHECK(dev.startTick == 0);
  ClientOptions file;
  REQUIRE(parse({"--t", "60", "--replay", "a.kfmr"}, file, error));
  CHECK(file.startTick == 60);

  // A fresh ClientOptions per case: a refused parse leaves what it read.
  const auto refused = [&](const std::vector<const char*>& argv) {
    ClientOptions c;
    return !parse(argv, c, error);
  };
  CHECK(refused({"--match", "../x"}));
  CHECK(error.find("--match") != std::string::npos);
  CHECK(refused({"--match", ""}));
  CHECK(refused({"--match", "m1", "--replay", "a.kfmr"}));
  CHECK(error.find("exclude") != std::string::npos);
  CHECK(refused({"--ladder", "ftp://x", "--match", "m1"}));
  CHECK(error.find("--ladder") != std::string::npos);
  CHECK(refused({"--t", "60"}));
  CHECK(error.find("--t") != std::string::npos);
  CHECK(refused({"--match", "m1", "--t", "4294967296"}));
  CHECK(refused({"--match", "m1", "--t", "-1"}));
  CHECK(refused({"--match", "m1", "--autostart"}));
  CHECK(client::isMatchId("0193f2a1-7c3e-7b1a-9d2e-5f6a7b8c9d0e"));
  CHECK_FALSE(client::isMatchId(std::string(65, 'a')));
  CHECK_FALSE(client::isMatchId("a/b"));
  CHECK_FALSE(client::isMatchId("a?b"));
}

TEST_CASE("client flags: the replay viewer's Copy link", "[client][lobby][options]") {
  using client::replayShareLink;
  CHECK(replayShareLink("", 600).empty());  // natively: no page
  CHECK(replayShareLink("https://kfm.minip8.com/v0.11.0/?match=m1", 600) ==
        "https://kfm.minip8.com/v0.11.0/?match=m1&t=600");
  // The link's own t is replaced; at tick 0 it goes.
  CHECK(replayShareLink("https://kfm.minip8.com/v0.11.0/?match=m1&t=90", 600) ==
        "https://kfm.minip8.com/v0.11.0/?match=m1&t=600");
  CHECK(replayShareLink("https://kfm.minip8.com/v0.11.0/?t=90&match=m1", 0) ==
        "https://kfm.minip8.com/v0.11.0/?match=m1");
  // Other parameters stay (a long ?replay= link, a development ladder);
  // a key that merely starts with t stays too; the fragment goes.
  CHECK(replayShareLink(
            "http://127.0.0.1:8000/kfm_client.html?ladder=http://x&match=m1&theme=dark#a", 5) ==
        "http://127.0.0.1:8000/kfm_client.html?ladder=http://x&match=m1&theme=dark&t=5");
  CHECK(replayShareLink("https://kfm.minip8.com/?replay=https%3A%2F%2Fb%2Fr.kfmr%3Fsig%3D1", 7) ==
        "https://kfm.minip8.com/?replay=https%3A%2F%2Fb%2Fr.kfmr%3Fsig%3D1&t=7");
  CHECK(replayShareLink("https://kfm.minip8.com/", 7) == "https://kfm.minip8.com/?t=7");
}

TEST_CASE("client flags: --replay and what it excludes", "[client][lobby][options]") {
  ClientOptions o;
  std::string error;
  CHECK_FALSE(o.replayMode());
  REQUIRE(parse({"--replay", "match.kfmr", "--mute"}, o, error));
  CHECK(o.replay == "match.kfmr");
  CHECK(o.replayMode());
  // The browser passes ?replay=<url> through as --replay <url>.
  ClientOptions web;
  REQUIRE(parse({"--replay", "https://bots.kfm.minip8.com/replays/1.kfmr"}, web, error));
  CHECK(web.replay == "https://bots.kfm.minip8.com/replays/1.kfmr");
  // --lobby alone (the page's config.js always passes one) is not a room.
  ClientOptions lobby;
  REQUIRE(parse({"--lobby", "wss://lobby.example", "--replay", "a.kfmr"}, lobby, error));

  ClientOptions bad;
  CHECK_FALSE(parse({"--replay"}, bad, error));
  CHECK(error.find("--replay") != std::string::npos);
  CHECK_FALSE(parse({"--replay", ""}, bad, error));
  for (const std::vector<const char*>& other : std::vector<std::vector<const char*>>{
           {"--autostart"},
           {"--connect", "ws://127.0.0.1:9000"},
           {"--lobby", "ws://127.0.0.1:8080", "--room", "ABCDE"},
           {"--lobby", "ws://127.0.0.1:8080", "--create"},
           {"--screen", "menu"},
       }) {
    std::vector<const char*> argv = other;
    argv.push_back("--replay");
    argv.push_back("a.kfmr");
    ClientOptions c;
    INFO(other.front());
    CHECK_FALSE(parse(argv, c, error));
    CHECK(error.find("--replay") != std::string::npos);
    CHECK(error.find(other.front() == std::string("--lobby") ? other[2] : other.front()) !=
          std::string::npos);
  }
}
