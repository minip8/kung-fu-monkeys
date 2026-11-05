// kfm_client: the playable client (architecture §3, §6.8, §7, §11 M2, M3).
//
//   kfm_client [--autostart] [--bots N] [--seed S] [--players-seed S]
//              [--bot-difficulty easy|medium|hard] [--bot-module PATH] [--bot SPEC]
//              [--content DIR] [--arena NAME] [--mode NAME] [--loadout NAME]
//              [--powerups LIST] [--no-hot-reload] [--no-settings] [--overlay] [--capsules]
//              [--exit-after-frames N]
//              [--screenshot out.ppm]
//              [--mute] [--volume 0..100]
//              [--connect ws://host:port [--token T] [--spectate] [--stun URL] [--players N]]
//              [--lobby URL] [--room CODE | --create [--region R]]
//
// Frame: pollEvents (ImGui first, then SDL -> InputEvent into the session)
// -> MatchSession::advance (0..4 fixed 60 Hz ticks, inputs sampled right
// before each step) or, in network mode, NetSession::advance (pump,
// reconcile, onRollback, the rate-controlled ticks, journal events) ->
// buildRenderState -> audio (the frame's sound cues into the mixer, the
// device queue topped up) -> arena camera -> HUD / lobby / network status /
// debug overlay (ImGui) -> debugUiFlush -> default pass: renderer.draw, then
// debugUiDraw -> present.
//
// Network mode (--connect): connects at startup with one local slot (or
// --players N), else one per device bound in the local lobby when Play
// online connects later; Escape disconnects and returns to the menu. The
// session runs over a WebRtcTransport, natively and in the browser alike.
//
// Rooms (M6a, architecture §13.5): --room CODE / --create join or create a
// room through the lobby (--lobby, else the build's KFM_LOBBY_URL; in the
// browser ?room=CODE, ?create&region=us and config.js's lobbyUrl), as do
// the Play online screen's Create room and Join (v0.43; the local lobby's
// Online section until then). The transport runs in lobby mode, the session
// takes the room's token before its Hello, and the HUD names the room with
// an invite (the page's link with ?room=CODE in the browser, the flags
// natively). Leaving a room keeps its code in the Play online screen's
// field, so Join rejoins it. IPlatformServices (LocalPlatformServices) keeps
// a guest id beside the settings (nothing sends it yet) and, v0.43, copies
// and shares the room's link.
//
// The online room (v0.42, architecture §6.6): a server on the room flow
// sends RoomState; while the room waits or shows its match over, the frame
// draws roomFrame(): the waiting screen (the roster with nicknames and the
// host's crown, the host's settings rows, the lobby panel's, sent as
// RoomSettings on change, read-only for everyone else, Start match, Leave)
// or the match-over screen (the host's Play again sends Start). The HUD
// names slots by nickname. --nick (?nick=, saved as `nick`, Settings > Name)
// is the name in Hello; without one the client picks a monkey name once.
//
// Content (§5.10): loaded at startup from --content, else content/ next to
// the executable, else the copy built into it (always, in the browser); its
// hash must match the server's online. Natively, local play polls the
// content directory about once a second and on a change reloads it and
// restarts the match with the same seed (errors are printed and the old
// content kept). Online the server's content and match config rule.
//
// Lobby settings (v0.24, core/lobby_menu.hpp, core/lobby_settings.hpp): the
// local lobby's panel picks the arena, mode, loadout, power-up set, bots, the bot
// difficulty, volume and mute, from the mouse, the arrow keys / WASD or a joined pad's d-pad and
// stick. Every change applies to the lobby's MatchSession and the audio at
// once and is saved (natively in SDL's preference directory, in the browser
// in localStorage) unless --no-settings; at startup the saved settings come
// first and the flags (or URL parameters) override them.
//
// Bots (§8, core/bot_roster.hpp): local bot slots are agents in the session's
// AgentHost, the reference bot at the lobby's difficulty tier, or natively a
// --bot-module .wasm (loaded once; one that fails to load is reported and
// the reference bot plays instead). Online the server owns the bots.
//
// The front end (v0.43, architecture §7): without --autostart, --connect,
// --room or --create the client opens on the main menu (core/menus.hpp,
// screens.cpp): Play online (the Play online screen: the region with its
// availability, Create room, a room code and Join; with --connect and no
// lobby it connects to that server), Play local (the local lobby), Settings
// (name, volume, sound, bot difficulty) and, natively, Quit. Arrows / WASD,
// a pad's d-pad or stick and the mouse move between items, Enter / Space or
// a pad's South / Start selects, Escape or a pad's East / Back goes back.
// Escape from the local lobby or any online screen returns to the menu.
// The room's waiting screen shows the code large with Copy link and, on
// touch devices in the browser, Share (IPlatformServices: SDL's clipboard
// natively, navigator.clipboard / navigator.share in the browser); the
// host's settings rows take the keyboard and pads like the local lobby's,
// Enter starts. A match's end shows the match-over screen (the winner, the
// final scoreboard, Play again / Back to lobby locally, the host's Play
// again and Leave online), rounds modes a scoreboard between rounds.
//
// Keys outside the game bindings: F1 debug overlay, P pause, O single step,
// Escape back (a match to its lobby, the lobby or an online screen to the
// menu, the menu quits natively), - / = bot count in the lobby, Enter / pad
// Start starts the match (and picks the focused match-over button). Pause,
// step and the bot keys do nothing online.
#include "client_audio.hpp"
#include "core/client_options.hpp"
#include "core/lobby_menu.hpp"
#include "core/lobby_settings.hpp"
#include "core/match_session.hpp"
#include "core/menus.hpp"
#include "core/net_session.hpp"
#include "core/online_menu.hpp"
#include "debug_overlay.hpp"
#include "hud.hpp"
#include "kfm/content/app_content.hpp"
#include "kfm/content/loader.hpp"
#include "kfm/platform/debug_ui.hpp"
#include "kfm/platform/gfx.hpp"
#include "kfm/platform/main_loop.hpp"
#include "kfm/platform/platform_services.hpp"
#include "kfm/platform/window.hpp"
#include "kfm/presentation/builder.hpp"
#include "kfm/presentation/camera.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/presentation/renderer.hpp"
#include "screens.hpp"
#include "sdl_input.hpp"
#include "ui_style.hpp"

#ifdef KFM_CLIENT_WEBRTC
#include "kfm/net/webrtc_transport.hpp"
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten/em_js.h>
#endif

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace kfm;
using client::MatchSession;
using client::SessionPhase;

// Which front-end screen the local frame draws (v0.43); online, the
// NetSession's phase decides instead.
enum class Screen : std::uint8_t { Menu = 0, Settings, Online, Play };

#ifdef __EMSCRIPTEN__
constexpr bool kQuitItem = false;  // a browser tab is closed by the browser
#else
constexpr bool kQuitItem = true;
#endif

