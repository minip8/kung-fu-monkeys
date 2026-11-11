// The HUD's and the match-over screen's view models (core/hud_view.hpp,
// architecture §7): hudView() of a local match, the "you" rows online, the
// winner's title and colour, the clock's last seconds, the match-over
// screen's typed buttons locally and in an online room, the lost-connection
// screen's typed buttons, and what the room's slots show (the server's bot
// placement).
#include "core/hud_view.hpp"

#include "core/match_session.hpp"
#include "kfm/net/messages.hpp"
#include "kfm/presentation/render_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace kfm;
using client::HudView;
using client::LostConnectionAction;
using client::MatchOverAction;
using client::SlotOwner;
using presentation::HudPhase;
using presentation::RenderHud;

namespace {

// Three free-for-all slots: P1 local, slot 1 another client's, slot 2 a bot.
[[nodiscard]] HudView threeSlots(bool network) {
  HudView v;
  v.network = network;
  v.owners[0] = SlotOwner::Local;
  v.owners[1] = SlotOwner::Remote;
  v.owners[2] = SlotOwner::Bot;
  v.bots = 0b100;
  return v;
}

[[nodiscard]] RenderHud matchEnded(int8_t winner) {
  RenderHud hud;
  hud.phase = HudPhase::MatchEnded;
  hud.presentMask = 0b111;
  hud.matchWinner = winner;
  return hud;
}

}  // namespace

TEST_CASE("hud view: a local match's owners, bots and pause", "[client][hud]") {
  const client::MatchSession s(client::SessionConfig{.seed = 7, .bots = 2});
  const HudView v = client::hudView(s);
  CHECK(v.owners[0] == SlotOwner::Local);
  CHECK(std::popcount(static_cast<uint32_t>(v.bots)) == 2);
  CHECK((v.bots & 1u) == 0);
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    CHECK(((v.bots >> i) & 1u) == (v.owners[i] == SlotOwner::Bot ? 1u : 0u));
  }
  CHECK_FALSE(v.network);
  CHECK_FALSE(v.replay);
  CHECK_FALSE(v.paused);
  // Local play marks nobody "you": several players share the screen.
  CHECK(client::onlineLocalMask(v) == 0);
  const auto labels = client::slotLabels(v);
  CHECK(labels[0] == "P1");
}

TEST_CASE("hud view: online, this client's slots (dropped ones too) are \"you\"", "[client][hud]") {
  HudView v = threeSlots(true);
  v.owners[3] = SlotOwner::Dropped;
  CHECK(client::onlineLocalMask(v) == 0b1001);
  CHECK(client::onlineLocalMask(threeSlots(false)) == 0);
}

TEST_CASE("hud view: the winner's title and colour", "[client][hud]") {
  HudView v = threeSlots(true);
  v.names[1] = "Bonobo 42";
  RenderHud hud;
  CHECK(client::winnerTitle(v, hud, 0) == client::WinnerTitle{.text = "P1", .colorIndex = 0});
  CHECK(client::winnerTitle(v, hud, 1) ==
        client::WinnerTitle{.text = "Bonobo 42", .colorIndex = 1});
  CHECK(client::winnerTitle(v, hud, 2) == client::WinnerTitle{.text = "BOT3", .colorIndex = 2});
  // A team mode names the winner's team, in the team's colour.
  hud.teamCount = 2;
  hud.teams = {0, 1, 1};
  CHECK(client::winnerTitle(v, hud, 2) == client::WinnerTitle{.text = "Team 2", .colorIndex = 1});
  CHECK(client::winnerTitle(v, hud, 0) == client::WinnerTitle{.text = "Team 1", .colorIndex = 0});
}

TEST_CASE("hud view: the clock is urgent in the last ten seconds of play", "[client][hud]") {
  RenderHud hud;
  hud.hasClock = true;
  hud.phase = HudPhase::Playing;
  hud.clockSeconds = client::kClockUrgentSeconds + 0.01f;
  CHECK_FALSE(client::clockUrgent(hud));
  hud.clockSeconds = client::kClockUrgentSeconds;
  CHECK(client::clockUrgent(hud));
  hud.clockSeconds = 0.0f;
  CHECK(client::clockUrgent(hud));
  // Not while the round starts or after it ended.
  hud.phase = HudPhase::RoundStarting;
  CHECK_FALSE(client::clockUrgent(hud));
  hud.phase = HudPhase::RoundEnded;
  CHECK_FALSE(client::clockUrgent(hud));
}

TEST_CASE("match over: title, colour and the scoreboard", "[client][hud][over]") {
  const HudView v = threeSlots(true);
  const client::MatchOverView draw = client::matchOverView(v, matchEnded(presentation::kNoSlot));
  CHECK(draw.title == "Draw!");
  CHECK_FALSE(draw.colorIndex.has_value());
  CHECK(draw.actions.empty());
  CHECK(draw.board.final);
  CHECK(draw.board.rows.size() == 3);

  const client::MatchOverView won = client::matchOverView(v, matchEnded(2));
  CHECK(won.title == "BOT3 wins the match!");
  CHECK(won.colorIndex == std::optional<uint8_t>{2});
  // The scoreboard marks this client's slot and the bot.
  for (const client::ScoreRow& row : won.board.rows) {
    CHECK(row.local == (row.slot == 0));
    CHECK(row.bot == (row.slot == 2));
  }
}

