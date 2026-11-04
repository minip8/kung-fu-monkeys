// kfm_client command line (headless-testable).
//
//   kfm_client [--autostart] [--bots N] [--seed S] [--players-seed S]
//              [--bot-difficulty easy|medium|hard] [--bot-module PATH] [--bot SPEC]
//              [--content DIR] [--arena NAME] [--mode NAME] [--loadout NAME]
//              [--powerups LIST] [--no-hot-reload] [--no-settings]
//              [--overlay] [--capsules] [--exit-after-frames N] [--screenshot out.ppm]
//              [--mute] [--volume 0..100]
//              [--connect ws://host:port [--token T] [--spectate] [--stun URL] [--players N]]
#pragma once

#include "kfm/ai/bot_spec.hpp"
#include "match_session.hpp"
#include "net_session.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace kfm::client {

inline constexpr const char* kUsage =
    "usage: kfm_client [--autostart] [--bots N] [--seed S] [--players-seed S]\n"
    "                  [--bot-difficulty easy|medium|hard] [--bot-module PATH] [--bot SPEC]\n"
    "                  [--content DIR] [--arena NAME] [--mode NAME] [--loadout NAME]\n"
    "                  [--powerups LIST] [--no-hot-reload] [--no-settings]\n"
    "                  [--overlay] [--capsules] [--exit-after-frames N] [--screenshot out.ppm]\n"
    "                  [--mute] [--volume 0..100]\n"
    "                  [--connect ws://host:port [--token T] [--spectate] [--stun URL]\n"
    "                   [--players N]]\n"
    "  --autostart            skip the lobby: start the match on the first frame\n"
    "  --bots N               bots filling the slots after the local players (0-9)\n"
    "  --seed S               world seed (MatchConfig::seed)\n"
    "  --players-seed S       seed of the bots (each slot's agent seed derives from it)\n"
    "  --bot-difficulty T     the reference bot's tier: easy, medium (default) or hard\n"
    "  --bot-module PATH      run bots from this .wasm bot module (ABI v1; native only).\n"
    "                         One that cannot be loaded falls back to the reference bot\n"
    "  --bot SPEC             easy|medium|hard, ref, ref:TIER or PATH.wasm: the two\n"
    "                         flags above in one (the browser's ?bot=easy)\n"
    "  --content DIR          content directory (*.toml); default: content/ next to the\n"
    "                         executable, else the copy built into it\n"
    "  --arena NAME           arena of local matches (default: the first one)\n"
    "  --mode NAME            mode of local matches (default: the first one)\n"
    "  --loadout NAME         loadout of local matches: base (default, the player's\n"
    "                         kit) or one of content/loadouts.toml (blink, slam, ...)\n"
    "  --powerups LIST        power-ups the spawners of local matches may draw: all\n"
    "                         (default), none or comma-separated names\n"
    "  --no-hot-reload        do not watch the content directory (native local play\n"
    "                         reloads it on change and restarts the match)\n"
    "  --no-settings          neither read nor save the lobby settings (arena, mode,\n"
    "                         loadout, power-ups, bots, bot difficulty, volume, mute;\n"
    "                         flags win over saved ones)\n"
    "  --overlay              open the debug overlay (F1) on startup\n"
    "  --capsules             draw players as capsules, not the skinned model\n"
    "  --exit-after-frames N  render N frames and exit 0 (smoke test)\n"
    "  --screenshot path      write the last frame as binary PPM (native only)\n"
    "  --mute                 start with sound off (F1 overlay, Audio, turns it on)\n"
    "  --volume N             master volume in percent, 0-100 (default 80)\n"
    "network mode:\n"
    "  --connect ws://h:p     play on a server: connects at startup; Esc goes to the\n"
    "                         local lobby, whose Start reconnects with every device\n"
    "  --token T              connection token sent to the server\n"
    "  --spectate             watch without a slot\n"
    "  --stun URL             STUN server (stun:host:port) for ICE\n"
    "  --players N            local slots to request (default: 1 at startup, then\n"
    "                         the devices bound in the lobby when connecting)\n";

struct ClientOptions {
  uint64_t exitAfterFrames{0};  // 0: run until quit
  std::string screenshotPath;   // empty: none
  bool autostart{false};
  bool overlay{false};
  bool capsules{false};  // capsule placeholders instead of the skinned character model
  uint8_t bots{0};
  uint64_t seed{1};
  uint64_t playersSeed{kDefaultBotSeed};  // the bots' seed (SessionConfig::botSeed)
  ai::ReferenceTier botDifficulty{kDefaultBotTier};
  std::string botModule;   // --bot-module; empty = the reference bot
  std::string contentDir;  // --content; empty = next to the executable, else embedded
  std::string arena;       // --arena / --mode / --loadout names; empty = index 0
  std::string mode;
  std::string loadout;
  std::string powerUps;  // --powerups list; empty = all
  bool hotReload{true};
  bool settings{true};  // read and save the lobby settings (lobby_settings.hpp)
  // Audio (§7): the mixer's starting settings; the overlay can change them.
  bool mute{false};
  uint8_t volume{80};  // percent, = presentation::kDefaultMasterVolume
  // Network mode (empty connect: local play).
  std::string connect;  // signaling URL, "ws://host:port"
  std::string token;
  std::string stun;  // "stun:host:port"
  bool spectate{false};
  uint8_t players{0};  // local slots to request; 0 = the bound devices

  [[nodiscard]] bool networkMode() const { return !connect.empty(); }
};

// The bots of local play: the module when one is given (falling back to
// the reference bot at botDifficulty), else the reference bot at it.
[[nodiscard]] ai::BotSpec botSpec(const ClientOptions& options);

// The NetSession settings of the network flags. `atStartup` is the connect
// at launch, which requests one slot unless --players says otherwise; a
// connect from the lobby requests one per bound device by default.
[[nodiscard]] NetSessionConfig netSessionConfig(const ClientOptions& options, bool atStartup);

// Parses argv[1..]. On failure returns false and sets `error`. Numbers accept
// decimal or 0x-prefixed hex.
[[nodiscard]] bool parseClientOptions(std::span<const char* const> args, ClientOptions& out,
                                      std::string& error);

}  // namespace kfm::client