constexpr std::uint64_t kCopiedShowNs = 2'500'000'000;  // "Copied" after Copy link

struct App {
  client::ClientOptions options;
  content::LoadedContent content;
  content::ContentWatcher contentWatcher;  // inactive unless hot reload applies
  platform::Window window;
  presentation::Renderer renderer;
  bool rendererReady = false;
  // What the renderer's meshes were built from (content hash, arena index):
  // the floor follows the arena, so a new arena or content re-initialises it.
  std::uint64_t rendererKey = 0;
  std::unique_ptr<MatchSession> session;
  std::unique_ptr<client::NetSession> net;  // network mode, while connected or showing why not
  client::NetPhase lastNetPhase = client::NetPhase::Connecting;
  bool connectRequested = false;                   // Play online with --connect: connect next frame
  std::optional<client::LobbyTarget> pendingRoom;  // the Play online screen's Create / Join
  bool leaveRequested = false;                     // an online screen's Leave: next frame
  bool reconnectRequested = false;                 // v0.44: the lost session's Reconnect
  // v0.42: the online room's settings rows (the host edits them) and the
  // RoomState count when the host last sent new settings (rows follow the
  // server again once its answer arrived).
  client::LobbyMenu roomMenu;
  std::optional<std::uint32_t> roomSentAt;
  // v0.43: the front end. The keyboard and pads reach the menus through
  // onEvent(); what they asked for is acted on in the frame.
  Screen screen = Screen::Menu;
  client::MainMenu mainMenu{kQuitItem};
  client::SettingsMenu settingsMenu;
  client::OnlineMenu onlineMenu;
  client::ButtonMenu overMenu;  // the match-over screen's buttons
  bool editNick = false;        // put the keyboard into the name field
  bool editCode = false;        // ... into the room code field
  bool menuActivate = false;    // Enter / Start on a menu screen, this frame
  bool roomChanged = false;     // the host's rows changed from the keyboard / a pad
  bool roomStart = false;       // the host's Enter on the waiting screen
  bool overActivate = false;    // Enter on the match-over screen
  std::uint64_t copiedAtNs = 0;
#ifdef KFM_CLIENT_WEBRTC
  std::unique_ptr<net::WebRtcTransport> regionsQuery;  // the lobby's region list, in flight
#endif
  bool regionsQueried = false;  // asked once per visit to the Play online screen
  bool roomAnnounced = false;   // the session's room was printed
  std::unique_ptr<platform::IPlatformServices> services;
  std::string pageUrl;  // the browser's window.location.href; empty natively
  client::GamepadSet pads;
  client::DebugOverlay overlay;
  client::ClientAudio audio;
  client::LobbyMenu lobby;      // the local lobby's settings panel
  client::MenuInput menuInput;  // raw input -> lobby menu actions
  bool lobbyChanged = false;    // a menu action changed a setting this frame
  presentation::RenderState renderState;
  std::uint64_t frames = 0;
  std::uint64_t lastFrameNs = 0;
  bool quit = false;
  int exitCode = EXIT_SUCCESS;
};

// Static storage: under Emscripten main() is unwound once the loop starts.
App gApp;

// --- saved lobby settings (lobby_settings.hpp) ------------------------------------

#ifdef __EMSCRIPTEN__
EM_JS_DEPS(kfm_settings, "$stringToNewUTF8,$UTF8ToString");
// localStorage can be missing or throw (privacy modes, sandboxed frames): the
// settings are then simply not remembered. (JavaScript: clang-format would
// break its operators.)
// clang-format off
EM_JS(char*, kfmLoadPrefJs, (const char* key), {
  try {
    const s = window.localStorage.getItem(UTF8ToString(key));
    return s === null ? 0 : stringToNewUTF8(s);
  } catch (e) {
    return 0;
  }
});
EM_JS(void, kfmSavePrefJs, (const char* key, const char* text), {
  try {
    window.localStorage.setItem(UTF8ToString(key), UTF8ToString(text));
  } catch (e) {
  }
});
// The page's address, for a room's invite link.
EM_JS(char*, kfmPageUrlJs, (), {
  try {
    return stringToNewUTF8(String(window.location.href));
  } catch (e) {
    return 0;
  }
});
// v0.43: Copy link and Share. Both need the page's transient activation,
// which the click (one frame earlier) gives; a refusal is silent.
EM_JS(int, kfmCopyTextJs, (const char* text), {
  try {
    if (!navigator.clipboard || !navigator.clipboard.writeText) return 0;
    navigator.clipboard.writeText(UTF8ToString(text)).catch(() => {});
    return 1;
  } catch (e) {
    return 0;
  }
});
EM_JS(int, kfmCanShareJs, (), {
  try {
    return typeof navigator.share === 'function' && navigator.maxTouchPoints > 0 ? 1 : 0;
  } catch (e) {
    return 0;
  }
});
EM_JS(int, kfmShareJs, (const char* title, const char* text, const char* url), {
  try {
    navigator.share({ title: UTF8ToString(title), text: UTF8ToString(text), url: UTF8ToString(url) })
        .catch(() => {});
    return 1;
  } catch (e) {
    return 0;
  }
});
// clang-format on

// localStorage keys: "kfm.settings", "kfm.guest_id".
[[nodiscard]] std::string prefKey(const char* name) {
  return std::string("kfm.") + name;
}

[[nodiscard]] std::string takeJsString(char* text) {
  if (text == nullptr) return {};
  std::string s(text);
  std::free(text);  // NOLINT(cppcoreguidelines-no-malloc): allocated by stringToNewUTF8
  return s;
}
#else
// Files in SDL's preference directory: "settings.txt", "guest_id.txt".
[[nodiscard]] std::filesystem::path prefPath(const char* name) {
  char* dir = SDL_GetPrefPath("kfm", "kung-fu-monkeys");  // created if missing
  if (dir == nullptr) return {};
  std::filesystem::path p = std::filesystem::path(dir) / (std::string(name) + ".txt");
  SDL_free(dir);
  return p;
}
#endif

[[nodiscard]] std::string loadPrefText(const char* name) {
#ifdef __EMSCRIPTEN__
  return takeJsString(kfmLoadPrefJs(prefKey(name).c_str()));
#else
  const std::filesystem::path path = prefPath(name);
  if (path.empty()) return {};
  const std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
#endif
}

void savePrefText(const char* name, std::string_view text) {
#ifdef __EMSCRIPTEN__
  kfmSavePrefJs(prefKey(name).c_str(), std::string(text).c_str());
#else
  const std::filesystem::path path = prefPath(name);
  if (path.empty()) return;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  if (!out) std::fprintf(stderr, "warning: cannot save %s\n", path.string().c_str());
#endif
}

[[nodiscard]] std::string loadSettingsText() {
  return loadPrefText("settings");
}

void saveSettingsText(const std::string& text) {
  savePrefText("settings", text);
}

