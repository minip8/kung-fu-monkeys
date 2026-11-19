// The v0.94 usability models, headless: readable content names and what a
// human lobby offers (core/content_labels.hpp), the rows the panel shows,
// the How to play card (core/how_to_play.hpp), the first round's hint and
// the "name chosen" setting.
#include "core/content_labels.hpp"

#include "core/client_options.hpp"
#include "core/how_to_play.hpp"
#include "core/hud_view.hpp"
#include "core/lobby_menu.hpp"
#include "core/lobby_settings.hpp"
#include "kfm/content/loader.hpp"
#include "kfm/platform/input_sampler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <vector>

using namespace kfm;

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

[[nodiscard]] bool parse(std::vector<const char*> argv, client::ClientOptions& o,
                         std::string& error) {
  argv.insert(argv.begin(), "kfm_client");
  return client::parseClientOptions(std::span<const char* const>(argv.data(), argv.size()), o,
                                    error);
}

}  // namespace

TEST_CASE("content names read as words, new content included", "[client][labels]") {
  CHECK(client::prettyContentName("team_deathmatch") == "Team Deathmatch");
  CHECK(client::prettyContentName("big_banana_1t") == "Big Banana (1-tick)");
  CHECK(client::prettyContentName("courtyard_zones") == "Courtyard Zones");
  CHECK(client::prettyContentName("x") == "X");
  CHECK(client::prettyContentName("a__b_") == "A B");
  CHECK(client::prettyContentName("").empty());

  // The table's modes have a title and a line on how to win.
  CHECK(client::modeLabel("hill").title == "King of the Hill");
  CHECK(client::modeLabel("rounds").title == "Last Monkey Standing");
  CHECK_FALSE(client::modeLabel("capture").blurb.empty());
  // v0.90's kill_rounds has its own title; a name the table lacks falls back.
  CHECK(client::modeLabel("kill_rounds").title == "Knockout Rounds");
  const client::ModeLabel unknown = client::modeLabel("lava_tag_2v2");
  CHECK(unknown.title == "Lava Tag 2v2");
  CHECK(unknown.blurb.empty());
  CHECK(client::arenaTitle("dojo_powerups") == "Dojo with Power-ups");
  CHECK(client::arenaTitle("island") == "Island");
  CHECK(client::arenaTitle("big_lagoon") == "Big Lagoon");
  CHECK(client::loadoutTitle("base") == "Standard");
  CHECK(client::loadoutTitle("slam") == "Slam");
  CHECK(client::powerUpTitle("speed_boost") == "Speed Boost");

  // Every shipped name gets a title that is not the raw name with underscores.
  const sim::ContentDB& db = shipped();
  for (uint8_t i = 0; i < db.modeCount; ++i) {
    CHECK(client::modeLabel(db.modes[i].name.view()).title.find('_') == std::string::npos);
  }
  for (uint8_t i = 0; i < db.arenaCount; ++i) {
    CHECK(client::arenaTitle(db.arenas[i].name.view()).find('_') == std::string::npos);
  }
}

TEST_CASE("bot contests are not offered to people", "[client][labels][lobby]") {
  const sim::ContentDB& db = shipped();
  const auto mode = [&](std::string_view name) {
    const std::optional<uint8_t> m = db.findMode(name);
    if (!m) FAIL("no mode " << name);
    return db.modeAt(m.value_or(0));
  };
  CHECK(client::isBotMode(mode("contest")));
  CHECK(client::isBotMode(mode("control_1t")));
  CHECK(client::isBotMode(mode("grid_rounds_1t")));
  CHECK_FALSE(client::isBotMode(mode("grid_rounds")));  // 15-tick turns: people play it
  CHECK_FALSE(client::isBotMode(mode("rounds")));
  CHECK_FALSE(client::isBotMode(mode("hill")));
  const auto loadout = [&](std::string_view name) {
    const std::optional<uint8_t> l = db.findLoadout(name);
    if (!l) FAIL("no loadout " << name);
    return l.value_or(0);
  };
  CHECK(client::isBotLoadout(db, loadout("grid_1t")));
  CHECK(client::isBotLoadout(db, loadout("catcher_1t")));
  CHECK_FALSE(client::isBotLoadout(db, loadout("grid")));  // grid_rounds hands it out
  CHECK_FALSE(client::isBotLoadout(db, loadout("base")));
  CHECK_FALSE(client::isBotLoadout(db, loadout("slam")));
}

TEST_CASE("role kits are the ones a mode hands out by role (v0.97)", "[client][lobby]") {
  const sim::ContentDB& db = shipped();
  const auto loadout = [&](std::string_view name) {
    const std::optional<uint8_t> l = db.findLoadout(name);
    if (!l) FAIL("no loadout " << name);
    return l.value_or(0);
  };
  CHECK(client::isRoleLoadout(db, loadout("grid")));  // grid_rounds hands it out
  CHECK(client::isRoleLoadout(db, loadout("catcher")));
  CHECK(client::isRoleLoadout(db, loadout("grid_1t")));
  CHECK(client::isRoleLoadout(db, loadout("catcher_1t")));
  CHECK_FALSE(client::isRoleLoadout(db, loadout("base")));
  CHECK_FALSE(client::isRoleLoadout(db, loadout("slam")));
  CHECK_FALSE(client::isRoleLoadout(db, loadout("blink")));
  CHECK_FALSE(client::isRoleLoadout(db, loadout("parry")));
  // The dojo draws no power-up; its twin with spawners does.
  CHECK(client::arenaPowerUps(db, db.arenaAt(db.findArena("dojo").value_or(0))) == 0);
  CHECK(client::arenaPowerUps(db, db.arenaAt(db.findArena("dojo_powerups").value_or(0))) != 0);
}

