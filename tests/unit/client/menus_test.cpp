// The client's front-end screens (architecture §7, v0.43): the main menu,
// the settings screen, the button row of the match-over screens and the
// menu presses, all headless.
#include "core/menus.hpp"

#include "core/client_options.hpp"
#include "core/lobby_menu.hpp"
#include "core/lobby_settings.hpp"
#include "kfm/platform/input_event.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

using namespace kfm;
using client::ButtonMenu;
using client::LobbySettings;
using client::MainMenu;
using client::MainMenuItem;
using client::MenuAction;
using client::PauseItem;
using client::SettingsMenu;
using client::SettingsRow;

TEST_CASE("main menu: Play online, Play local, Settings, Quit natively", "[client][menu]") {
  MainMenu m;
  REQUIRE(m.items().size() == 4);
  CHECK(m.items()[0] == MainMenuItem::PlayOnline);
  CHECK(m.items()[3] == MainMenuItem::Quit);
  CHECK(m.focusedItem() == MainMenuItem::PlayOnline);
  CHECK(m.activate() == MainMenuItem::PlayOnline);
  m.apply(MenuAction::Down);
  CHECK(m.activate() == MainMenuItem::PlayLocal);
  m.apply(MenuAction::Right);  // Left/Right do nothing
  CHECK(m.focusedItem() == MainMenuItem::PlayLocal);
  m.apply(MenuAction::Down);
  m.apply(MenuAction::Down);
  CHECK(m.focusedItem() == MainMenuItem::Quit);
  m.apply(MenuAction::Down);  // wraps
  CHECK(m.focusedItem() == MainMenuItem::PlayOnline);
  m.apply(MenuAction::Up);  // wraps back
  CHECK(m.focusedItem() == MainMenuItem::Quit);
  CHECK(std::string(MainMenu::label(MainMenuItem::Settings)) == "Settings");

  // The browser has no Quit.
  const MainMenu web(false);
  REQUIRE(web.items().size() == 3);
  CHECK(web.items()[2] == MainMenuItem::Settings);
}

TEST_CASE("main menu: without a way online, Play online is greyed and skipped", "[client][menu]") {
  MainMenu m;
  m.setOnlineAvailable(false);
  CHECK_FALSE(m.enabled(MainMenuItem::PlayOnline));
  CHECK(m.enabled(MainMenuItem::PlayLocal));
  CHECK(m.focusedItem() == MainMenuItem::PlayLocal);  // moved off the disabled item
  m.apply(MenuAction::Up);
  CHECK(m.focusedItem() == MainMenuItem::Quit);  // skipped Play online
  m.apply(MenuAction::Down);
  CHECK(m.focusedItem() == MainMenuItem::PlayLocal);
  m.setFocus(0);  // the mouse on the greyed item: ignored
  CHECK(m.focusedItem() == MainMenuItem::PlayLocal);
  CHECK(std::string(m.hint(MainMenuItem::PlayOnline)).find("--lobby") != std::string::npos);
  m.setOnlineAvailable(true);
  m.setFocus(0);
  CHECK(m.activate() == MainMenuItem::PlayOnline);
}

TEST_CASE("settings menu: name, volume, sound, bot difficulty, theme", "[client][menu][settings]") {
  SettingsMenu m;
  LobbySettings ls;
  ls.volume = 80;
  std::string nick = "Bonobo 42";
  REQUIRE(SettingsMenu::rows().size() == 6);
  CHECK(m.focusedRow() == SettingsRow::Name);
  CHECK(SettingsMenu::rowValue(SettingsRow::Name, ls, nick) == "Bonobo 42");
  CHECK_FALSE(m.apply(MenuAction::Right, ls));  // the name is typed, not cycled

  m.apply(MenuAction::Down, ls);
  REQUIRE(m.focusedRow() == SettingsRow::Volume);
  CHECK(m.apply(MenuAction::Right, ls));
  CHECK(ls.volume == 90);
  CHECK(m.apply(MenuAction::Right, ls));
  CHECK_FALSE(m.apply(MenuAction::Right, ls));  // clamped at 100
  CHECK(SettingsMenu::rowValue(SettingsRow::Volume, ls, nick) == "100%");

  m.apply(MenuAction::Down, ls);
  REQUIRE(m.focusedRow() == SettingsRow::Sound);
  CHECK(m.apply(MenuAction::Left, ls));
  CHECK(ls.mute);
  CHECK(SettingsMenu::rowValue(SettingsRow::Sound, ls, nick) == "muted");

  m.apply(MenuAction::Down, ls);
  REQUIRE(m.focusedRow() == SettingsRow::BotDifficulty);
  ls.botTier = ai::ReferenceTier::Hard;
  CHECK(m.apply(MenuAction::Right, ls));  // wraps
  CHECK(ls.botTier == ai::ReferenceTier::Easy);
  CHECK(m.apply(MenuAction::Left, ls));
  CHECK(ls.botTier == ai::ReferenceTier::Hard);

  m.apply(MenuAction::Down, ls);  // v0.52
  REQUIRE(m.focusedRow() == SettingsRow::Theme);
  CHECK(SettingsMenu::rowValue(SettingsRow::Theme, ls, nick) == "auto");
  CHECK(m.apply(MenuAction::Left, ls));  // wraps
  CHECK(ls.theme == kfm::presentation::ThemeChoice::Beach);
  CHECK(SettingsMenu::rowValue(SettingsRow::Theme, ls, nick) == "beach");

  m.apply(MenuAction::Down, ls);
  CHECK(m.focusedRow() == SettingsRow::Back);
  m.apply(MenuAction::Down, ls);  // wraps
  CHECK(m.focusedRow() == SettingsRow::Name);

  // Names are sanitised like the wire's; an empty one keeps the old name.
  CHECK(SettingsMenu::setNick(nick, "  Grumpy   Gibbon \t"));
  CHECK(nick == "Grumpy Gibbon");
  CHECK_FALSE(SettingsMenu::setNick(nick, "Grumpy Gibbon"));  // unchanged
  CHECK_FALSE(SettingsMenu::setNick(nick, "   "));
  CHECK(nick == "Grumpy Gibbon");
  CHECK(SettingsMenu::setNick(nick, "A name far longer than sixteen bytes"));
  CHECK(nick.size() <= 16);
}