// IPlatformServices (M6a): the guest id kept beside the settings (made per
// run under --no-settings, which stores nothing), the options' lobby as the
// session directory; v0.43: the clipboard and the share sheet (SDL's
// clipboard natively, navigator.clipboard / navigator.share in the browser).
[[nodiscard]] std::unique_ptr<platform::IPlatformServices> makePlatformServices(
    const client::ClientOptions& options) {
  platform::GuestIdStore store;
  if (options.settings) {
    store.load = [] { return loadPrefText("guest_id"); };
    store.save = [](std::string_view id) { savePrefText("guest_id", id); };
  }
  std::random_device device;
  const auto clock = static_cast<std::uint64_t>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
  const std::uint64_t a = (std::uint64_t{device()} << 32) | device();
  platform::ShareHooks share;
#ifdef __EMSCRIPTEN__
  share.copy = [](std::string_view text) { return kfmCopyTextJs(std::string(text).c_str()) != 0; };
  share.canShare = [] { return kfmCanShareJs() != 0; };
  share.share = [](std::string_view title, std::string_view text, std::string_view url) {
    return kfmShareJs(std::string(title).c_str(), std::string(text).c_str(),
                      std::string(url).c_str()) != 0;
  };
#else
  share.copy = [](std::string_view text) {
    return SDL_SetClipboardText(std::string(text).c_str());
  };
#endif
  return std::make_unique<platform::LocalPlatformServices>(std::move(store), options.lobby, a,
                                                           clock, std::move(share));
}

// The lobby's settings changed (panel or keys): apply them to the lobby's
// session and the audio, write them into the options and save them.
void applyLobbySettings(App& app) {
  MatchSession& s = *app.session;
  client::LobbySettings& ls = app.lobby.settings();
  if (s.arena() != ls.arena) s.setArena(ls.arena);
  if (s.mode() != ls.mode) s.setMode(ls.mode);
  if (s.powerUps() != ls.powerUps) s.setPowerUps(ls.powerUps);
  if (s.loadout() != ls.loadout) s.setLoadout(ls.loadout);
  s.setBotCount(ls.bots);
  ls.bots = s.botCount();  // clamped to the free slots
  if (s.botSpec().tier != ls.botTier) s.setBotTier(ls.botTier);
  app.audio.setMuted(ls.mute);
  app.audio.setVolumePercent(ls.volume);
  client::storeLobbySettings(ls, app.content.db, app.options);
  if (app.options.settings) saveSettingsText(client::settingsText(app.options));
}

// What changed outside the panel (the - / = keys, joins taking bot slots,
// the F1 overlay's audio controls) flows back into the panel, and is saved.
void syncLobbySettings(App& app) {
  client::LobbySettings& ls = app.lobby.settings();
  const client::LobbySettings before = ls;
  ls.bots = app.session->botCount();
  ls.volume = static_cast<uint8_t>(std::clamp(app.audio.volumePercent(), 0, 100));
  ls.mute = app.audio.muted();
  if (!(ls == before)) applyLobbySettings(app);
}

constexpr double kMaxBuilderDtSeconds = 0.25;  // a stalled frame must not fast-forward effects

// The rigged character model: generated by the build next to the executable
// natively (kfm_use_test_rig), embedded at a fixed path under Emscripten.
[[nodiscard]] std::string characterModelPath() {
#ifdef __EMSCRIPTEN__
  return "/content/models/test_rig.glb";
#else
  const char* base = SDL_GetBasePath();  // owned by SDL, ends with a separator
  return std::string(base != nullptr ? base : "") + "content/models/test_rig.glb";
#endif
}

// (Re)initialises the renderer when the match's arena or the loaded
// content changed since its meshes were built. False only when the renderer
// cannot be initialised at all.
bool syncRenderer(App& app, const sim::MatchContent& content, std::uint32_t contentHash,
                  std::uint8_t arena) {
  const std::uint64_t key = (std::uint64_t{contentHash} << 8) | arena;
  if (app.rendererReady && key == app.rendererKey) return true;
  app.rendererReady = app.renderer.init(content);
  app.rendererKey = key;
  if (!app.rendererReady) return false;
  if (!app.options.capsules) {
    const std::string modelPath = characterModelPath();
    if (!app.renderer.loadCharacterModel(modelPath.c_str())) {
      std::fprintf(stderr, "warning: drawing capsules: %s\n",
                   app.renderer.characterModelError().c_str());
    }
  }
  return true;
}

// Hot reload (§5.10): the content directory changed on disk.
void reloadContent(App& app, std::uint64_t nowNs) {
  content::LoadedContent reloaded;
  content::Diagnostics diag;
  if (!content::loadAppContent(app.content.dir.string(), {}, reloaded, diag)) {
    std::fprintf(stderr, "kfm_client: content reload failed, keeping the old content:\n%s",
                 diag.format().c_str());
    return;
  }
  if (!diag.items().empty()) std::fprintf(stderr, "%s", diag.format().c_str());
  app.content = std::move(reloaded);
  app.session->replaceContent(app.content.db, nowNs);
  app.lobby.setContent(app.content.db);
  app.lobby.settings().arena = app.session->arena();  // kept when the new content has it
  app.lobby.settings().mode = app.session->mode();
  app.lobby.settings().loadout = app.session->loadout();
  std::printf("kfm_client: reloaded %s; match restarted\n", app.content.describe().c_str());
  std::fflush(stdout);
}

// Whenever an ImGui widget has the keyboard (the overlay's fields, but also
// the lobby's Bots field with the overlay closed), key presses go to it, not
// to the game bindings or the app shortcuts.
[[nodiscard]] bool gameInputBlocked() {
  return platform::debugUiWantsKeyboard();
}

// Starts network mode: a WebRtcTransport to --connect and a NetSession over
// the lobby's bound devices. A transport that cannot be created leaves a
// failed session that shows why.
#ifdef KFM_CLIENT_WEBRTC
// The session's view of a lobby-mode WebRtcTransport.
class WebRtcLobbyJoin final : public client::ILobbyJoin {
 public:
  explicit WebRtcLobbyJoin(const net::WebRtcTransport& transport) : transport_(transport) {}
  [[nodiscard]] std::optional<client::LobbyRoomInfo> room() const override {
    const std::optional<net::LobbyRoom> r = transport_.lobbyRoom();
    if (!r) return std::nullopt;
    return client::LobbyRoomInfo{.code = r->code, .region = r->region};
  }
  [[nodiscard]] std::vector<std::byte> token() const override {
    const std::span<const std::byte> t = transport_.connectionToken();
    return {t.begin(), t.end()};
  }
  [[nodiscard]] std::string error() const override { return transport_.lastError(); }

 private:
  const net::WebRtcTransport& transport_;
};
#endif

