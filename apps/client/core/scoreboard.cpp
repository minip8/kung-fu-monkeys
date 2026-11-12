#include "scoreboard.hpp"

#include "kfm/sim/content/mode.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace kfm::client {

namespace {

using presentation::HudPhase;

[[nodiscard]] const char* scoreLabelOf(sim::ModeKind kind) {
  switch (kind) {
    case sim::ModeKind::Deathmatch:
    case sim::ModeKind::TeamDeathmatch:
      // v0.57: eliminations plus catch points less suicides, not kills alone.
      return "Score";
    case sim::ModeKind::Hill:
    case sim::ModeKind::Control:  // v0.80: the zones' points
      return "Points";
    case sim::ModeKind::Capture:
      return "Flags";
    case sim::ModeKind::Rounds:
    case sim::ModeKind::Survival:
    case sim::ModeKind::TeamRounds:
    case sim::ModeKind::Harvest:  // v0.81: a minimal case until the client draws the kind
    case sim::ModeKind::kCount:
      break;
  }
  return "Rounds";
}

// The kinds whose MatchRules::deaths count (deathmatch's respawns, the
// objective kinds' and survival's eliminations).
[[nodiscard]] bool countsDeaths(sim::ModeKind kind) {
  return sim::scoresPoints(kind) || kind == sim::ModeKind::Survival;
}

}  // namespace

std::string slotLabel(std::string_view name, SlotOwner owner, uint8_t slot) {
  if (!name.empty()) return std::string(name);
  return (owner == SlotOwner::Bot ? "BOT" : "P") + std::to_string(static_cast<unsigned>(slot) + 1U);
}

bool showsScoreboard(const presentation::RenderHud& hud) {
  switch (hud.phase) {
    case HudPhase::MatchEnded:
      return true;
    case HudPhase::RoundEnded:
      return !sim::scoresPoints(hud.mode);
    case HudPhase::RoundStarting:  // the countdown shows only its banner and count
    case HudPhase::Lobby:
    case HudPhase::Playing:
      break;
  }
  return false;
}

Scoreboard buildScoreboard(const presentation::RenderHud& hud,
                           const std::array<std::string, sim::kMaxPlayers>& labels,
                           sim::PlayerMask local, sim::PlayerMask bots) {
  Scoreboard b;
  b.scoreLabel = scoreLabelOf(hud.mode);
  b.showDeaths = countsDeaths(hud.mode);
  b.teamCount = hud.teamCount;
  b.teamScores = hud.teamScores;
  b.final = hud.phase == HudPhase::MatchEnded;
  const bool teams = hud.teamCount > 0;
  const bool decided =
      b.final && hud.matchWinner >= 0 && std::cmp_less(hud.matchWinner, sim::kMaxPlayers);
  const auto winner = static_cast<uint8_t>(decided ? hud.matchWinner : 0);
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if ((hud.presentMask & (1u << i)) == 0) continue;
    const PlayerId slot{i};
    ScoreRow r;
    r.slot = i;
    r.label = labels[i];
    r.team = teams ? static_cast<int8_t>(hud.teams[i]) : int8_t{-1};
    // An int8_t score is a number, not a character (v0.57: signed scores).
    r.score = hud.scores[i];  // NOLINT(bugprone-signed-char-misuse)
    r.deaths = b.showDeaths ? hud.deaths[i] : uint8_t{0};
    r.local = sim::maskHas(local, slot);
    r.bot = sim::maskHas(bots, slot);
    r.winner = decided && (teams ? hud.teams[i] == hud.teams[winner] : i == winner);
    b.rows.push_back(std::move(r));
  }
  std::ranges::sort(b.rows, [&](const ScoreRow& a, const ScoreRow& c) {
    if (teams && a.team != c.team) return a.team < c.team;
    if (a.score != c.score) return a.score > c.score;
    if (a.deaths != c.deaths) return a.deaths < c.deaths;
    return a.slot < c.slot;
  });
  // Places: within a team in a team mode, over everyone otherwise.
  for (std::size_t k = 0; k < b.rows.size(); ++k) {
    ScoreRow& r = b.rows[k];
    if (k == 0 || (teams && b.rows[k - 1].team != r.team)) {
      r.place = 1;
    } else {
      const ScoreRow& prev = b.rows[k - 1];
      const bool tie = prev.score == r.score && prev.deaths == r.deaths;
      // The place counts the rows ahead of this one in its group.
      std::size_t ahead = 0;
      for (std::size_t j = 0; j < k; ++j) {
        if (!teams || b.rows[j].team == r.team) ++ahead;
      }
      r.place = tie ? prev.place : static_cast<uint8_t>(ahead + 1);
    }
  }
  return b;
}

}  // namespace kfm::client
