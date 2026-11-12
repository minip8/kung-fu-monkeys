// The scoreboard view model (architecture §7, v0.43): when it shows, its
// rows' order, places, columns per mode kind, team grouping and the local /
// bot / winner marks, from a RenderHud alone.
#include "core/scoreboard.hpp"

#include "core/match_session.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/sim/constants.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>

using namespace kfm;
using client::buildScoreboard;
using client::Scoreboard;
using client::showsScoreboard;
using presentation::HudPhase;
using presentation::RenderHud;

namespace {

[[nodiscard]] std::array<std::string, sim::kMaxPlayers> labels() {
  std::array<std::string, sim::kMaxPlayers> l;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i)
    l[i] = "S" + std::to_string(i);
  return l;
}

}  // namespace

TEST_CASE("slot labels: the name, else BOT<n> for a bot, else P<n>", "[client][scoreboard]") {
  CHECK(client::slotLabel("Bonobo 42", client::SlotOwner::Remote, 3) == "Bonobo 42");
  CHECK(client::slotLabel("", client::SlotOwner::Bot, 3) == "BOT4");
  CHECK(client::slotLabel("", client::SlotOwner::Local, 0) == "P1");
  CHECK(client::slotLabel("", client::SlotOwner::Remote, 9) == "P10");
}

TEST_CASE("the scoreboard shows after a round in round kinds and at every match end",
          "[client][scoreboard]") {
  RenderHud hud;
  hud.mode = sim::ModeKind::Rounds;
  hud.round = 1;
  hud.phase = HudPhase::Lobby;
  CHECK_FALSE(showsScoreboard(hud));
  hud.phase = HudPhase::RoundStarting;
  CHECK_FALSE(showsScoreboard(hud));  // the first round: nothing to show yet
  hud.phase = HudPhase::Playing;
  CHECK_FALSE(showsScoreboard(hud));
  hud.phase = HudPhase::RoundEnded;
  CHECK(showsScoreboard(hud));
  // The next round's countdown has the screen to itself (the round banner
  // and the count): the board is gone once RoundStarting begins.
  hud.round = 2;
  hud.phase = HudPhase::RoundStarting;
  CHECK_FALSE(showsScoreboard(hud));
  hud.mode = sim::ModeKind::TeamRounds;
  CHECK_FALSE(showsScoreboard(hud));
  hud.phase = HudPhase::RoundEnded;
  CHECK(showsScoreboard(hud));
  hud.mode = sim::ModeKind::Rounds;
  hud.phase = HudPhase::MatchEnded;
  CHECK(showsScoreboard(hud));
  // Deathmatch plays one long round: only its end.
  hud.mode = sim::ModeKind::Deathmatch;
  hud.phase = HudPhase::RoundStarting;
  CHECK_FALSE(showsScoreboard(hud));
  hud.phase = HudPhase::MatchEnded;
  CHECK(showsScoreboard(hud));
  hud.mode = sim::ModeKind::Survival;
  hud.phase = HudPhase::RoundEnded;
  CHECK(showsScoreboard(hud));
}

TEST_CASE("free-for-all rows: score, then deaths, then slot; shared places",
          "[client][scoreboard]") {
  RenderHud hud;
  hud.mode = sim::ModeKind::Deathmatch;
  hud.phase = HudPhase::MatchEnded;
  hud.presentMask = 0b1011'0111;  // slots 0, 1, 2, 4, 5, 7
  hud.scores = {3, 7, 3, 0, 1, 7, 0, 3};
  hud.deaths = {2, 4, 1, 0, 5, 4, 0, 2};
  hud.matchWinner = 1;
  const Scoreboard b = buildScoreboard(hud, labels(), /*local*/ 0b0000'0100, /*bots*/ 0b1000'0000);
  CHECK(std::string(b.scoreLabel) == "Score");  // v0.57: kills plus catches less suicides
  CHECK(b.showDeaths);
  CHECK(b.final);
  REQUIRE(b.rows.size() == 6);
  // 1 and 5 tie on 7 kills and 4 deaths: shared first place, slot order.
  CHECK(b.rows[0].slot == 1);
  CHECK(b.rows[1].slot == 5);
  CHECK(b.rows[0].place == 1);
  CHECK(b.rows[1].place == 1);
  // Then the 3-kill slots by deaths: 2 (1), then 0 and 7 (2 each, tied).
  CHECK(b.rows[2].slot == 2);
  CHECK(b.rows[2].place == 3);
  CHECK(b.rows[3].slot == 0);
  CHECK(b.rows[4].slot == 7);
  CHECK(b.rows[3].place == 4);
  CHECK(b.rows[4].place == 4);
  CHECK(b.rows[5].slot == 4);
  CHECK(b.rows[5].place == 6);
  CHECK(b.rows[0].winner);
  CHECK_FALSE(b.rows[1].winner);  // the sim's winner, not every tied slot
  CHECK(b.rows[2].local);
  CHECK(b.rows[2].label == "S2");
  CHECK(b.rows[4].bot);
  CHECK(b.rows[0].score == 7);
  CHECK(b.rows[0].deaths == 4);
}