// The transport a session config asks for: the lobby (create, join, or
// v0.44 rejoin with the old token) or --connect's server. Null with the
// reason when it cannot be created.
struct ClientTransport {
  std::unique_ptr<net::ITransport> owned;
  net::ITransport* raw{nullptr};
  std::unique_ptr<client::ILobbyJoin> lobby;  // lobby mode: the session's view of it
  std::string error;
};

ClientTransport makeTransport(const App& app, const client::NetSessionConfig& config) {
  ClientTransport out;
#ifdef KFM_CLIENT_WEBRTC
  if (app.options.verbose) net::WebRtcTransport::initLogging(net::WebRtcLogLevel::Debug);
  net::WebRtcClientConfig wc;
  wc.signalingUrl = config.lobby ? app.options.lobby : app.options.connect;
  wc.stunServer = app.options.stun;
  wc.connectionToken = config.token;
  if (config.lobby) {
    const client::LobbyTarget& t = *config.lobby;
    wc.lobby = net::LobbyRequest{
        .kind = t.create ? net::LobbyRequest::Kind::Create : net::LobbyRequest::Kind::Join,
        .region = t.region,
        .code = t.code,
        .token = config.rejoinToken,
        .buildId = config.buildId,
        .contentHash = app.content.hash};
  }
  auto transport = net::WebRtcTransport::createClient(wc);
  if (!transport) {
    out.error = transport.error();
    return out;
  }
  net::WebRtcTransport* raw = transport->get();
  out.raw = raw;
  if (config.lobby) out.lobby = std::make_unique<WebRtcLobbyJoin>(*raw);
  out.owned = std::move(*transport);
#else
  static_cast<void>(app);
  static_cast<void>(config);
  out.error = "this build has no WebRTC (KFM_WEBRTC=OFF)";
#endif
  return out;
}

// `room`: the Online section's Create / Join; else the options' --room /
// --create, else --connect.
void startNetwork(App& app, bool atStartup, std::uint64_t nowNs,
                  std::optional<client::LobbyTarget> room = std::nullopt) {
  client::NetSessionConfig config = client::netSessionConfig(app.options, atStartup);
  if (app.options.verbose)
    std::fprintf(stderr, "kfm_client: startNetwork atStartup=%d\n", int{atStartup});
  if (room) {
    config.lobby = std::move(room);
    config.serverLabel = app.options.lobby;
  }
  if (config.lobby) config.token.clear();  // the lobby's token replaces --token
  config.content = &app.content.db;
  app.roomAnnounced = false;
  const platform::InputSampler& lobby = app.session->sampler();
  ClientTransport t = makeTransport(app, config);
  app.net = std::make_unique<client::NetSession>(config, lobby, t.raw, nowNs, std::move(t.owned),
                                                 std::move(t.error));
  if (t.lobby) app.net->attachLobby(std::move(t.lobby));
  app.lastNetPhase = app.net->phase();
  app.roomMenu.setContent(app.net->content());
  app.roomSentAt.reset();
  std::printf("kfm_client: %s\n", app.net->statusText().c_str());
  std::fflush(stdout);
}

// v0.44: the lost session's Reconnect button: a new transport to the same
// server, or a lobby rejoin of the same room, with the same token (§6.6).
void reconnectNetwork(App& app, std::uint64_t nowNs) {
  client::NetSession& n = *app.net;
  ClientTransport t = makeTransport(app, n.reconnectConfig());
  n.reconnect(t.raw, nowNs, std::move(t.owned), std::move(t.error));
  if (t.lobby) n.attachLobby(std::move(t.lobby));
  app.lastNetPhase = n.phase();
  app.reconnectRequested = false;
  std::printf("kfm_client: reconnecting (%u): %s\n", n.reconnects(), n.statusText().c_str());
  std::fflush(stdout);
}

// The Play online screen's region list: asked of the lobby once per visit
// to the screen through a short-lived transport in Regions mode. The screen
// offers kRegions until the lobby answers, then every registered region with
// its availability (lobby protocol v1.1), and keeps kRegions if the lobby
// cannot be reached (the create is then refused with the real reason).
void pollRegions(App& app, std::uint64_t now) {
#ifdef KFM_CLIENT_WEBRTC
  if (!app.regionsQueried) {
    app.regionsQueried = true;
    const std::string url = app.services->lobbyUrl();
    if (url.empty()) return;
    net::WebRtcClientConfig wc;
    wc.signalingUrl = url;
    wc.stunServer = app.options.stun;
    wc.lobby = net::LobbyRequest{.kind = net::LobbyRequest::Kind::Regions,
                                 .region = {},
                                 .code = {},
                                 .token = {},
                                 .buildId = client::netSessionConfig(app.options, false).buildId,
                                 .contentHash = app.content.hash};
    if (auto t = net::WebRtcTransport::createClient(wc)) app.regionsQuery = std::move(*t);
    return;
  }
  if (!app.regionsQuery) return;
  net::ConnectionEvents events;
  app.regionsQuery->poll(now, events);
  if (const std::optional<std::vector<net::LobbyRegion>> regions =
          app.regionsQuery->lobbyRegions()) {
    std::vector<client::RegionChoice> choices;
    std::string list;
    for (const net::LobbyRegion& r : *regions) {
      choices.push_back(client::RegionChoice{.name = r.name, .free = r.free});
      list += (list.empty() ? "" : ", ") + r.name + (r.free ? "" : " (no server)");
    }
    app.onlineMenu.setRegions(choices);
    app.lobby.settings().region = app.onlineMenu.region();
    std::printf("kfm_client: lobby regions: %s\n", list.empty() ? "(none)" : list.c_str());
    app.regionsQuery.reset();
    return;
  }
  for (std::size_t i = 0; i < events.count; ++i) {
    if (events.items[i].kind != net::ConnectionEventKind::Disconnected) continue;
    std::printf("kfm_client: lobby regions unavailable: %s\n",
                app.regionsQuery->lastError().c_str());
    app.regionsQuery.reset();
    return;
  }
#else
  (void)app;
  (void)now;
#endif
}

// Escape or Leave online: disconnect, hand the devices back to the local
// lobby and show the menu. The room's code stays in the Play online
// screen's field, so Join takes the player back in; --room / --create
// served their one join.
void leaveNetwork(App& app) {
  app.regionsQueried = false;  // the next Play online visit asks the lobby again
  if (app.net->room()) app.onlineMenu.setCode(app.net->room()->code);
  app.net->disconnect();
  app.session->sampler() = app.net->lobbySampler();
  app.net.reset();
  app.options.room.clear();
  app.options.create = false;
  app.screen = Screen::Menu;
  app.leaveRequested = false;
  std::printf("kfm_client: back to the menu\n");
  std::fflush(stdout);
}

