// kfm_client command line (headless-testable).
//
//   kfm_client [--autostart] [--bots N] [--seed S] [--players-seed S]
//              [--bot-difficulty easy|medium|hard] [--bot-module PATH] [--bot SPEC]
//              [--content DIR] [--arena NAME] [--mode NAME] [--loadout NAME]
//              [--powerups LIST] [--no-hot-reload] [--no-settings]
//              [--overlay] [--capsules] [--exit-after-frames N] [--screenshot out.ppm]
//              [--mute] [--volume 0..100]
//              [--connect ws://host:port [--token T] [--spectate] [--stun URL] [--players N]]
//              [--lobby URL] [--room CODE | --create [--region R]] [--nick NAME]
//              [--screen menu|settings|online|local|paused]
//
// v0.43: --screen opens the client on that front-end screen instead of the
// main menu (screenshots and development; --autostart and the network flags
// still go straight to play). v0.46: `paused` starts a local match and opens
// the pause menu over it.
//
// v0.42: --nick (the browser's ?nick=) is the name sent in Hello and shown to
// the room; saved with the lobby settings (`nick=`); with none, the client
// picks a random monkey name once and saves it (randomMonkeyName()).
//
// M6a (architecture §13.5, docs/lobby-protocol.md §3): --room / --create join
// or create a room through the lobby at --lobby (default: the build's
// KFM_LOBBY_URL, empty unless the build sets it) and are network mode like
// --connect, which they exclude; --token is then the lobby's, not the flag's.
#pragma once

#include "kfm/ai/bot_spec.hpp"
#include "match_session.hpp"
#include "net_session.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace kfm::client {

// The regions the lobby panel cycles through (docs/lobby-protocol.md §2:
// operator labels; --region takes any other too).
inline constexpr std::array<const char*, 3> kRegions{"eu", "us", "asia"};

// The build's default lobby URL (the KFM_LOBBY_URL CMake cache variable);
// empty when the build sets none.
[[nodiscard]] const char* defaultLobbyUrl();
// v0.43: the client's web page as the build knows it (the KFM_PAGE_URL
// CMake cache variable), for a native client's invite links; usually empty.
[[nodiscard]] const char* defaultPageUrl();

inline constexpr const char* kUsage =
    "usage: kfm_client [--autostart] [--bots N] [--seed S] [--players-seed S]\n"
    "                  [--bot-difficulty easy|medium|hard] [--bot-module PATH] [--bot SPEC]\n"
    "                  [--content DIR] [--arena NAME] [--mode NAME] [--loadout NAME]\n"
    "                  [--powerups LIST] [--no-hot-reload] [--no-settings]\n"
    "                  [--overlay] [--capsules] [--exit-after-frames N] [--screenshot out.ppm]\n"
    "                  [--mute] [--volume 0..100]\n"
    "                  [--connect ws://host:port [--token T] [--spectate] [--stun URL]\n"
    "                   [--players N]]\n"
    "                  [--lobby URL] [--room CODE | --create [--region R]] [--nick NAME]\n"
    "                  [--screen menu|settings|online|local|paused]\n"
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
    "  --verbose              online: the transport's own log (ICE, DTLS, the lobby socket)\n"
    "  --volume N             master volume in percent, 0-100 (default 80)\n"
    "network mode:\n"
    "  --connect ws://h:p     play on a server: connects at startup; Esc goes to the\n"
    "                         local lobby, whose Start reconnects with every device\n"
    "  --token T              connection token sent to the server\n"
    "  --spectate             watch without a slot\n"
    "  --stun URL             STUN server (stun:host:port) for ICE\n"
    "  --players N            local slots to request (default: 1 at startup, then\n"
    "                         the devices bound in the lobby when connecting)\n"
    "rooms (through the lobby service; not with --connect):\n"
    "  --lobby URL            the lobby's base URL, ws:// or wss:// (default: the\n"
    "                         build's KFM_LOBBY_URL, if any); the lobby panel then\n"
    "                         offers Create room and Join\n"
    "  --room CODE            join room CODE (5 characters) at startup\n"
    "  --create               create a room at startup and join it\n"
    "  --region R             the region of --create and of the panel's Create\n"
    "                         room: eu (default), us, asia or any server region\n"
    "  --nick NAME            your name online, at most 16 bytes (saved; default: a\n"
    "                         random monkey name picked once)\n"
    "  --screen S             open on this screen instead of the main menu: menu,\n"
    "                         settings, online (Play online), local (the lobby) or\n"
    "                         paused (a local match under the pause menu)\n";

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
  bool verbose{false};  // network mode: libdatachannel's log at Debug on stderr
  uint8_t volume{80};   // percent, = presentation::kDefaultMasterVolume
  // Network mode (empty connect: local play).
  std::string connect;  // signaling URL, "ws://host:port"
  std::string token;
  std::string stun;  // "stun:host:port"
  bool spectate{false};
  uint8_t players{0};  // local slots to request; 0 = the bound devices
  // Rooms (M6a): the lobby's base URL, a room to join or a room to create.
  std::string lobby{defaultLobbyUrl()};
  std::string room;  // --room, normalised upper case; empty = none
  bool create{false};
  std::string region{kRegions[0]};  // --region: --create's and the panel's region
  std::string nick;                 // v0.42: --nick, valid (net::validNickname); empty = none yet
  std::string screen{"menu"};       // v0.43: --screen, the first front-end screen

  // --connect, --room or --create: the client plays online from startup and
  // the lobby's Start (re)connects.
  [[nodiscard]] bool networkMode() const { return !connect.empty() || lobbyMode(); }
  // --room or --create (with a lobby URL, which parsing ensures).
  [[nodiscard]] bool lobbyMode() const { return !room.empty() || create; }
};

// The room the options name (--room or --create), or none.
[[nodiscard]] std::optional<LobbyTarget> lobbyTarget(const ClientOptions& options);

// A room code as the lobby sends it: 5 characters of
// ABCDEFGHJKLMNPQRSTUVWXYZ23456789, any case in, upper case out
// (docs/lobby-protocol.md §2). Anything else is nullopt.
[[nodiscard]] std::optional<std::string> normalizeRoomCode(std::string_view code);
[[nodiscard]] bool isRoomCodeChar(char c);
// A region label: 1-16 characters of a-z, 0-9, '-'.
[[nodiscard]] bool isRegionLabel(std::string_view region);
// What a player should send a friend to join `room`: in the browser
// (`pageUrl` = window.location.href) the page's link with ?room=CODE (and
// the page's own ?lobby= when it had one), natively the flags
// "--lobby URL --room CODE".
[[nodiscard]] std::string roomShareText(const LobbyRoomInfo& room, std::string_view pageUrl,
                                        std::string_view lobbyUrl);
// What Copy link copies and Share offers (v0.43): with a page URL (the
// browser's own, natively defaultPageUrl()) the same link as
// roomShareText(), else the room's code alone.
[[nodiscard]] std::string roomInviteLink(const LobbyRoomInfo& room, std::string_view pageUrl);

// A monkey name for a player who gave none (v0.42): one of a fixed list of
// monkey species picked by `seed`, with a number 1..99 ("Bonobo 42"); always
// a valid nickname.
[[nodiscard]] std::string randomMonkeyName(uint64_t seed);

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
