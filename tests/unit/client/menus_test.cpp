// The client's front-end screens (architecture §7, v0.43): the main menu,
// the settings screen, the button row of the match-over screens and the
// menu presses, all headless.
#include "core/menus.hpp"

#include "core/client_options.hpp"
#include "core/lobby_menu.hpp"
#include "core/lobby_settings.hpp"
#include "core/menu_highlight.hpp"
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

TEST_CASE("main menu: Play online, Play local, How to play, Settings, Quit natively",
          "[client][menu]") {
  MainMenu m;
  REQUIRE(m.items().size() == 5);
  CHECK(m.items()[0] == MainMenuItem::PlayOnline);
  CHECK(m.items()[2] == MainMenuItem::HowToPlay);  // v0.94
  CHECK(m.items()[4] == MainMenuItem::Quit);
  CHECK(m.focusedItem() == MainMenuItem::PlayOnline);
  CHECK(m.activate() == MainMenuItem::PlayOnline);
  m.apply(MenuAction::Down);
  CHECK(m.activate() == MainMenuItem::PlayLocal);
  m.apply(MenuAction::Right);  // Left/Right do nothing
  CHECK(m.focusedItem() == MainMenuItem::PlayLocal);
  m.apply(MenuAction::Down);
  CHECK(m.activate() == MainMenuItem::HowToPlay);
  CHECK(std::string(MainMenu::label(MainMenuItem::HowToPlay)) == "How to play");
  CHECK_FALSE(std::string(m.hint(MainMenuItem::HowToPlay)).empty());
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
  REQUIRE(web.items().size() == 4);
  CHECK(web.items()[3] == MainMenuItem::Settings);
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

TEST_CASE("settings menu: name, volume, sound, theme, mouse aim (v0.97: no bot difficulty)",
          "[client][menu][settings]") {
  SettingsMenu m;
  LobbySettings ls;
  ls.volume = 80;
  std::string nick = "Bonobo 42";
  REQUIRE(SettingsMenu::rows().size() == 6);
  for (const SettingsRow r : SettingsMenu::rows())
    CHECK(std::string(SettingsMenu::rowLabel(r)) != "Bot difficulty");  // the setup panel's
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

  m.apply(MenuAction::Down, ls);  // v0.52
  REQUIRE(m.focusedRow() == SettingsRow::Theme);
  CHECK(SettingsMenu::rowValue(SettingsRow::Theme, ls, nick) == "beach");  // v0.93's default
  CHECK(m.apply(MenuAction::Right, ls));                                   // wraps
  CHECK(ls.theme == kfm::presentation::ThemeChoice::Auto);
  CHECK(SettingsMenu::rowValue(SettingsRow::Theme, ls, nick) == "auto (per arena)");
  CHECK(m.apply(MenuAction::Left, ls));
  CHECK(ls.theme == kfm::presentation::ThemeChoice::Beach);

  m.apply(MenuAction::Down, ls);  // mouse aim: on by default, either arrow toggles
  REQUIRE(m.focusedRow() == SettingsRow::MouseAim);
  CHECK(SettingsMenu::rowLabel(SettingsRow::MouseAim) == std::string("Mouse aim"));
  CHECK(ls.mouseAim);
  CHECK(SettingsMenu::rowValue(SettingsRow::MouseAim, ls, nick) == "on");
  CHECK(m.apply(MenuAction::Right, ls));
  CHECK_FALSE(ls.mouseAim);
  CHECK(SettingsMenu::rowValue(SettingsRow::MouseAim, ls, nick) == "off");
  CHECK(m.apply(MenuAction::Left, ls));
  CHECK(ls.mouseAim);
  // The mouse's value arrows go through change() with the row index.
  CHECK(SettingsMenu::change(m.focus(), -1, ls));
  CHECK_FALSE(ls.mouseAim);

  m.apply(MenuAction::Down, ls);
  CHECK(m.focusedRow() == SettingsRow::Back);
  CHECK(std::string(SettingsMenu::rowLabel(SettingsRow::Back)) == "Done");
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

TEST_CASE("settings overlay: opens over a screen, closes back, Enter acts (v0.97)",
          "[client][menu][settings]") {
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
  const auto pad = [](PadButton b) {
    InputEvent e;
    e.kind = InputEventKind::PadButtonDown;
    e.deviceId = 2;
    e.button = b;
    return e;
  };
  SettingsMenu m;
  LobbySettings ls;
  CHECK_FALSE(m.isOpen());
  m.setFocus(3);
  m.open();
  CHECK(m.isOpen());
  CHECK(m.focus() == 0);  // opens on the first row

  using Press = SettingsMenu::Press;
  CHECK(SettingsMenu::press(keyDown(Key::Escape)) == Press::Close);
  CHECK(SettingsMenu::press(pad(PadButton::East)) == Press::Close);
  CHECK(SettingsMenu::press(pad(PadButton::Back)) == Press::Close);
  CHECK(SettingsMenu::press(keyDown(Key::Enter)) == Press::Activate);
  CHECK(SettingsMenu::press(keyDown(Key::Space)) == Press::Activate);
  CHECK(SettingsMenu::press(pad(PadButton::South)) == Press::Activate);
  CHECK(SettingsMenu::press(keyDown(Key::Down)) == Press::None);
  CHECK(SettingsMenu::press(keyDown(Key::J)) == Press::None);

  using Activation = SettingsMenu::Activation;
  CHECK(m.activate(ls) == Activation::EditName);  // the name field takes the keyboard
  m.setFocus(2);
  REQUIRE(m.focusedRow() == SettingsRow::Sound);
  CHECK(m.activate(ls) == Activation::Changed);  // as Right: toggles
  CHECK(ls.mute);
  m.setFocus(1);
  ls.volume = 100;
  CHECK(m.activate(ls) == Activation::None);  // already at the top
  m.setFocus(SettingsMenu::rows().size() - 1);
  CHECK(m.activate(ls) == Activation::Closed);  // Done
  CHECK_FALSE(m.isOpen());

  // Tab, or the Back (View) of a pad that navigates there, opens it from the
  // lobby or the waiting room; nothing else does.
  CHECK(client::isSettingsEvent(keyDown(Key::Tab), true));
  CHECK(client::isSettingsEvent(pad(PadButton::Back), true));
  CHECK_FALSE(client::isSettingsEvent(pad(PadButton::Back), false));  // an unbound pad joins
  CHECK_FALSE(client::isSettingsEvent(keyDown(Key::Escape), true));
  CHECK_FALSE(client::isSettingsEvent(pad(PadButton::East), true));
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

TEST_CASE("the room host's menu: the same rows, Bot difficulty the server's (v0.97)",
          "[client][menu][lobby]") {
  client::LobbyMenu m;
  const std::size_t all = client::LobbyMenu::rows().size();
  m.setRoomRows(true);
  CHECK(client::LobbyMenu::rows().size() == all);  // v0.97: fixed (v0.43 dropped rows)
  CHECK_FALSE(m.rowEnabled(client::LobbyRowKind::BotDifficulty));
  // Navigation skips it: Up from the first wraps to Bots.
  m.setFocus(0);
  CHECK_FALSE(m.apply(MenuAction::Up));
  CHECK(m.focusedRow() == client::LobbyRowKind::Bots);
  // Focused when the room's panel came: back on the first row.
  client::LobbyMenu focused;
  focused.setFocus(client::LobbyMenu::rows().size() - 1);
  focused.setRoomRows(true);
  CHECK(focused.focus() == 0);
  m.setRoomRows(false);
  CHECK(m.rowEnabled(client::LobbyRowKind::BotDifficulty));
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
  for (const char* s : {"howto", "powerups", "local-settings", "paused-settings"}) {  // v0.97
    REQUIRE(parse({"--screen", s}, o, error));
    CHECK(o.screen == s);
  }
  client::ClientOptions bad;
  CHECK_FALSE(parse({"--screen", "credits"}, bad, error));
  CHECK(error.find("--screen") != std::string::npos);
  CHECK_FALSE(parse({"--screen"}, bad, error));
}

TEST_CASE("--window sets the native window's size (v0.94)", "[client][options]") {
  const auto parse = [](std::vector<const char*> argv, client::ClientOptions& o, std::string& e) {
    argv.insert(argv.begin(), "kfm_client");
    return client::parseClientOptions(std::span<const char* const>(argv.data(), argv.size()), o, e);
  };
  client::ClientOptions o;
  std::string error;
  CHECK(o.windowWidth == 1280);
  CHECK(o.windowHeight == 720);
  REQUIRE(parse({"--window", "700x900"}, o, error));
  CHECK(o.windowWidth == 700);
  CHECK(o.windowHeight == 900);
  for (const char* bad : {"700", "700x", "x900", "100x900", "700x99999", "axb"}) {
    client::ClientOptions b;
    CHECK_FALSE(parse({"--window", bad}, b, error));
    CHECK(error.find("--window") != std::string::npos);
  }
  client::ClientOptions b;
  CHECK_FALSE(parse({"--window"}, b, error));
}

TEST_CASE("pause menu: the local and online buttons, focus on Resume (v0.46)", "[client][menu]") {
  using client::PauseContext;
  client::PauseMenu m;
  CHECK_FALSE(m.isOpen());
  m.open(PauseContext{.online = false});
  REQUIRE(m.isOpen());
  CHECK_FALSE(m.online());
  // v0.97: Settings and Restart match join.
  REQUIRE(m.items().size() == 6);
  CHECK(m.items()[0] == PauseItem::Resume);
  CHECK(m.items()[1] == PauseItem::Settings);
  CHECK(m.items()[2] == PauseItem::HowToPlay);  // v0.94
  CHECK(m.items()[3] == PauseItem::Restart);
  CHECK(m.items()[4] == PauseItem::BackToLobby);
  CHECK(m.items()[5] == PauseItem::MainMenu);
  CHECK(std::string(client::PauseMenu::label(PauseItem::Settings)) == "Settings");
  CHECK(std::string(client::PauseMenu::label(PauseItem::Restart)) == "Restart match");
  CHECK(m.focusedItem() == PauseItem::Resume);
  for (int i = 0; i < 5; ++i)
    m.apply(MenuAction::Down);
  CHECK(m.focusedItem() == PauseItem::MainMenu);
  m.apply(MenuAction::Down);  // wraps
  CHECK(m.focusedItem() == PauseItem::Resume);
  m.apply(MenuAction::Up);
  CHECK(m.focusedItem() == PauseItem::MainMenu);
  CHECK(std::string(client::PauseMenu::label(PauseItem::BackToLobby)) == "Back to lobby");

  // Reopened online: four buttons, the focus back on Resume (clamped from 5).
  m.close();
  CHECK_FALSE(m.isOpen());
  m.open(PauseContext{.online = true});
  CHECK(m.online());
  REQUIRE(m.items().size() == 4);
  CHECK(m.items()[0] == PauseItem::Resume);
  CHECK(m.items()[1] == PauseItem::Settings);
  CHECK(m.items()[2] == PauseItem::HowToPlay);
  CHECK(m.items()[3] == PauseItem::LeaveRoom);
  CHECK(m.focusedItem() == PauseItem::Resume);
  // The host's menu is the same until NetSession can end or restart a match.
  CHECK(client::pauseItems(PauseContext{.online = true, .host = true}).count == 4);
  CHECK(m.context().online);
  m.setFocus(3);
  CHECK(m.focusedItem() == PauseItem::LeaveRoom);
  m.setFocus(4);  // out of range: ignored
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
    m.open(client::PauseContext{.online = online});
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

  // v0.94: How to play shows the controls in the menu's place; every back
  // or pick press then closes the card (HowToPlay), never resumes.
  m.open(client::PauseContext{});
  m.setFocus(2);
  REQUIRE(m.press(keyDown(Key::Enter)) == PauseItem::HowToPlay);
  m.setHelp(true);
  CHECK(m.helpOpen());
  CHECK(m.press(keyDown(Key::Escape)) == PauseItem::HowToPlay);
  CHECK(m.press(pad(PadButton::East)) == PauseItem::HowToPlay);
  CHECK(m.press(pad(PadButton::Start)) == PauseItem::HowToPlay);
  CHECK(m.press(keyDown(Key::Enter)) == PauseItem::HowToPlay);
  CHECK_FALSE(m.press(keyDown(Key::J)));
  m.setHelp(false);
  CHECK_FALSE(m.helpOpen());
  CHECK(m.press(keyDown(Key::Escape)) == PauseItem::Resume);
  m.setHelp(true);
  m.close();  // closing drops the card too
  CHECK_FALSE(m.helpOpen());
  m.setHelp(true);  // a closed menu has no card
  CHECK_FALSE(m.helpOpen());
}

// v0.55: the highlight follows the last menu device. main.cpp feeds every
// event to onEvent() and asks reveal() before applying a menu press; the
// widgets draw shows(). This mirrors that wiring over a MainMenu.
TEST_CASE("menu highlight: the mouse shows only the hovered item, a key reveals the focus",
          "[client][menu]") {
  using client::MenuHighlight;
  using platform::InputEvent;
  using platform::InputEventKind;
  MenuHighlight h;
  MainMenu m;
  const auto event = [](InputEventKind kind) {
    InputEvent e;
    e.kind = kind;
    return e;
  };
  // A press as main.cpp routes it: dropped when it only reveals.
  bool activated = false;
  const auto press = [&](MenuAction action, bool select, bool pointerOnFocus) {
    if (h.reveal(pointerOnFocus)) return;
    m.apply(action);
    if (select) activated = m.activate().has_value();
  };

  // Startup: pointer mode, nothing under the pointer, nothing highlighted
  // (the focus on Play online is not drawn).
  CHECK(h.mode() == MenuHighlight::Mode::Pointer);
  CHECK_FALSE(h.shows(true, false));
  CHECK(h.shows(false, true));  // the hovered item is

  // Enter with nothing hovered does not activate the invisible focus: it
  // reveals it.
  press(MenuAction::None, true, false);
  CHECK_FALSE(activated);
  CHECK(h.keys());
  CHECK(h.shows(true, false));
  CHECK_FALSE(h.shows(false, true));  // a resting pointer is not a highlight now
  CHECK(m.focusedItem() == MainMenuItem::PlayOnline);

  // Keys mode: presses act.
  press(MenuAction::Down, false, false);
  CHECK(m.focusedItem() == MainMenuItem::PlayLocal);

  // Mouse motion: back to the pointer rule, the focus hidden again.
  h.onEvent(event(InputEventKind::MouseMotion));
  CHECK_FALSE(h.keys());
  CHECK_FALSE(h.shows(true, false));

  // The first arrow reveals without moving.
  press(MenuAction::Down, false, false);
  CHECK(h.keys());
  CHECK(m.focusedItem() == MainMenuItem::PlayLocal);

  // A click is the pointer too; releases, key ups and pads do not change
  // the mode (only menu presses through reveal() do).
  h.onEvent(event(InputEventKind::MouseButtonDown));
  CHECK_FALSE(h.keys());
  h.onEvent(event(InputEventKind::KeyDown));
  h.onEvent(event(InputEventKind::PadAxis));
  h.onEvent(event(InputEventKind::MouseButtonUp));
  CHECK_FALSE(h.keys());

  // With the pointer over the focused item (it took the focus), the focus
  // was visible already: Enter activates the hovered item at once.
  activated = false;
  press(MenuAction::None, true, true);
  CHECK(activated);
  CHECK(h.keys());
}

TEST_CASE("menu highlight: a touch counts only while the finger is down", "[client][menu]") {
  using client::MenuHighlight;
  CHECK(MenuHighlight::pointerOver(true, false, false));  // a mouse at rest over it
  CHECK_FALSE(MenuHighlight::pointerOver(false, false, true));
  CHECK(MenuHighlight::pointerOver(true, true, true));         // a finger on it
  CHECK_FALSE(MenuHighlight::pointerOver(true, true, false));  // where a tap ended
}