// The main menu's choice.
void chooseMainItem(App& app, client::MainMenuItem item) {
  app.audio.click();
  switch (item) {
    case client::MainMenuItem::PlayOnline:
      if (!app.services->lobbyUrl().empty()) {
        app.screen = Screen::Online;
        app.regionsQueried = false;
        app.onlineMenu.setFocus(static_cast<std::size_t>(client::OnlineRow::CreateRoom));
      } else if (!app.options.connect.empty()) {
        app.connectRequested = true;  // --connect without a lobby: that server
      }
      break;
    case client::MainMenuItem::PlayLocal:
      app.screen = Screen::Play;
      break;
    case client::MainMenuItem::Settings:
      app.screen = Screen::Settings;
      app.settingsMenu.setFocus(0);
      break;
    case client::MainMenuItem::Quit:
      app.quit = true;
      break;
  }
}

// Escape (or a pad's Back) on a menu screen.
void menuBack(App& app) {
  if (app.screen == Screen::Menu) {
    if (kQuitItem) app.quit = true;
    return;
  }
  app.screen = Screen::Menu;
  app.audio.click();
}

// App shortcuts that are not game bindings.
void handleShortcut(App& app, const SDL_Event& event) {
  if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) return;
  MatchSession& s = *app.session;
  const SDL_Scancode sc = event.key.scancode;
  if (sc == SDL_SCANCODE_F1) {
    app.overlay.open = !app.overlay.open;
    return;
  }
  if (gameInputBlocked()) return;
  if (app.net) {
    // Online the server's clock rules: no pause, step or bot keys.
    if (sc == SDL_SCANCODE_ESCAPE) leaveNetwork(app);
    return;
  }
  if (app.screen != Screen::Play) {
    if (sc == SDL_SCANCODE_ESCAPE) menuBack(app);
    return;
  }
  switch (sc) {
    case SDL_SCANCODE_ESCAPE:
      if (s.phase() == SessionPhase::Match) {
        s.returnToLobby();
      } else {
        app.screen = Screen::Menu;
      }
      break;
    case SDL_SCANCODE_P:
      if (s.phase() == SessionPhase::Match) s.setPaused(!s.paused());
      break;
    case SDL_SCANCODE_O:
      if (s.phase() == SessionPhase::Match) {
        s.setPaused(true);
        s.requestSingleStep();
      }
      break;
    case SDL_SCANCODE_MINUS:
      if (s.phase() == SessionPhase::Lobby && s.botCount() > 0) {
        s.setBotCount(static_cast<std::uint8_t>(s.botCount() - 1));
      }
      break;
    case SDL_SCANCODE_EQUALS:
      if (s.phase() == SessionPhase::Lobby) {
        s.setBotCount(static_cast<std::uint8_t>(s.botCount() + 1));
      }
      break;
    default:
      break;
  }
}

// Input that counts as a user gesture for the browser's autoplay policy.
[[nodiscard]] bool isGesture(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      return true;
    default:
      return false;
  }
}

void onEvent(void* user, const SDL_Event& event) {
  auto* app = static_cast<App*>(user);
  if (isGesture(event)) app->audio.onUserGesture();
  app->pads.onEvent(event);
  handleShortcut(*app, event);
  const auto input = client::translateSdlEvent(event);
  if (!input) return;
  // While a debug text field has the keyboard, key presses stay out of the
  // game; releases always go through so no key is left held.
  if (input->kind == platform::InputEventKind::KeyDown && gameInputBlocked()) return;
  if (app->net) {
    // v0.43: the room's screens. Every device navigates (nothing joins
    // online); the press still goes to the session, which samples nothing
    // while the room waits or shows its match over.
    client::NetSession& n = *app->net;
    const client::MenuAction action = app->menuInput.translate(*input, true);
    if (n.phase() == client::NetPhase::Waiting && n.isHost()) {
      if (action != client::MenuAction::None && app->roomMenu.apply(action)) {
        app->roomChanged = true;
      }
      if (client::isActivateEvent(*input, true)) app->roomStart = true;
    } else if (n.phase() == client::NetPhase::Over || n.canReconnect()) {
      // The match-over buttons, or (v0.44) Reconnect / Leave.
      app->overMenu.apply(action);
      if (client::isMenuActivate(*input)) app->overActivate = true;
    }
    n.push(*input);
    return;
  }
  // v0.43: the menu screens take every device's navigation and nothing
  // reaches the session (no joins, no Start there).
  if (app->screen != Screen::Play) {
    const client::MenuAction action = app->menuInput.translate(*input, true);
    switch (app->screen) {
      case Screen::Menu:
        app->mainMenu.apply(action);
        break;
      case Screen::Settings:
        if (app->settingsMenu.apply(action, app->lobby.settings())) app->lobbyChanged = true;
        break;
      case Screen::Online:
        if (app->onlineMenu.apply(action)) {
          app->lobby.settings().region = app->onlineMenu.region();
          app->lobbyChanged = true;
        }
        break;
      case Screen::Play:
        break;
    }
    if (client::isMenuActivate(*input)) app->menuActivate = true;
    if (client::isMenuBack(*input)) menuBack(*app);
    return;
  }
  // The local lobby's settings panel: navigation from the keyboard and the
  // joined pads (an unbound pad's press is its join). The press still goes
  // to the session, which ignores bound devices' presses in the lobby.
  MatchSession& s = *app->session;
  const bool bound = client::eventDeviceBound(s.sampler(), *input);
  if (s.phase() == SessionPhase::Lobby) {
    const client::MenuAction action = app->menuInput.translate(*input, bound);
    if (action != client::MenuAction::None && app->lobby.apply(action)) app->lobbyChanged = true;
  } else if (s.curr().rules.phase == sim::MatchPhase::MatchEnded) {
    // v0.43: the match-over screen's buttons. Enter on Play again restarts
    // here; on Back to lobby it goes to the session, which returns.
    app->overMenu.apply(app->menuInput.translate(*input, bound));
    if (client::isActivateEvent(*input, bound) && app->overMenu.focus() == 0) {
      s.restart(platform::Window::nowNs(), true);
      return;
    }
  }
  s.push(*input);
}

bool writePpm(const std::string& path, int width, int height) {
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 3U);
  if (!platform::readPixelsRgb(width, height, pixels)) {
    std::fprintf(stderr, "error: reading the framebuffer failed\n");
    return false;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "error: cannot open '%s' for writing\n", path.c_str());
    return false;
  }
  out << "P6\n" << width << ' ' << height << "\n255\n";
  out.write(reinterpret_cast<const char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size()));
  return static_cast<bool>(out.flush());
}

// The small line in the main menu's corner: what this build speaks.
[[nodiscard]] std::string versionLine(const App& app) {
  char text[96];
  std::snprintf(text, sizeof text, "protocol %u  content %08x",
                static_cast<unsigned>(net::kProtocolVersion), app.content.hash);
  return text;
}