TEST_CASE("button row: any direction moves the focus, wrapping", "[client][menu]") {
  ButtonMenu b;
  b.setCount(2);
  CHECK(b.focus() == 0);
  b.apply(MenuAction::Right);
  CHECK(b.focus() == 1);
  b.apply(MenuAction::Down);
  CHECK(b.focus() == 0);
  b.apply(MenuAction::Left);
  CHECK(b.focus() == 1);
  b.setCount(1);  // the focus follows a shorter row
  CHECK(b.focus() == 0);
  b.apply(MenuAction::Up);
  CHECK(b.focus() == 0);
  b.setFocus(5);
  CHECK(b.focus() == 0);
}

TEST_CASE("menu presses: activate and back, from any device", "[client][menu][input]") {
  platform::InputEvent e;
  e.kind = platform::InputEventKind::KeyDown;
  for (const platform::Key k :
       {platform::Key::Enter, platform::Key::KpEnter, platform::Key::Space}) {
    e.key = k;
    CHECK(client::isMenuActivate(e));
  }
  e.key = platform::Key::J;
  CHECK_FALSE(client::isMenuActivate(e));
  e.kind = platform::InputEventKind::KeyUp;
  e.key = platform::Key::Enter;
  CHECK_FALSE(client::isMenuActivate(e));  // presses only
  e.kind = platform::InputEventKind::PadButtonDown;
  e.deviceId = 7;  // any pad: nothing joins on the menu screens
  e.button = platform::PadButton::South;
  CHECK(client::isMenuActivate(e));
  CHECK_FALSE(client::isMenuBack(e));
  e.button = platform::PadButton::Start;
  CHECK(client::isMenuActivate(e));
  e.button = platform::PadButton::East;
  CHECK(client::isMenuBack(e));
  CHECK_FALSE(client::isMenuActivate(e));
  e.button = platform::PadButton::Back;
  CHECK(client::isMenuBack(e));
}

TEST_CASE("the room host's menu has only the rows a RoomSettings carries (v0.43)",
          "[client][menu][lobby]") {
  client::LobbyMenu m;
  const std::size_t all = m.rows().size();
  m.setRoomRows(true);
  CHECK(m.rows().size() == all - 4);
  for (const client::LobbyRow& r : m.rows()) {
    CHECK(r.kind != client::LobbyRowKind::BotDifficulty);
    CHECK(r.kind != client::LobbyRowKind::Volume);
    CHECK(r.kind != client::LobbyRowKind::Mute);
    CHECK(r.kind != client::LobbyRowKind::Theme);  // v0.52: personal, not the room's
  }
  CHECK(m.rows().back().kind == client::LobbyRowKind::Bots);
  // Navigation stays on those rows: Up from the first wraps to Bots.
  m.setFocus(0);
  CHECK_FALSE(m.apply(MenuAction::Up));
  CHECK(m.focusedRow().kind == client::LobbyRowKind::Bots);
  m.setRoomRows(false);
  CHECK(m.rows().size() == all);
}

TEST_CASE("--screen opens a front-end screen (v0.43)", "[client][menu][options]") {
  const auto parse = [](std::vector<const char*> argv, client::ClientOptions& o, std::string& e) {
    argv.insert(argv.begin(), "kfm_client");
    return client::parseClientOptions(std::span<const char* const>(argv.data(), argv.size()), o, e);
  };
  client::ClientOptions o;
  std::string error;
  CHECK(o.screen == "menu");
  REQUIRE(parse({"--screen", "settings"}, o, error));
  CHECK(o.screen == "settings");
  REQUIRE(parse({"--screen", "local"}, o, error));
  CHECK(o.screen == "local");
  REQUIRE(parse({"--screen", "paused"}, o, error));  // v0.46
  CHECK(o.screen == "paused");
  client::ClientOptions bad;
  CHECK_FALSE(parse({"--screen", "credits"}, bad, error));
  CHECK(error.find("--screen") != std::string::npos);
  CHECK_FALSE(parse({"--screen"}, bad, error));
}

