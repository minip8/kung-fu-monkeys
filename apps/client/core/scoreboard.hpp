// The scoreboard as a view model (architecture §7, v0.43, M6b step 3): a pure
// function from the HUD's view of the match (presentation::RenderHud, built
// from MatchRules and the ModeDef) and the slots' names to sorted rows, so
// the between-rounds scoreboard and the match-over screen are tested
// headlessly and hud.cpp only draws.
//
// Shown between rounds in the round kinds (rounds, survival, team rounds:
// RoundEnded, and RoundStarting after the first round) and at the match's
// end in every mode (showsScoreboard()). A row is a present slot: its label
// (the nickname online, else P<n> / BOT<n>), its team, its score (round
// wins, eliminations, hill points or captures by mode kind), its deaths
// where the mode counts them (deathmatch, team deathmatch, hill, capture,
// survival), whether it is a local player or a bot, and whether it won.
// Free-for-all rows sort by score (desc), deaths (asc), slot; team rows by
// team first. Places are shared on a tie of score and deaths.
#pragma once

#include "kfm/presentation/render_state.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/input.hpp"
#include "match_session.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kfm::client {

// A slot as the HUD names it: `name` when it has one, else "BOT<n>" for a
// bot and "P<n>" otherwise (n = slot + 1).
[[nodiscard]] std::string slotLabel(std::string_view name, SlotOwner owner, uint8_t slot);

struct ScoreRow {
  uint8_t slot{0};
  std::string label;
  int8_t team{-1};  // -1: free-for-all
  uint16_t score{0};
  uint8_t deaths{0};
  uint8_t place{1};  // 1-based, shared on a tie
  bool local{false};
  bool bot{false};
  bool winner{false};  // the match's winner (or on its winning team)
  bool operator==(const ScoreRow&) const = default;
};

struct Scoreboard {
  std::vector<ScoreRow> rows;
  const char* scoreLabel{"Rounds"};  // the score column: Rounds, Kills, Points, Flags
  bool showDeaths{false};
  uint8_t teamCount{0};
  std::array<uint16_t, sim::kMaxPlayers> teamScores{};
  bool final{false};  // the match is over
};

// Whether the HUD shows the scoreboard now (see the header comment).
[[nodiscard]] bool showsScoreboard(const presentation::RenderHud& hud);

// `labels`: each slot's slotLabel(); `local` / `bots`: the slots this
// client drives and the slots bots drive.
[[nodiscard]] Scoreboard buildScoreboard(const presentation::RenderHud& hud,
                                         const std::array<std::string, sim::kMaxPlayers>& labels,
                                         sim::PlayerMask local, sim::PlayerMask bots);

}  // namespace kfm::client