// v0.43: the menu screens over the lobby's arena: the main menu, Settings,
// Play online. A press from onEvent (menuActivate) acts on the focused item
// here, like a click.
void menuFrame(App& app, std::uint64_t now) {
  const bool press = std::exchange(app.menuActivate, false);
  switch (app.screen) {
    case Screen::Menu: {
      std::optional<client::MainMenuItem> item =
          client::drawMainMenu(app.mainMenu, versionLine(app));
      if (press && !item) item = app.mainMenu.activate();
      if (item) chooseMainItem(app, *item);
      break;
    }
    case Screen::Settings: {
      if (press) {
        if (app.settingsMenu.focusedRow() == client::SettingsRow::Name) app.editNick = true;
        if (app.settingsMenu.focusedRow() == client::SettingsRow::Back) {
          menuBack(app);
          break;
        }
      }
      const client::SettingsOutcome out = client::drawSettingsScreen(
          app.settingsMenu, app.lobby.settings(), app.options.nick, app.editNick);
      if (out.changed) app.lobbyChanged = true;
      if (out.nickChanged) {
        app.audio.click();
        std::printf("kfm_client: name %s\n", app.options.nick.c_str());
        std::fflush(stdout);
        if (app.options.settings) saveSettingsText(client::settingsText(app.options));
      }
      if (out.back) menuBack(app);
      break;
    }
    case Screen::Online: {
      pollRegions(app, now);
      if (press) {
        switch (app.onlineMenu.activate()) {
          case client::OnlineMenu::Activation::EditCode:
            app.editCode = true;
            break;
          case client::OnlineMenu::Activation::Back:
            menuBack(app);
            return;
          case client::OnlineMenu::Activation::Command:
          case client::OnlineMenu::Activation::None:
            break;
        }
      }
      const client::OnlineOutcome out =
          client::drawOnlineScreen(app.onlineMenu, app.editCode, app.services->lobbyUrl());
      if (out.regionChanged) {
        app.lobby.settings().region = app.onlineMenu.region();
        app.lobbyChanged = true;
      }
      if (std::optional<client::LobbyCommand> cmd = app.onlineMenu.takeCommand()) {
        app.audio.click();
        app.pendingRoom =
            client::LobbyTarget{.create = cmd->kind == client::LobbyCommand::Kind::CreateRoom,
                                .region = cmd->region,
                                .code = cmd->code};
      }
      if (out.back) menuBack(app);
      break;
    }
    case Screen::Play:
      break;
  }
  if (app.lobbyChanged) {
    applyLobbySettings(app);
    app.audio.click();
    app.lobbyChanged = false;
  }
}

// The local frame: the menus, the lobby or a local match (M2).
presentation::Camera localFrame(App& app, std::uint64_t now, double dt, int w, int h,
                                float density) {
  MatchSession& s = *app.session;
  s.advance(now);
  s.presentation().pingSlots = s.localMask();  // v0.35: a local human's call pings
  presentation::buildRenderState(
      s.presentation(), s.renderPrev(), s.curr(), s.renderAlpha(), s.frameEvents(),
      s.matchContent(), static_cast<float>(std::min(dt, kMaxBuilderDtSeconds)), app.renderState);
  const presentation::Camera camera = presentation::makeArenaCamera(
      s.matchContent().arena, static_cast<float>(w), static_cast<float>(h));
  (void)syncRenderer(app, s.matchContent(), app.content.hash, s.matchConfig().arena);

  platform::debugUiNewFrame(w, h, dt, density);
  if (s.phase() == SessionPhase::Lobby) syncLobbySettings(app);
  if (app.screen != Screen::Play) {
    menuFrame(app, now);
  } else if (s.phase() == SessionPhase::Lobby) {
    const client::LobbyOutcome lobby = client::drawLobby(s, app.lobby);
    if (lobby.changed || app.lobbyChanged) {
      applyLobbySettings(app);
      app.audio.click();
    }
    app.lobbyChanged = false;
    if (lobby.start) {
      app.audio.click();
      (void)s.startMatch(now);
    }
  } else {
    client::HudView view = client::hudView(s);
    view.overScreen = s.curr().rules.phase == sim::MatchPhase::MatchEnded;
    client::drawMatchHud(
        view,
        client::HudInput{.state = &app.renderState, .camera = &camera, .uiScale = 1.0f / density});
    if (view.overScreen) {
      // v0.43: the match-over screen: Play again (a new seed) or the lobby.
      client::MatchOverView over = client::matchOverView(view, app.renderState);
      over.buttons = {"Play again", "Back to lobby"};
      std::optional<std::size_t> pick = client::drawMatchOver(over, app.overMenu);
      if (std::exchange(app.overActivate, false) && !pick) pick = app.overMenu.focus();
      if (pick) {
        app.audio.click();
        if (*pick == 0) {
          s.restart(now, true);
        } else {
          s.returnToLobby();
        }
        app.overMenu.setFocus(0);
      }
    } else {
      app.overMenu.setFocus(0);
    }
  }
  app.overlay.draw(s, app.renderer.stats(), now);
  app.audio.frame(app.renderState);
  return camera;
}

// Copy link / Share from a room screen (v0.43).
void applyShare(App& app, const client::ShareOutcome& out, const client::ShareView& view,
                std::uint64_t now) {
  if (out.copy) {
    app.audio.click();
    if (app.services->copyText(view.link)) app.copiedAtNs = now;
  }
  if (out.share) {
    app.audio.click();
    (void)app.services->share("Kung Fu Monkeys", "Join my room " + view.code + "!", view.link);
  }
}

// v0.42: the online room's waiting or match-over screen. The rows show the
// server's settings, except between the host's change and the server's
// answer; a change goes out as RoomSettings, the button as Start. v0.43:
// the waiting screen with Copy link / Share and the match-over screen.
void roomFrame(App& app, client::NetSession& n, const client::ShareView& share,
               const client::HudView& view, std::uint64_t now) {
  const std::optional<net::RoomStateMsg>& room = n.roomState();
  if (!room) return;
  if (room->phase == net::RoomPhase::Over) {
    client::MatchOverView over = client::matchOverView(view, app.renderState);
    if (n.isHost()) {
      over.buttons = {"Play again", "Leave"};
    } else {
      over.buttons = {"Leave"};
      over.waiting = "Waiting for " +
                     (room->host.isValid() ? n.slotName(room->host) : std::string("the host")) +
                     " to play again";
    }
    std::optional<std::size_t> pick = client::drawMatchOver(over, app.overMenu);
    if (std::exchange(app.overActivate, false) && !pick) pick = app.overMenu.focus();
    if (pick) {
      app.audio.click();
      if (over.buttons[*pick] == "Play again") {
        n.start();
      } else {
        app.leaveRequested = true;
      }
      app.overMenu.setFocus(0);
    }
    return;
  }
  app.overMenu.setFocus(0);
  app.overActivate = false;
  if (app.roomSentAt && n.client().roomUpdates() != *app.roomSentAt) app.roomSentAt.reset();
  client::LobbySettings& ls = app.roomMenu.settings();
  if (!app.roomSentAt) {
    ls.arena = room->settings.arena;
    ls.mode = room->settings.mode;
    ls.loadout = room->settings.loadout;
    ls.powerUps = room->settings.powerUps;
    ls.bots = room->settings.bots;
  }
  const client::RoomOutcome out = client::drawRoomScreen(n, app.roomMenu, share);
  applyShare(app, out.share, share, now);
  if (out.leave) app.leaveRequested = true;
  if (out.settingsChanged || std::exchange(app.roomChanged, false)) {
    app.audio.click();
    n.sendSettings(net::RoomSettings{.arena = ls.arena,
                                     .mode = ls.mode,
                                     .loadout = ls.loadout,
                                     .powerUps = ls.powerUps,
                                     .bots = ls.bots});
    app.roomSentAt = n.client().roomUpdates();
  }
  if ((out.start || std::exchange(app.roomStart, false)) && n.isHost()) {
    app.audio.click();
    n.start();
  }
}