TEST_CASE("pause menu: the local and online buttons, focus on Resume (v0.46)", "[client][menu]") {
  client::PauseMenu m;
  CHECK_FALSE(m.isOpen());
  m.open(false);
  REQUIRE(m.isOpen());
  CHECK_FALSE(m.online());
  REQUIRE(m.items().size() == 3);
  CHECK(m.items()[0] == PauseItem::Resume);
  CHECK(m.items()[1] == PauseItem::BackToLobby);
  CHECK(m.items()[2] == PauseItem::MainMenu);
  CHECK(m.focusedItem() == PauseItem::Resume);
  m.apply(MenuAction::Down);
  m.apply(MenuAction::Down);
  CHECK(m.focusedItem() == PauseItem::MainMenu);
  m.apply(MenuAction::Down);  // wraps
  CHECK(m.focusedItem() == PauseItem::Resume);
  m.apply(MenuAction::Up);
  CHECK(m.focusedItem() == PauseItem::MainMenu);
  CHECK(std::string(client::PauseMenu::label(PauseItem::BackToLobby)) == "Back to lobby");

  // Reopened online: two buttons, the focus back on Resume (clamped from 2).
  m.close();
  CHECK_FALSE(m.isOpen());
  m.open(true);
  CHECK(m.online());
  REQUIRE(m.items().size() == 2);
  CHECK(m.items()[0] == PauseItem::Resume);
  CHECK(m.items()[1] == PauseItem::LeaveRoom);
  CHECK(m.focusedItem() == PauseItem::Resume);
  m.setFocus(1);
  CHECK(m.focusedItem() == PauseItem::LeaveRoom);
  m.setFocus(2);  // out of range: ignored
  CHECK(m.focusedItem() == PauseItem::LeaveRoom);
  CHECK(std::string(client::PauseMenu::label(PauseItem::LeaveRoom)) == "Leave room");
}

TEST_CASE("pause menu: what opens it, what resumes, what picks (v0.46)", "[client][menu][input]") {
  using platform::InputEvent;
  using platform::InputEventKind;
  using platform::Key;
  using platform::PadButton;
  const auto keyDown = [](Key k) {
    InputEvent e;
    e.kind = InputEventKind::KeyDown;
    e.key = k;
    return e;
  };
  const auto pad = [](PadButton b, InputEventKind kind = InputEventKind::PadButtonDown) {
    InputEvent e;
    e.kind = kind;
    e.deviceId = 3;
    e.button = b;
    return e;
  };

  // Escape, or a bound pad's Start (an unbound pad's press takes over a slot).
  CHECK(client::isPauseEvent(keyDown(Key::Escape), true));
  CHECK_FALSE(client::isPauseEvent(keyDown(Key::Enter), true));
  CHECK(client::isPauseEvent(pad(PadButton::Start), true));
  CHECK_FALSE(client::isPauseEvent(pad(PadButton::Start), false));
  CHECK_FALSE(client::isPauseEvent(pad(PadButton::Start, InputEventKind::PadButtonUp), true));
  CHECK_FALSE(client::isPauseEvent(pad(PadButton::South), true));

  client::PauseMenu m;
  CHECK_FALSE(m.press(keyDown(Key::Enter)));  // closed: nothing
  for (const bool online : {false, true}) {
    m.open(online);
    m.apply(MenuAction::Down);  // off Resume
    REQUIRE(m.focusedItem() != PauseItem::Resume);
    // Every resume path, whatever the focus.
    CHECK(m.press(keyDown(Key::Escape)) == PauseItem::Resume);
    CHECK(m.press(pad(PadButton::East)) == PauseItem::Resume);
    CHECK(m.press(pad(PadButton::Back)) == PauseItem::Resume);
    CHECK(m.press(pad(PadButton::Start)) == PauseItem::Resume);
    // Activation picks the focused item.
    const PauseItem focused = m.focusedItem();
    CHECK(m.press(keyDown(Key::Enter)) == focused);
    CHECK(m.press(keyDown(Key::KpEnter)) == focused);
    CHECK(m.press(keyDown(Key::Space)) == focused);
    CHECK(m.press(pad(PadButton::South)) == focused);
    // Others do nothing: a game key, a release, navigation.
    CHECK_FALSE(m.press(keyDown(Key::J)));
    CHECK_FALSE(m.press(pad(PadButton::South, InputEventKind::PadButtonUp)));
    CHECK_FALSE(m.press(keyDown(Key::Down)));
    m.setFocus(0);
    CHECK(m.press(keyDown(Key::Enter)) == PauseItem::Resume);
  }
  CHECK(m.focusedItem() == PauseItem::Resume);
}
