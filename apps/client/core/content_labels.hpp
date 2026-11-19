// What players read for the content's names (architecture §7, v0.94): the
// lobby, the room, the HUD and the replay bar show "King of the Hill", not
// `hill`, with a line on how to win. SDL- and ImGui-free; tested in
// tests/unit/client/content_labels_test.cpp.
//
// A table names the shipped modes, arenas, loadouts and power-ups; a name
// it does not know (new TOML content) is prettified instead ("kill_rounds"
// -> "Kill Rounds", "big_banana_1t" -> "Big Banana (1-tick)"), so new
// content never shows up broken or raw.
//
// Bot-contest content is not offered in a human lobby: a mode on the grid
// with one-tick turns (the ladder's `contest`, `control_1t`,
// `grid_rounds_1t`) is for bots, and so is a loadout only such a mode hands
// out by role (`grid_1t`, `catcher_1t`). The lobby's rows skip them unless
// the match already uses one (a --mode flag still selects it).
#pragma once

#include "kfm/sim/content.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace kfm::client {

// "team_deathmatch" -> "Team Deathmatch"; a trailing "_1t" reads
// " (1-tick)"; empty stays empty.
[[nodiscard]] std::string prettyContentName(std::string_view name);

struct ModeLabel {
  std::string title;  // "King of the Hill"
  std::string blurb;  // "Stand alone on the hill to score for your team."
};
// The mode's title and one line of how to win (empty for a mode the table
// does not know: the lobby's modeSummary() line still gives its numbers).
[[nodiscard]] ModeLabel modeLabel(std::string_view name);
[[nodiscard]] std::string arenaTitle(std::string_view name);
[[nodiscard]] std::string loadoutTitle(std::string_view name);
[[nodiscard]] std::string powerUpTitle(std::string_view name);

// Bot-contest content (see the header comment).
[[nodiscard]] bool isBotMode(const sim::ModeDef& mode);
[[nodiscard]] bool isBotLoadout(const sim::ContentDB& db, uint8_t loadout);
// v0.97: a kit some mode hands out by role (ModeDef::roles: `grid`,
// `catcher` and the bot contests' `_1t` twins): the lobby's Kit row, which
// picks the kit every monkey plays, skips it.
[[nodiscard]] bool isRoleLoadout(const sim::ContentDB& db, uint8_t loadout);

// Which power-ups an arena's spawners can draw (trees aside), as a
// ContentDB::powerUps bit set: 0 when the arena has no power-up spawner.
[[nodiscard]] uint16_t arenaPowerUps(const sim::ContentDB& db, const sim::ArenaDef& arena);

}  // namespace kfm::client