// The network frame: connection status until the match runs, then the
// predicted match with remote players smoothed (§6.9).
presentation::Camera netFrame(App& app, std::uint64_t now, double dt, int w, int h, float density) {
  client::NetSession& n = *app.net;
  n.advance(now);
  if (n.room() && !app.roomAnnounced) {
    app.roomAnnounced = true;
    std::printf("kfm_client: in room %s (%s); invite: %s\n", n.room()->code.c_str(),
                n.room()->region.c_str(),
                client::roomShareText(*n.room(), app.pageUrl, app.options.lobby).c_str());
    std::fflush(stdout);
    // A room made with --create is the one Start rejoins after Escape.
    if (app.options.create && n.config().lobby && n.config().lobby->create) {
      app.options.create = false;
      app.options.room = n.room()->code;
    }
  }
  if (n.phase() != app.lastNetPhase) {
    app.lastNetPhase = n.phase();
    std::printf("kfm_client: %s\n", n.statusText().c_str());
    std::fflush(stdout);
  }
  n.build(static_cast<float>(std::min(dt, kMaxBuilderDtSeconds)), app.renderState);
  const presentation::Camera camera = presentation::makeArenaCamera(
      n.matchContent().arena, static_cast<float>(w), static_cast<float>(h));
  (void)syncRenderer(app, n.matchContent(), app.content.hash, n.client().simConfig().arena);

  platform::debugUiNewFrame(w, h, dt, density);
  client::ShareView share;
  if (n.room()) {
    share.code = n.room()->code;
    share.region = n.room()->region;
    share.link = client::roomInviteLink(*n.room(), app.pageUrl);
    share.canShare = app.services->canShare();
    share.copied = app.copiedAtNs != 0 && now - app.copiedAtNs < kCopiedShowNs;
  }
  const client::NetPhase phase = n.phase();
  client::HudView view = client::hudView(n);
  view.overScreen = phase == client::NetPhase::Over && n.roomState().has_value();
  if (n.everRan() && phase != client::NetPhase::Waiting) {
    if (n.room()) view.shareLine = "Invite: " + share.link;
    client::drawMatchHud(
        view,
        client::HudInput{.state = &app.renderState, .camera = &camera, .uiScale = 1.0f / density});
  }
  if (phase == client::NetPhase::Waiting || phase == client::NetPhase::Over) {
    roomFrame(app, n, share, view, now);
  } else if (!n.running()) {
    const client::NetStatusOutcome out = client::drawNetStatus(n, share, app.overMenu);
    applyShare(app, out.share, share, now);
    // v0.44: Reconnect / Leave, by click or Enter on the focused one.
    const bool activate = std::exchange(app.overActivate, false) && n.canReconnect();
    if (out.reconnect || (activate && app.overMenu.focus() == 0)) {
      app.audio.click();
      app.reconnectRequested = true;
    } else if (out.leave || (activate && app.overMenu.focus() == 1)) {
      app.audio.click();
      app.leaveRequested = true;
    }
  }
  app.overlay.draw(n, app.renderer.stats());
  app.audio.frame(app.renderState);
  return camera;
}

bool frame(void* user) {
  auto* app = static_cast<App*>(user);
  if (!app->window.pollEvents(onEvent, app)) app->quit = true;

  const std::uint64_t now = platform::Window::nowNs();
  const double dt = static_cast<double>(now - app->lastFrameNs) * 1e-9;
  app->lastFrameNs = now;
  app->overlay.recordFrame(dt);

  const platform::DrawableSize size = app->window.drawableSize();
  const int w = std::max(size.width, 1);
  const int h = std::max(size.height, 1);
  const float density = app->window.pixelDensity();
  if (!app->net && app->contentWatcher.poll(static_cast<double>(now) * 1e-9)) {
    reloadContent(*app, now);
  }
  if (app->connectRequested && !app->net) startNetwork(*app, false, now);
  app->connectRequested = false;
  if (app->pendingRoom && !app->net) startNetwork(*app, false, now, std::move(app->pendingRoom));
  app->pendingRoom.reset();
  if (app->reconnectRequested && app->net) reconnectNetwork(*app, now);
  app->reconnectRequested = false;
  const presentation::Camera camera =
      app->net ? netFrame(*app, now, dt, w, h, density) : localFrame(*app, now, dt, w, h, density);
  platform::debugUiFlush();
  if (app->leaveRequested && app->net) leaveNetwork(*app);

  // v0.43: the page and the menus' dark jungle green (ui::kJungle) around the arena.
  platform::beginDefaultPass({.r = 0.078F, .g = 0.141F, .b = 0.094F, .a = 1.0F}, w, h);
  if (app->rendererReady) app->renderer.draw(app->renderState, camera, w, h);
  platform::debugUiDraw();
  platform::endPass();

  ++app->frames;
  const bool lastFrame = app->quit || (app->options.exitAfterFrames != 0 &&
                                       app->frames >= app->options.exitAfterFrames);
  if (lastFrame && !app->options.screenshotPath.empty() &&
      !writePpm(app->options.screenshotPath, w, h)) {
    app->exitCode = EXIT_FAILURE;
  }
  app->window.swap();
  return !lastFrame;
}

