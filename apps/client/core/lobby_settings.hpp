// What the local lobby's settings panel edits, and how it is remembered
// between runs (architecture §7, v0.24). SDL-free and headless-tested.
//
// LobbySettings holds indices into the loaded content; ClientOptions holds
// the same settings as names (the --arena / --mode / --loadout / --powerups
// flags and the browser's ?arena=&mode=&loadout=&powerups=), so they survive
// a content change
// that reorders or removes an entry. The settings text is the persisted
// form: one "key=value" per line,
//
//   arena=pillars
//   mode=deathmatch
//   loadout=slam                    (v0.31; base = the player's kit)
//   powerups=shield,big_banana      (all, none or comma-separated names)
//   bots=3
//   bot_difficulty=medium           (easy, medium or hard: the reference bot's tier)
//   volume=80
//   mute=0
//   region=eu                       (M6a: the Online section's Create room region)
//   nick=Bonobo 42                  (v0.42: the name online; absent until there is one)
//
// stored natively in SDL's per-user preference directory and in the
// browser's localStorage (main.cpp). Precedence at startup: defaults, then
// the saved text, then the command line (or URL), so a flag always wins for
// that run; changing a setting in the lobby saves the whole set again.
#pragma once

#include "client_options.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/world_state.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kfm::client {

struct LobbySettings {
  uint8_t arena{0};  // MatchConfig::arena / ::mode / ::powerUps / ::loadout
  uint8_t mode{0};
  uint16_t powerUps{sim::kAllPowerUps};
  uint8_t loadout{0};
  uint8_t bots{0};
  ai::ReferenceTier botTier{kDefaultBotTier};
  uint8_t volume{80};  // percent
  bool mute{false};
  std::string region{"eu"};  // M6a: the Online section's region label (the lobby lists them)
  constexpr bool operator==(const LobbySettings&) const = default;
};

// Resolves the options' names against the content. An unknown arena, mode,
// loadout or power-up is an error (it came from a flag: applySettingsText() never
// stores one). An arena that does not suit the mode (v0.32,
// ContentDB::compatible) is replaced by the first one that does.
[[nodiscard]] bool lobbySettingsFrom(const ClientOptions& options, const sim::ContentDB& content,
                                     LobbySettings& out, std::string& error);

// Writes the settings back into the options as names ("all"/"none" for the
// power-up set), the form settingsText() persists.
void storeLobbySettings(const LobbySettings& settings, const sim::ContentDB& content,
                        ClientOptions& options);

// The persisted settings of `options`, one "key=value" line each.
[[nodiscard]] std::string settingsText(const ClientOptions& options);

// Applies saved settings text to `options`. Lines that cannot apply (an
// unknown key, a malformed number, a name this content does not define) are
// skipped with a message in `warnings`; '#' starts a comment line. Never
// fails: a stale settings file must not stop the client.
void applySettingsText(std::string_view text, const sim::ContentDB& content, ClientOptions& options,
                       std::vector<std::string>& warnings);

}  // namespace kfm::client
