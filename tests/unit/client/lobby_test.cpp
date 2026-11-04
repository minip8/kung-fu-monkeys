// The local lobby's settings panel (architecture §7, v0.24): the LobbyMenu
// state machine, the generic menu input, the settings persistence and the
// flag / URL parsing, headless over the shipped (embedded) content.
#include "core/client_options.hpp"
#include "core/lobby_menu.hpp"
#include "core/lobby_settings.hpp"
#include "core/match_session.hpp"
#include "kfm/content/loader.hpp"
#include "kfm/platform/input_event.hpp"
#include "kfm/sim/content.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace kfm;
using client::ClientOptions;
using client::LobbyMenu;
using client::LobbyRow;
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

[[nodiscard]] std::size_t rowOf(const LobbyMenu& m, LobbyRowKind kind, uint8_t powerUp = 0) {
  const auto rows = m.rows();
  std::size_t found = rows.size();
  for (std::size_t i = 0; i < rows.size() && found == rows.size(); ++i) {
    if (rows[i].kind == kind && (kind != LobbyRowKind::PowerUp || rows[i].powerUp == powerUp)) {
      found = i;
    }
  }
  if (found == rows.size()) FAIL("no such row");  // MSVC: no unreachable return after FAIL
  return found;
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

TEST_CASE("lobby menu rows follow the content", "[client][lobby]") {
  const sim::ContentDB& db = shipped();
  REQUIRE(db.arenaCount >= 2);
  REQUIRE(db.modeCount >= 2);
  REQUIRE(db.powerUpCount >= 2);
  LobbyMenu m;
  m.setContent(db);
  const auto rows = m.rows();
  REQUIRE(rows.size() == 7u + db.powerUpCount);
  CHECK(rows[0].kind == LobbyRowKind::Arena);
  CHECK(rows[1].kind == LobbyRowKind::Mode);
  CHECK(rows[2].kind == LobbyRowKind::Loadout);
  CHECK(m.rowLabel(rows[2]) == "Loadout");
  CHECK(m.rowValue(rows[2]) == "base");
  for (uint8_t i = 0; i < db.powerUpCount; ++i) {
    CHECK(rows[3 + i] == LobbyRow{LobbyRowKind::PowerUp, i});
    CHECK(m.rowLabel(rows[3 + i]) == db.powerUps[i].name.view());
  }
  CHECK(rows[rows.size() - 4].kind == LobbyRowKind::Bots);
  CHECK(rows[rows.size() - 3].kind == LobbyRowKind::BotDifficulty);
  CHECK(m.rowLabel(rows[rows.size() - 3]) == "Bot difficulty");
  CHECK(rows[rows.size() - 2].kind == LobbyRowKind::Volume);
  CHECK(rows.back().kind == LobbyRowKind::Mute);
  CHECK(m.focus() == 0);

  // A menu that never saw content still has the default fixture's rows.
  const LobbyMenu bare;
  CHECK(bare.rows().size() == 7u + sim::defaultContent().powerUpCount);
}

TEST_CASE("lobby menu: focus wraps, values cycle, toggle and clamp", "[client][lobby]") {
  const sim::ContentDB& db = shipped();
  LobbyMenu m;
  m.setContent(db);
  const std::size_t n = m.rows().size();

  SECTION("Up from the first row wraps to the last, Down back") {
    CHECK_FALSE(m.apply(MenuAction::Up));
    CHECK(m.focus() == n - 1);
    CHECK_FALSE(m.apply(MenuAction::Down));
    CHECK(m.focus() == 0);
    CHECK_FALSE(m.apply(MenuAction::None));
  }
  SECTION("arena cycles both ways with wrap-around") {
    CHECK(m.rowValue(m.focusedRow()) == db.arenas[0].name.view());
    CHECK(m.apply(MenuAction::Right));
    CHECK(m.settings().arena == 1);
    CHECK(m.rowValue(m.focusedRow()) == db.arenas[1].name.view());
    CHECK(m.apply(MenuAction::Left));
    CHECK(m.apply(MenuAction::Left));
    CHECK(m.settings().arena == db.arenaCount - 1);
    CHECK(m.apply(MenuAction::Right));
    CHECK(m.settings().arena == 0);
  }
  SECTION("mode cycles") {
    (void)m.apply(MenuAction::Down);
    CHECK(m.focusedRow().kind == LobbyRowKind::Mode);
    for (uint8_t i = 1; i <= db.modeCount; ++i) {
      CHECK(m.apply(MenuAction::Right));
      CHECK(m.settings().mode == i % db.modeCount);
    }
  }
  SECTION("loadout cycles through base and content/loadouts.toml (v0.31)") {
    REQUIRE(db.loadoutCount >= 2);
    m.setFocus(rowOf(m, LobbyRowKind::Loadout));
    for (uint8_t i = 1; i <= db.loadoutCount; ++i) {
      CHECK(m.apply(MenuAction::Right));
      CHECK(m.settings().loadout == i % db.loadoutCount);
      CHECK(m.rowValue(m.focusedRow()) == db.loadoutAt(m.settings().loadout).name.view());
    }
    CHECK(m.apply(MenuAction::Left));
    CHECK(m.settings().loadout == db.loadoutCount - 1);
  }
  SECTION("a power-up toggles with either direction") {
    const std::size_t row = rowOf(m, LobbyRowKind::PowerUp, 1);
    CHECK(m.settings().powerUps == sim::kAllPowerUps);
    CHECK(m.change(row, +1));
    CHECK((m.settings().powerUps & 0b10) == 0);
    CHECK(m.rowValue(m.rows()[row]) == "off");
    CHECK(m.change(row, -1));
    CHECK(m.settings().powerUps == sim::kAllPowerUps);
    CHECK(m.rowValue(m.rows()[row]) == "on");
  }
  SECTION("bots step within 0..9, volume within 0..100 by 10, mute toggles") {
    const std::size_t bots = rowOf(m, LobbyRowKind::Bots);
    CHECK_FALSE(m.change(bots, -1));  // already 0: nothing changes
    for (int i = 0; i < 12; ++i)
      (void)m.change(bots, +1);
    CHECK(m.settings().bots == sim::kMaxPlayers - 1);
    CHECK_FALSE(m.change(bots, +1));

    const std::size_t volume = rowOf(m, LobbyRowKind::Volume);
    CHECK(m.settings().volume == 80);
    CHECK(m.change(volume, +1));
    CHECK(m.settings().volume == 90);
    CHECK(m.change(volume, +1));
    CHECK_FALSE(m.change(volume, +1));
    CHECK(m.rowValue(m.rows()[volume]) == "100%");
    for (int i = 0; i < 12; ++i)
      (void)m.change(volume, -1);
    CHECK(m.settings().volume == 0);

    const std::size_t mute = rowOf(m, LobbyRowKind::Mute);
    m.setFocus(mute);
    CHECK(m.apply(MenuAction::Right));
    CHECK(m.settings().mute);
    CHECK(m.rowValue(m.focusedRow()) == "muted");
    CHECK(m.apply(MenuAction::Left));
    CHECK_FALSE(m.settings().mute);
  }
  SECTION("bot difficulty cycles easy, medium, hard with wrap-around") {
    const std::size_t row = rowOf(m, LobbyRowKind::BotDifficulty);
    m.setFocus(row);
    CHECK(m.settings().botTier == ai::ReferenceTier::Medium);  // the casual default
    CHECK(m.rowValue(m.focusedRow()) == "medium");
    CHECK(m.apply(MenuAction::Right));
    CHECK(m.settings().botTier == ai::ReferenceTier::Hard);
    CHECK(m.rowValue(m.focusedRow()) == "hard");
    CHECK(m.apply(MenuAction::Right));
    CHECK(m.settings().botTier == ai::ReferenceTier::Easy);
    CHECK(m.apply(MenuAction::Left));
    CHECK(m.apply(MenuAction::Left));
    CHECK(m.settings().botTier == ai::ReferenceTier::Medium);
  }
  SECTION("out-of-range rows and directions do nothing") {
    CHECK_FALSE(m.change(n, +1));
    CHECK_FALSE(m.change(0, 0));
    m.setFocus(n + 3);
    CHECK(m.focus() == 0);
  }
}

TEST_CASE("lobby menu: new content clamps the settings and keeps the focus", "[client][lobby]") {
  const sim::ContentDB& db = shipped();
  LobbyMenu m;
  m.setContent(db);
  m.settings().arena = static_cast<uint8_t>(db.arenaCount - 1);
  m.settings().mode = static_cast<uint8_t>(db.modeCount - 1);
  m.settings().loadout = static_cast<uint8_t>(db.loadoutCount - 1);
  m.setFocus(rowOf(m, LobbyRowKind::Volume));
  m.setContent(sim::defaultContent());  // one arena, one mode, one loadout
  CHECK(m.settings().arena == 0);
  CHECK(m.settings().mode == 0);
  CHECK(m.settings().loadout == 0);
  CHECK(m.focusedRow().kind == LobbyRowKind::Volume);
  m.setFocus(rowOf(m, LobbyRowKind::Arena));
  CHECK_FALSE(m.apply(MenuAction::Right));  // a single arena cycles onto itself
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
  const std::string text = client::settingsText(saved);
  CHECK(text ==
        "arena=island\nmode=survival\nloadout=parry\npowerups=none\nbots=4\nbot_difficulty=hard\n"
        "volume=30\nmute=1\n");

  ClientOptions loaded;
  std::vector<std::string> warnings;
  client::applySettingsText(text, db, loaded, warnings);
  CHECK(warnings.empty());
  CHECK(client::settingsText(loaded) == text);
  CHECK(loaded.botDifficulty == ai::ReferenceTier::Hard);

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
    CHECK(o.volume == 80);
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
  sim::ModeDef survival;
  survival.kind = sim::ModeKind::Survival;
  survival.roundsToWin = 3;
  survival.timeLimitTicks = 3600;
  CHECK(client::modeSummary(survival) == "first to 3 rounds of 1:00, last monkey standing");
  CHECK(client::ticksClock(61) == "0:02");
  CHECK(client::ticksClock(0) == "0:00");
}