TEST_CASE("match over: typed buttons locally and in an online room", "[client][hud][over]") {
  const HudView v = threeSlots(false);
  const client::MatchOverView local = client::localMatchOverView(v, matchEnded(0));
  CHECK(local.actions ==
        std::vector<MatchOverAction>{MatchOverAction::PlayAgain, MatchOverAction::BackToLobby});
  CHECK(local.waiting.empty());
  CHECK(local.actionAt(0) == MatchOverAction::PlayAgain);
  CHECK(local.actionAt(1) == MatchOverAction::BackToLobby);
  CHECK_FALSE(local.actionAt(2).has_value());
  // onEvent's Enter on the focused button reads the same list.
  CHECK(client::matchOverActionAt(client::localMatchOverActions(), 0) ==
        MatchOverAction::PlayAgain);
  CHECK(client::matchOverActionAt(client::localMatchOverActions(), 1) ==
        MatchOverAction::BackToLobby);
  CHECK_FALSE(client::matchOverActionAt(client::localMatchOverActions(), 2).has_value());

  const HudView online = threeSlots(true);
  const client::MatchOverView host =
      client::roomMatchOverView(online, matchEnded(0), true, "Bonobo 42");
  CHECK(host.actions ==
        std::vector<MatchOverAction>{MatchOverAction::PlayAgain, MatchOverAction::Leave});
  CHECK(host.waiting.empty());
  const client::MatchOverView guest =
      client::roomMatchOverView(online, matchEnded(0), false, "Bonobo 42");
  CHECK(guest.actions == std::vector<MatchOverAction>{MatchOverAction::Leave});
  CHECK(guest.actionAt(0) == MatchOverAction::Leave);
  CHECK(guest.waiting == "Waiting for Bonobo 42 to play again");
  CHECK(client::roomMatchOverView(online, matchEnded(0), false, "").waiting ==
        "Waiting for the host to play again");

  CHECK(std::string(client::matchOverLabel(MatchOverAction::PlayAgain)) == "Play again");
  CHECK(std::string(client::matchOverLabel(MatchOverAction::BackToLobby)) == "Back to lobby");
  CHECK(std::string(client::matchOverLabel(MatchOverAction::Leave)) == "Leave");
}

TEST_CASE("lost connection: Reconnect and Leave only while the session can reconnect",
          "[client][hud][lost]") {
  const std::span<const LostConnectionAction> lost = client::lostConnectionActions(true);
  CHECK(std::vector<LostConnectionAction>(lost.begin(), lost.end()) ==
        std::vector<LostConnectionAction>{LostConnectionAction::Reconnect,
                                          LostConnectionAction::Leave});
  // netFrame's Enter on the focused button reads the same list.
  CHECK(client::lostConnectionActionAt(lost, 0) == LostConnectionAction::Reconnect);
  CHECK(client::lostConnectionActionAt(lost, 1) == LostConnectionAction::Leave);
  CHECK_FALSE(client::lostConnectionActionAt(lost, 2).has_value());

  // Connecting, rejected or failed: no buttons (Esc goes back to the menu),
  // so Enter picks nothing whatever the focus.
  const std::span<const LostConnectionAction> none = client::lostConnectionActions(false);
  CHECK(none.empty());
  CHECK_FALSE(client::lostConnectionActionAt(none, 0).has_value());
  CHECK_FALSE(client::lostConnectionActionAt(none, 1).has_value());

  CHECK(std::string(client::lostConnectionLabel(LostConnectionAction::Reconnect)) == "Reconnect");
  CHECK(std::string(client::lostConnectionLabel(LostConnectionAction::Leave)) == "Leave");
}

// The waiting screen shows the server's placement as it is: the client has
// no rule of its own for where the bots go (issue #68; the rule and its
// cases are the server's, tests/unit/server/room_flow_test.cpp).
TEST_CASE("room: the slots show the humans, the bots and the server's placement",
          "[client][hud][room]") {
  using client::RoomSlot;
  const RoomSlot o = RoomSlot::Open;  // short names: the rows read as the screen
  const RoomSlot h = RoomSlot::Human;
  const RoomSlot b = RoomSlot::Bot;
  const RoomSlot s = RoomSlot::BotAtStart;
  using Slots = std::array<RoomSlot, sim::kMaxPlayers>;

  net::RoomStateMsg room;
  CHECK(client::roomSlots(room) == Slots{o, o, o, o, o, o, o, o, o, o});  // nobody

  // Waiting: the bots to come are exactly botPlacement, whatever the bot count.
  room.humans = 0b11;
  room.settings.bots = 3;
  room.botPlacement = 0b1100;  // the server's cap left room for two
  CHECK(client::roomSlots(room) == Slots{h, h, s, s, o, o, o, o, o, o});
  room.botPlacement = 0b10'0000'0100;  // not the lowest: shown as sent
  CHECK(client::roomSlots(room) == Slots{h, h, s, o, o, o, o, o, o, s});
  room.botPlacement = 0;  // no room for a bot, or none asked for
  CHECK(client::roomSlots(room) == Slots{h, h, o, o, o, o, o, o, o, o});

  // Playing and Over: the bots hold their slots (botPlacement == bots).
  room.phase = net::RoomPhase::Playing;
  room.bots = 0b0101'0000;
  room.botPlacement = room.bots;
  CHECK(client::roomSlots(room) == Slots{h, h, o, o, b, o, b, o, o, o});
}
