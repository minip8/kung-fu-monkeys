// The HUD's and the match-over screen's view models (core/hud_view.hpp,
// architecture §7): hudView() of a local match, the "you" rows online, the
// winner's title and colour, the clock's last seconds, the match-over
// screen's typed buttons locally and in an online room, and where the
// room's bots join at the start.
#include "core/hud_view.hpp"

#include "core/match_session.hpp"
#include "kfm/net/messages.hpp"
#include "kfm/presentation/render_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace kfm;
using client::HudView;
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

// The server places a room's bots in the lowest free slots at Start
// (apps/server ServerHost::placeRoomBots); the waiting screen previews the
// same slots. The cases mirror tests/unit/server/room_flow_test.cpp's.
TEST_CASE("room: the bots join in the lowest slots no human holds", "[client][hud][room]") {
  net::RoomStateMsg room;
  room.humans = 0b11;
  room.settings.bots = 2;
  CHECK(client::roomBotPreview(room) == 0b1100);
  room.humans = 0b10;
  CHECK(client::roomBotPreview(room) == 0b101);
  room.humans = 0b1;
  room.settings.bots = 3;
  CHECK(client::roomBotPreview(room) == 0b1110);
  room.settings.bots = 0;
  CHECK(client::roomBotPreview(room) == 0);
  // More bots than slots: every slot no human holds.
  room.settings.bots = 20;
  CHECK(client::roomBotPreview(room) == static_cast<sim::PlayerMask>(0x3FEu));
  // Once the server placed them there is nothing left to preview.
  room.settings.bots = 2;
  room.bots = 0b110;
  CHECK(client::roomBotPreview(room) == 0);
}