void onExit(void* user) {
  auto* app = static_cast<App*>(user);
  if (app->rendererReady) app->renderer.shutdown();
  app->rendererReady = false;
  platform::debugUiShutdown();
  platform::gfxShutdown();
  app->audio.shutdown();
  app->pads.closeAll();
  app->window.shutdown();
  if (app->net) {
    const net::ClientNetStats& st = app->net->client().netStats();
    std::printf(
        "kfm_client: stopped after %llu frames online (%s) at tick %u, checksum 0x%08X, "
        "%llu rollbacks, %llu checksum mismatches\n",
        static_cast<unsigned long long>(app->frames), client::netPhaseName(app->net->phase()),
        app->net->curr().tick.value, app->net->checksum(),
        static_cast<unsigned long long>(st.rollbacks),
        static_cast<unsigned long long>(st.checksumMismatches));
    app->net->disconnect();
    app->net.reset();
  } else {
    std::printf("kfm_client: stopped after %llu frames at tick %u, checksum 0x%08X\n",
                static_cast<unsigned long long>(app->frames), app->session->curr().tick.value,
                app->session->checksum());
  }
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(misc-const-correctness): main signature
  App& app = gApp;
  std::string error;
  const std::span<const char* const> args(argv, static_cast<std::size_t>(argc));
  if (!client::parseClientOptions(args, app.options, error)) {
    std::fprintf(stderr, "error: %s\n%s", error.c_str(), client::kUsage);
    return EXIT_FAILURE;
  }
  {
    content::Diagnostics diag;
    const char* base = SDL_GetBasePath();  // owned by SDL; null under Emscripten is fine
    const bool loaded = content::loadAppContent(
        app.options.contentDir,
        base != nullptr ? std::filesystem::path(base) : std::filesystem::path{}, app.content, diag);
    if (!diag.items().empty()) std::fprintf(stderr, "%s", diag.format().c_str());
    if (!loaded) {
      std::fprintf(stderr, "error: the content failed to load\n");
      return EXIT_FAILURE;
    }
    std::printf("kfm_client: %s\n", app.content.describe().c_str());
    std::fflush(stdout);
  }
  // Saved lobby settings first, then the command line (or URL) again on top,
  // so a flag always wins for this run.
  if (app.options.settings) {
    const std::string saved = loadSettingsText();
    if (!saved.empty()) {
      client::ClientOptions merged;
      std::vector<std::string> warnings;
      client::applySettingsText(saved, app.content.db, merged, warnings);
      for (const std::string& w : warnings)
        std::fprintf(stderr, "warning: %s\n", w.c_str());
      if (client::parseClientOptions(args, merged, error)) app.options = std::move(merged);
    }
  }
  // v0.42: a player who never gave a name gets a monkey's, kept from then on.
  if (app.options.nick.empty()) {
    app.options.nick = client::randomMonkeyName(
        static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()));
    if (app.options.settings) saveSettingsText(client::settingsText(app.options));
  }
  client::LobbySettings lobby;
  if (!client::lobbySettingsFrom(app.options, app.content.db, lobby, error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return EXIT_FAILURE;
  }
  app.lobby.setContent(app.content.db);
  app.lobby.settings() = lobby;
  app.roomMenu.setRoomRows(true);
  app.onlineMenu.setRegion(lobby.region);
  app.services = makePlatformServices(app.options);
  app.mainMenu.setOnlineAvailable(!app.services->lobbyUrl().empty() ||
                                  !app.options.connect.empty());
#ifdef __EMSCRIPTEN__
  app.pageUrl = takeJsString(kfmPageUrlJs());
#else
  app.pageUrl = client::defaultPageUrl();  // usually empty: Copy link copies the code
#endif
  if (!app.services->lobbyUrl().empty()) {
    std::printf("kfm_client: lobby %s, guest %.8s\n", app.services->lobbyUrl().c_str(),
                app.services->guestId().c_str());
    std::fflush(stdout);
  }
#ifndef __EMSCRIPTEN__
  if (app.options.hotReload && !app.content.embedded()) {
    app.contentWatcher = content::ContentWatcher(app.content.dir);
  }
#endif
  if (!app.window.init({.title = "Kung Fu Monkeys", .width = 1280, .height = 720, .vsync = true})) {
    return EXIT_FAILURE;
  }
  if (!platform::gfxInit()) {
    std::fprintf(stderr, "error: sokol_gfx failed to initialise\n");
    return EXIT_FAILURE;
  }
  if (!platform::debugUiInit(app.window)) {
    std::fprintf(stderr, "error: Dear ImGui failed to initialise\n");
    platform::gfxShutdown();
    return EXIT_FAILURE;
  }
  client::ui::applyUiStyle();  // v0.43: the game font and palette

  // Network mode at startup (--connect, --room, --create): straight online;
  // the local lobby stays local play (v0.43).
  const bool online = app.options.networkMode();
  app.session =
      std::make_unique<MatchSession>(client::SessionConfig{.seed = app.options.seed,
                                                           .arena = lobby.arena,
                                                           .mode = lobby.mode,
                                                           .powerUps = lobby.powerUps,
                                                           .loadout = lobby.loadout,
                                                           .content = &app.content.db,
                                                           .botSeed = app.options.playersSeed,
                                                           .bots = lobby.bots,
                                                           .botSpec = client::botSpec(app.options),
                                                           .hostHandlesStart = false});
  if (!app.session->botSpecError().empty()) {
    std::fprintf(stderr, "error: %s; the bots are the reference bot (%s)\n",
                 app.session->botSpecError().c_str(), app.session->botRoster().describe().c_str());
  } else if (!online && app.session->botRoster().moduleActive()) {
    std::printf("kfm_client: bots run %s\n", app.session->botRoster().describe().c_str());
    std::fflush(stdout);
  }
  // A renderer that cannot create its buffers, shaders or pipelines is fatal
  // (so client_smoke catches it); a missing character model is not.
  if (!syncRenderer(app, app.session->matchContent(), app.content.hash,
                    app.session->matchConfig().arena)) {
    std::fprintf(stderr, "error: the renderer failed to initialise\n");
    platform::debugUiShutdown();
    platform::gfxShutdown();
    app.window.shutdown();
    return EXIT_FAILURE;
  }

  app.overlay.open = app.options.overlay;
  app.overlay.audio = &app.audio;
  app.audio.init(lobby.mute, lobby.volume);
  app.lobby.settings().bots = app.session->botCount();
  app.lastFrameNs = platform::Window::nowNs();
  if (online) {
    // --connect connects at startup with the devices bound so far (keyboard
    // left: one slot, unless --players); Escape returns to the menu.
    app.screen = Screen::Play;
    startNetwork(app, true, app.lastFrameNs);
  } else if (app.options.screen != "menu" && !app.options.autostart) {
    // v0.43: --screen settings|online|local (screenshots, development).
    if (app.options.screen == "settings") {
      app.screen = Screen::Settings;
    } else if (app.options.screen == "local") {
      app.screen = Screen::Play;
    } else if (!app.services->lobbyUrl().empty()) {
      app.screen = Screen::Online;
    }
  } else if (app.options.autostart) {
    app.screen = Screen::Play;  // straight into local play, as before the menu
    if (!app.session->startMatch(app.lastFrameNs)) {
      std::fprintf(stderr, "warning: --autostart with no players; staying in the lobby\n");
    }
  }
  platform::runMainLoop(&app, frame, onExit);
  return app.exitCode;
}