TEST_CASE("How to play reads the bindings the sampler plays with", "[client][howto]") {
  const platform::KeyboardProfile keys = platform::defaultKeyboardProfile1();
  const platform::KeyboardProfile right = platform::defaultKeyboardProfile2();
  const platform::GamepadProfile pad = platform::defaultGamepadProfile();
  const client::HowToPlay card = client::howToPlay(keys, right, pad, true);
  REQUIRE(card.rows.size() == 6);
  CHECK(card.rows[0].action == "Move");
  CHECK(card.rows[0].keyboard == "WASD");
  CHECK(card.rows[1].action == "Aim");
  CHECK(card.rows[1].keyboard == "The mouse");
  CHECK(card.rows[2].action == "Throw");
  CHECK(card.rows[2].keyboard == "J, Space or Left click");
  CHECK(card.rows[2].gamepad == "X or RB");
  CHECK(card.rows[3].action == "Recall");
  CHECK(card.rows[3].keyboard == "Hold J, Space or Left click");
  CHECK(card.rows[4].action == "Dash");
  CHECK(card.rows[4].keyboard == "K, Left Shift or Right click");
  CHECK(card.rows[4].gamepad == "A or LB");
  CHECK(card.rows[4].what.find("catch") != std::string::npos);
  CHECK(card.rows[5].action == "Ability");
  CHECK(card.rows[5].keyboard == "L or E");
  CHECK(card.rows[5].gamepad == "Y or B");
  CHECK(card.secondKeyboard.find("the arrow keys") != std::string::npos);
  CHECK(card.secondKeyboard.find(". or Right Ctrl throw") != std::string::npos);
  CHECK_FALSE(card.tips.empty());

  // Mouse aim off: the move direction aims.
  CHECK(client::howToPlay(keys, right, pad, false).rows[1].keyboard == "The way you move");
  // A rebound profile shows its own keys.
  platform::KeyboardProfile custom = keys;
  custom[platform::Action::Throw] = {platform::Key::F, platform::Key::Unknown,
                                     platform::Key::Unknown};
  CHECK(client::howToPlay(custom, right, pad, true).rows[2].keyboard == "F");

  CHECK(client::keyName(platform::Key::Apostrophe) == "'");
  CHECK(client::keyName(platform::Key::Num7) == "7");
  CHECK(client::keyName(platform::Key::F11) == "F11");
  CHECK(client::padButtonName(platform::PadButton::DpadLeft) == "D-pad left");

  const std::string hint = client::firstRoundHint(keys, true);
  CHECK(hint.find("WASD") != std::string::npos);
  CHECK(hint.find("mouse aims") != std::string::npos);
  CHECK(hint.find("hold J / Left click: throw") != std::string::npos);
  CHECK(hint.find("K / Right click: dash") != std::string::npos);
  CHECK(client::firstRoundHint(keys, false).find("where you move") != std::string::npos);
}

TEST_CASE("the first round's countdown carries the controls line", "[client][hud]") {
  presentation::RenderHud hud;
  hud.phase = presentation::HudPhase::RoundStarting;
  hud.round = 1;
  CHECK(client::showsFirstRoundHint(hud));
  hud.round = 2;
  CHECK_FALSE(client::showsFirstRoundHint(hud));
  hud.round = 1;
  hud.phase = presentation::HudPhase::Playing;
  CHECK_FALSE(client::showsFirstRoundHint(hud));
}

TEST_CASE("the name is the player's own once chosen (v0.94)", "[client][options]") {
  const sim::ContentDB& db = shipped();
  client::ClientOptions o;
  CHECK_FALSE(o.nickChosen);
  o.nick = "Capuchin 7";
  CHECK(client::settingsText(o).find("nick_chosen") == std::string::npos);
  o.nickChosen = true;
  const std::string text = client::settingsText(o);
  CHECK(text.find("nick_chosen=on\n") != std::string::npos);
  client::ClientOptions loaded;
  std::vector<std::string> warnings;
  client::applySettingsText(text, db, loaded, warnings);
  CHECK(warnings.empty());
  CHECK(loaded.nickChosen);
  CHECK(loaded.nick == "Capuchin 7");
  client::applySettingsText("nick_chosen=maybe\n", db, loaded, warnings);
  CHECK(warnings.size() == 1);
  // --nick is a chosen name.
  client::ClientOptions flagged;
  std::string error;
  REQUIRE(parse({"--nick", "Gibbon"}, flagged, error));
  CHECK(flagged.nickChosen);
  REQUIRE(parse({"--screen", "howto"}, flagged, error));
  CHECK(flagged.screen == "howto");
}