TEST_CASE("negative scores sort below zero, the team score is the sum", "[client][scoreboard]") {
  // v0.57: a suicide costs a point (suicide_score), so a score may be < 0.
  RenderHud hud;
  hud.mode = sim::ModeKind::TeamDeathmatch;
  hud.phase = HudPhase::MatchEnded;
  hud.presentMask = 0b1111;
  hud.teamCount = 2;
  hud.teams = {0, 1, 0, 1};
  hud.scores = {-2, 3, 1, -1};
  hud.teamScores = {-1, 2};
  hud.matchWinner = 1;
  const Scoreboard b = buildScoreboard(hud, labels(), 0, 0);
  CHECK(std::string(b.scoreLabel) == "Score");
  REQUIRE(b.rows.size() == 4);
  CHECK(b.rows[0].slot == 2);  // team 0: 1 above -2
  CHECK(b.rows[1].slot == 0);
  CHECK(b.rows[1].score == -2);
  CHECK(b.rows[2].slot == 1);  // team 1: 3 above -1
  CHECK(b.rows[3].score == -1);
  CHECK(b.teamScores[0] == -1);
  CHECK(b.rows[2].winner);
  CHECK(b.rows[3].winner);
}

TEST_CASE("round kinds count rounds and no deaths; nobody wins mid-match", "[client][scoreboard]") {
  RenderHud hud;
  hud.mode = sim::ModeKind::Rounds;
  hud.phase = HudPhase::RoundEnded;
  hud.presentMask = 0b0011;
  hud.scores = {1, 2};
  hud.deaths = {9, 9};  // not counted by the kind: hidden
  hud.matchWinner = 1;  // stale: only MatchEnded names a winner
  const Scoreboard b = buildScoreboard(hud, labels(), 0, 0);
  CHECK(std::string(b.scoreLabel) == "Rounds");
  CHECK_FALSE(b.showDeaths);
  CHECK_FALSE(b.final);
  REQUIRE(b.rows.size() == 2);
  CHECK(b.rows[0].slot == 1);
  CHECK(b.rows[0].deaths == 0);
  CHECK_FALSE(b.rows[0].winner);

  hud.mode = sim::ModeKind::Hill;
  CHECK(std::string(buildScoreboard(hud, labels(), 0, 0).scoreLabel) == "Points");
  hud.mode = sim::ModeKind::Capture;
  CHECK(std::string(buildScoreboard(hud, labels(), 0, 0).scoreLabel) == "Flags");
  hud.mode = sim::ModeKind::Control;  // v0.80
  CHECK(std::string(buildScoreboard(hud, labels(), 0, 0).scoreLabel) == "Points");
  CHECK(buildScoreboard(hud, labels(), 0, 0).showDeaths);
  hud.mode = sim::ModeKind::Survival;
  CHECK(buildScoreboard(hud, labels(), 0, 0).showDeaths);
}

TEST_CASE("team rows group by team; the winning team's rows win", "[client][scoreboard]") {
  RenderHud hud;
  hud.mode = sim::ModeKind::TeamDeathmatch;
  hud.phase = HudPhase::MatchEnded;
  hud.presentMask = 0b1111;
  hud.teamCount = 2;
  hud.teams = {1, 0, 1, 0};
  hud.scores = {5, 2, 1, 4};
  hud.teamScores = {6, 6};
  hud.matchWinner = 0;  // a slot of team 1 (index 1)
  const Scoreboard b = buildScoreboard(hud, labels(), 0, 0);
  CHECK(b.teamCount == 2);
  CHECK(b.teamScores[0] == 6);
  REQUIRE(b.rows.size() == 4);
  // Team 0 (slots 3, 1 by score), then team 1 (slots 0, 2).
  CHECK(b.rows[0].slot == 3);
  CHECK(b.rows[1].slot == 1);
  CHECK(b.rows[2].slot == 0);
  CHECK(b.rows[3].slot == 2);
  CHECK(b.rows[0].team == 0);
  CHECK(b.rows[2].team == 1);
  // Places restart per team.
  CHECK(b.rows[0].place == 1);
  CHECK(b.rows[1].place == 2);
  CHECK(b.rows[2].place == 1);
  CHECK(b.rows[3].place == 2);
  CHECK_FALSE(b.rows[0].winner);
  CHECK(b.rows[2].winner);
  CHECK(b.rows[3].winner);

  // A draw (no winner) marks nobody.
  hud.matchWinner = presentation::kNoSlot;
  for (const client::ScoreRow& r : buildScoreboard(hud, labels(), 0, 0).rows)
    CHECK_FALSE(r.winner);
}
