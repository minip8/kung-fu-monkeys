// kfm_server: the headless dedicated server (architecture §3, §6, §13.5).
// One match at a time over WebRtcTransport. Two signaling modes:
//   direct (development, tests)  clients signal on a plain ws:// WebSocket
//                                at --bind:--port;
//   lobby (M6a production)       --lobby URL: no TCP port at all; the
//                                process dials the lobby's /server socket,
//                                registers (--lobby-secret, --region,
//                                --name) and receives rooms, peers, tokens
//                                and their signaling over it
//                                (docs/lobby-protocol.md, net::LobbyLink,
//                                server_lobby.hpp).
// Either way clients then talk WebRTC data channels.
//
//   kfm_server [--port P] [--bind ADDR] [--stun URL] [--ice-ports A-B] [--seed S] [--bot SPEC]...
//              [--bots N] [--bots-seed S] [--bot-delay auto|N] [--fuel N]
//              [--memory-on-respawn keep|wipe] [--max-players N] [--token T]... [--restart-delay S]
//              [--exit-after-seconds N] [--exit-after-matches N] [--stats-every S]
//              [--content DIR] [--arena NAME] [--mode NAME] [--loadout NAME]
//              [--powerups LIST] [--auto-start] [--verbose]
//              [--lobby URL --lobby-secret S --region R [--name N]]
//
// The room (v0.42, architecture §6.6): by default, in either mode, the
// process hosts an online room that waits (no ticks) until its host, the
// first client to hold a slot, picks the settings and starts; a match end
// shows the match over until the host plays again. --arena, --mode,
// --loadout, --powerups and --bots are the room's defaults, --bot the specs
// its bot count draws from. --auto-start keeps the M3 policy instead: a
// match runs from the start with every bot, and the next one starts
// --restart-delay after each match end; --exit-after-matches implies it.
//
// --port P             signaling port (default 9000; 0 = ephemeral, printed)
// --bind ADDR          signaling bind address (default all interfaces)
// --stun URL           "stun:host:port" for the server's ICE (default none;
//                      behind a 1:1 NAT such as a cloud VM's, give one so
//                      clients get server-reflexive candidates)
// --ice-ports A-B      the UDP ports ICE may use, A..B inclusive (default
//                      any), to match a firewall rule
// --lobby URL          lobby mode: the lobby's base URL (ws://host:port or
//                      wss://host); "/server" is appended. Excludes --port,
//                      --bind and --token: no signaling port is bound and the
//                      tokens are the lobby's
// --lobby-secret S     the lobby's shared secret (default: the environment
//                      variable KFM_LOBBY_SECRET); required with --lobby
// --region R           the region this process serves (eu, us, ...);
//                      required with --lobby
// --name N             a name for the lobby's logs (default hostname:pid)
// In lobby mode a room assigned while no peer is connected resets the room to
// Waiting with the defaults (--auto-start: restarts the match, fresh seeds,
// bots re-seeded), the process offers max players minus
// bots human slots (its `capacity`), the stats line ends with the lobby
// state, and a lost lobby link is retried every 1 s doubling to 30 s.
// --seed S             first match seed (default: from the clock, printed)
// --bot SPEC           a server-side bot (repeatable, in slot order): ref,
//                      ref:easy|medium|hard or a bot module path.wasm; a
//                      module that fails to load is a startup error
// --bots N             at least N bots: the --bot list padded with ref:hard
//                      (default 0); bots take the lowest free slots
// --bots-seed S        the first match's bot seed (default 0xB07B07); the
//                      agent in slot s gets S ^ (0x9E3779B97F4A7C15 * (s + 1)),
//                      kfm_arena's formula; each next match's seed is
//                      splitmix64(S)
// --bot-delay auto|N   the bots' input delay in ticks: auto (default) follows
//                      the median lead of the human clients, 0 with none;
//                      N fixes it (0..120)
// --fuel N             a bot module's instruction budget per tick (default
//                      100000); over it the tick repeats the last input
// --memory-on-respawn keep|wipe   v0.35: a bot whose monkey was eliminated is
//                      paused until it is back; wipe init()s it again then
//                      (a module gets a fresh instance), keep (default) not
// --max-players N      slots open to clients and bots, 1..10 (default 10)
// --token T            accept only these connection tokens (repeatable;
//                      none = accept every client)
// --restart-delay S    --auto-start: seconds between a match's end and the
//                      next (default 3)
// --auto-start         the M3 policy instead of the room (see above)
// --exit-after-seconds N, --exit-after-matches N   stop cleanly (0 = never)
// --stats-every S      one line of server stat deltas every S seconds, with
//                      per-bot acts and substitutions (modules: traps,
//                      over-budget ticks, mean fuel per act)
// --content DIR        content directory (default: content/ next to the
//                      executable, else the copy built into it)
// --arena NAME, --mode NAME, --loadout NAME   the matches' arena, mode and
//                      loadout (default: the first of each); clients receive
//                      them in Welcome/MatchConfig
// --powerups LIST      the power-ups the arena's spawners may draw: all
//                      (default), none or comma-separated names
//                      (shield,speed_boost); sent with the arena and mode
// --verbose            match/round starts, per-peer stats, libdatachannel at Info
//
// Prints the content it loaded (directory, hash) and then "KFM_SERVER
// listening ws://ADDR:PORT" once the socket is bound (lobby mode: "KFM_SERVER
// lobby URL region=R name=N"). A client must match
// the content hash and the build id 0 (Reject otherwise). SIGINT/SIGTERM disconnect every peer with
// ServerShuttingDown and exit 0.
#include "kfm/ai/agent_host.hpp"
#include "kfm/ai/bot_spec.hpp"
#include "kfm/content/app_content.hpp"
#include "kfm/net/lobby_link.hpp"
#include "kfm/net/net_server.hpp"
#include "kfm/net/webrtc_transport.hpp"
#include "kfm/sim/content.hpp"
#include "server_host.hpp"
#include "server_lobby.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

// An environment variable, or empty when unset. MSVC deprecates getenv
// (C4996, fatal under /WX) in favour of _dupenv_s; everywhere else getenv is
// fine here because nothing has started a thread yet.
[[nodiscard]] std::string readEnv(const char* name) {
#ifdef _MSC_VER
  char* value = nullptr;
  size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return {};
  std::string out(value);
  std::free(value);
  return out;
#else
  // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded at this point
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
#endif
}

using namespace kfm;

struct Options {
  uint16_t port{9000};
  bool portSet{false};
  std::string bind;
  std::string stun;
  uint16_t icePortBegin{0};
  uint16_t icePortEnd{0};
  std::string lobby;  // lobby mode when set
  std::string lobbySecret;
  std::string region;
  std::string name;
  uint64_t seed{0};
  bool seedSet{false};
  std::vector<ai::BotSpec> botSpecs;  // --bot, in order
  uint32_t bots{0};
  uint64_t botsSeed{0xB07B07};
  std::optional<uint8_t> botDelay;  // nullopt = auto
  uint32_t fuel{0};                 // 0 = the runtime default
  ai::RespawnMemory memory{ai::RespawnMemory::Keep};
  uint32_t maxPlayers{sim::kMaxPlayers};
  std::vector<std::string> tokens;
  uint32_t restartDelaySeconds{3};
  uint32_t exitAfterSeconds{0};
  uint32_t exitAfterMatches{0};
  uint32_t statsEverySeconds{0};
  std::string contentDir;  // empty: next to the executable, else embedded
  std::string arena;       // names; empty = the first one
  std::string mode;
  std::string loadout;
  std::string powerUps;   // --powerups list; empty = all
  bool autoStart{false};  // v0.42: the M3 policy instead of the room
  bool verbose{false};
};

constexpr const char* kUsage =
    "usage: kfm_server [--port P] [--bind ADDR] [--stun URL] [--seed S] [--bot SPEC]...\n"
    "                  [--bots N] [--bots-seed S] [--bot-delay auto|N] [--fuel N]\n"
    "                  [--memory-on-respawn keep|wipe] [--max-players N] [--token T]... "
    "[--restart-delay S]\n"
    "                  [--exit-after-seconds N] [--exit-after-matches N] [--stats-every S]\n"
    "                  [--content DIR] [--arena NAME] [--mode NAME] [--loadout NAME]\n"
    "                  [--powerups LIST] [--auto-start] [--verbose] [--ice-ports A-B]\n"
    "                  [--lobby URL --lobby-secret S --region R [--name N]]\n"
    "  --bot SPEC        ref, ref:easy|medium|hard or path.wasm (repeatable, slot order)\n"
    "  --bots N          at least N bots: the --bot list padded with ref:hard\n"
    "  --bot-delay V     auto (default: the human clients' lead, 0 without one) or ticks\n"
    "  --fuel N          a bot module's wasm instructions per tick (default 100000)\n"
    "  --powerups LIST   all (default), none or comma-separated power-up names\n"
    "  --ice-ports A-B   UDP ports ICE may use (default any)\n"
    "  --auto-start      run matches back to back (the M3 policy) instead of a room that\n"
    "                    waits for its host's Start; implied by --exit-after-matches\n"
    "  --lobby URL       lobby mode: dial URL/server (ws://host:port, wss://host); no\n"
    "                    --port/--bind/--token; --lobby-secret (or env KFM_LOBBY_SECRET)\n"
    "                    and --region required, --name defaults to hostname:pid\n";

// How long peers get to receive Disconnect{ServerShuttingDown} before the
// connections are closed.
constexpr auto kShutdownLinger = std::chrono::milliseconds(300);
constexpr auto kSleepSlice = std::chrono::milliseconds(1);

[[nodiscard]] uint64_t nowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

// Decimal or 0x-prefixed hexadecimal.
template <typename T>
[[nodiscard]] bool parseNumber(std::string_view v, T& out) {
  int base = 10;
  if (v.starts_with("0x") || v.starts_with("0X")) {
    v.remove_prefix(2);
    base = 16;
  }
  if (v.empty()) return false;
  const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), out, base);
  return ec == std::errc{} && end == v.data() + v.size();
}

[[nodiscard]] bool parseArgs(int argc, const char* const* argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool hasValue = i + 1 < argc;
    bool ok = true;
    if (arg == "--verbose") {
      o.verbose = true;
      continue;
    }
    if (arg == "--auto-start") {
      o.autoStart = true;
      continue;
    }
    if (arg == "--help" || arg == "-h") {
      std::printf("%s", kUsage);
      std::exit(EXIT_SUCCESS);  // NOLINT(concurrency-mt-unsafe): single-threaded at this point
    }
    if (!hasValue) {
      std::fprintf(stderr, "error: unknown or incomplete argument '%s'\n%s", argv[i], kUsage);
      return false;
    }
    const std::string_view v = argv[++i];
    if (arg == "--port") {
      ok = parseNumber(v, o.port);
      o.portSet = true;
    } else if (arg == "--ice-ports") {
      const size_t dash = v.find('-');
      ok = dash != std::string_view::npos && parseNumber(v.substr(0, dash), o.icePortBegin) &&
           parseNumber(v.substr(dash + 1), o.icePortEnd) && o.icePortBegin > 0 &&
           o.icePortBegin <= o.icePortEnd;
    } else if (arg == "--lobby") {
      ok = v.starts_with("ws://") || v.starts_with("wss://");
      o.lobby = v;
    } else if (arg == "--lobby-secret") {
      ok = !v.empty();
      o.lobbySecret = v;
    } else if (arg == "--region") {
      ok = !v.empty();
      o.region = v;
    } else if (arg == "--name") {
      ok = !v.empty();
      o.name = v;
    } else if (arg == "--bind") {
      o.bind = v;
    } else if (arg == "--stun") {
      o.stun = v;
    } else if (arg == "--seed") {
      ok = parseNumber(v, o.seed);
      o.seedSet = true;
    } else if (arg == "--bot") {
      const std::optional<ai::BotSpec> spec = ai::parseBotSpec(v);
      ok = spec.has_value() && o.botSpecs.size() < sim::kMaxPlayers;
      if (spec) o.botSpecs.push_back(*spec);
    } else if (arg == "--bots") {
      ok = parseNumber(v, o.bots) && o.bots <= sim::kMaxPlayers;
    } else if (arg == "--bots-seed") {
      ok = parseNumber(v, o.botsSeed);
    } else if (arg == "--bot-delay") {
      uint32_t d = 0;
      if (v == "auto") {
        o.botDelay.reset();
      } else {
        ok = parseNumber(v, d) && d <= ai::kMaxAgentDelayTicks;
        o.botDelay = static_cast<uint8_t>(d);
      }
    } else if (arg == "--fuel") {
      ok = parseNumber(v, o.fuel) && o.fuel > 0;
    } else if (arg == "--memory-on-respawn") {
      ok = v == "keep" || v == "wipe";
      o.memory = v == "wipe" ? ai::RespawnMemory::Wipe : ai::RespawnMemory::Keep;
    } else if (arg == "--max-players") {
      ok = parseNumber(v, o.maxPlayers) && o.maxPlayers >= 1 && o.maxPlayers <= sim::kMaxPlayers;
    } else if (arg == "--token") {
      ok = !v.empty() && v.size() <= net::kMaxTokenBytes;
      o.tokens.emplace_back(v);
    } else if (arg == "--restart-delay") {
      ok = parseNumber(v, o.restartDelaySeconds) && o.restartDelaySeconds <= 600;
    } else if (arg == "--exit-after-seconds") {
      ok = parseNumber(v, o.exitAfterSeconds);
    } else if (arg == "--exit-after-matches") {
      ok = parseNumber(v, o.exitAfterMatches);
    } else if (arg == "--stats-every") {
      ok = parseNumber(v, o.statsEverySeconds);
    } else if (arg == "--content") {
      o.contentDir = v;
    } else if (arg == "--arena") {
      o.arena = v;
    } else if (arg == "--mode") {
      o.mode = v;
    } else if (arg == "--loadout") {
      o.loadout = v;
    } else if (arg == "--powerups") {
      o.powerUps = v;
    } else {
      std::fprintf(stderr, "error: unknown argument '%s'\n%s", argv[i - 1], kUsage);
      return false;
    }
    if (!ok) {
      std::fprintf(stderr, "error: bad value '%s' for %s\n", argv[i], argv[i - 1]);
      return false;
    }
  }
  if (o.lobby.empty()) {
    if (!o.lobbySecret.empty() || !o.region.empty() || !o.name.empty()) {
      std::fprintf(stderr, "error: --lobby-secret, --region and --name need --lobby\n");
      return false;
    }
    return true;
  }
  if (o.portSet || !o.bind.empty() || !o.tokens.empty()) {
    std::fprintf(stderr, "error: --lobby excludes --port, --bind and --token\n");
    return false;
  }
  if (o.lobbySecret.empty()) o.lobbySecret = readEnv("KFM_LOBBY_SECRET");
  if (o.lobbySecret.empty()) {
    std::fprintf(stderr, "error: --lobby needs --lobby-secret or KFM_LOBBY_SECRET\n");
    return false;
  }
  if (o.region.empty()) {
    std::fprintf(stderr, "error: --lobby needs --region\n");
    return false;
  }
  return true;
}

// hostname:pid, the default --name.
[[nodiscard]] std::string defaultName() {
  std::string host;
#ifdef _WIN32
  host = readEnv("COMPUTERNAME");
  const int pid = _getpid();
#else
  std::array<char, 256> buf{};
  if (gethostname(buf.data(), buf.size() - 1) == 0) host = buf.data();
  const int pid = static_cast<int>(getpid());
#endif
  if (host.empty()) host = "kfm_server";
  return host + ":" + std::to_string(pid);
}

volatile std::sig_atomic_t gStop = 0;

extern "C" void onSignal(int /*signal*/) {
  gStop = 1;
}

[[nodiscard]] server::HostConfig hostConfig(const Options& o, const sim::MatchConfig& match) {
  server::HostConfig c;
  c.server.match = match;
  c.server.match.seed = o.seed;
  c.server.botSeed = o.botsSeed;
  c.server.maxPlayers = static_cast<uint8_t>(o.maxPlayers);
  if (!o.tokens.empty()) {
    c.server.acceptToken = [tokens = o.tokens](std::span<const std::byte> t) {
      const auto same = [&](const std::string& want) {
        return std::ranges::equal(
            t, want, [](std::byte b, char ch) { return b == static_cast<std::byte>(ch); });
      };
      return std::ranges::any_of(tokens, same);
    };
  }
  c.bots = o.botSpecs;
  while (c.bots.size() < o.bots)
    c.bots.push_back(ai::BotSpec{});  // ref:hard
  c.botDelayTicks = o.botDelay;
  c.botFuelPerTick = o.fuel;
  c.botMemory = o.memory;
  c.restartDelayTicks = o.restartDelaySeconds * sim::kTickHz;
  c.exitAfterMatches = o.exitAfterMatches;
  // v0.42: the room unless --auto-start (or a soak that counts matches).
  c.room = !o.autoStart && o.exitAfterMatches == 0;
  c.statsEveryNs = uint64_t{o.statsEverySeconds} * 1'000'000'000ull;
  c.verbose = o.verbose;
  return c;
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(misc-const-correctness): main signature
  Options o;
  if (!parseArgs(argc, argv, o)) return EXIT_FAILURE;
  if (!o.seedSet) {
    o.seed = server::nextMatchSeed(
        static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()));
  }
  content::LoadedContent loaded;
  sim::MatchConfig match{};
  {
    content::Diagnostics diag;
    const bool ok =
        content::loadAppContent(o.contentDir, content::executableDir(argv[0]), loaded, diag);
    if (!diag.items().empty()) std::fprintf(stderr, "%s", diag.format().c_str());
    std::string error;
    if (!ok || !content::resolveMatchNames(loaded.db, o.arena, o.mode, match, error) ||
        !content::resolveLoadoutName(loaded.db, o.loadout, match, error) ||
        !content::resolvePowerUpList(loaded.db, o.powerUps, match.powerUps, error)) {
      std::fprintf(stderr, "error: %s\n", ok ? error.c_str() : "the content failed to load");
      return EXIT_FAILURE;
    }
    std::printf("KFM_SERVER %s\n", loaded.describe().c_str());
    std::printf("KFM_SERVER power-ups: %s\n",
                content::powerUpListText(loaded.db, match.powerUps).c_str());
  }
  net::WebRtcTransport::initLogging(o.verbose ? net::WebRtcLogLevel::Info
                                              : net::WebRtcLogLevel::Error);

  const bool lobbyMode = !o.lobby.empty();
  net::WebRtcServerConfig tc;
  tc.signalingPort = o.port;
  tc.signalingBindAddress = o.bind;
  tc.stunServer = o.stun;
  tc.icePortBegin = o.icePortBegin;
  tc.icePortEnd = o.icePortEnd;
  tc.externalSignaling = lobbyMode;  // no signaling socket at all
  auto transport = net::WebRtcTransport::createServer(tc);
  if (!transport) {
    std::fprintf(stderr, "error: %s\n", transport.error().c_str());
    return EXIT_FAILURE;
  }

  const uint64_t start = nowNs();
  server::HostConfig hc = hostConfig(o, match);
  std::unique_ptr<net::LobbyLink> link;
  if (lobbyMode) {
    net::LobbyLinkConfig lc;
    lc.url = o.lobby;
    lc.secret = o.lobbySecret;
    lc.region = o.region;
    lc.name = o.name.empty() ? defaultName() : o.name;
    lc.buildId = hc.server.buildId;
    lc.contentHash = sim::contentHash(loaded.db);
    // A waiting room places no bots: every slot is open to humans.
    lc.capacity = server::lobbyCapacity(
        hc.server.maxPlayers,
        hc.room ? size_t{0} : std::min<size_t>(hc.bots.size(), hc.server.maxPlayers));
    auto created = net::LobbyLink::create(**transport, lc, start);
    if (!created) {
      std::fprintf(stderr, "error: %s\n", created.error().c_str());
      return EXIT_FAILURE;
    }
    link = std::move(*created);
    hc.server.acceptToken = server::lobbyTokenCheck(*link);
    hc.statsSuffix = server::lobbyStatsSuffix(*link);
    std::printf("KFM_SERVER lobby %s region=%s name=%s\n", link->url().c_str(), lc.region.c_str(),
                lc.name.c_str());
  } else {
    std::printf("KFM_SERVER listening ws://%s:%u\n", o.bind.empty() ? "0.0.0.0" : o.bind.c_str(),
                unsigned{(*transport)->listenPort()});
  }
  std::fflush(stdout);

  (void)std::signal(SIGINT, onSignal);
  (void)std::signal(SIGTERM, onSignal);

  auto created = server::ServerHost::create(**transport, loaded.db, std::move(hc), start);
  if (!created) {
    std::fprintf(stderr, "error: %s\n", created.error().c_str());
    return EXIT_FAILURE;
  }
  std::unique_ptr<server::ServerHost> host = std::move(*created);
  std::unique_ptr<server::LobbyAdapter> lobby;
  if (link) {
    lobby =
        std::make_unique<server::LobbyAdapter>(*host, *link, static_cast<uint8_t>(o.maxPlayers));
  }
  const uint64_t limit = uint64_t{o.exitAfterSeconds} * 1'000'000'000ull;
  while (gStop == 0 && !host->done() && (limit == 0 || nowNs() - start < limit)) {
    host->frame(nowNs());
    if (lobby) lobby->frame(nowNs());
    // Sleep in small slices: the transport's callbacks queue packets meanwhile.
    std::this_thread::sleep_for(kSleepSlice);
  }

  host->beginShutdown();
  const auto lingerEnd = std::chrono::steady_clock::now() + kShutdownLinger;
  while (std::chrono::steady_clock::now() < lingerEnd) {
    host->frame(nowNs());
    std::this_thread::sleep_for(kSleepSlice);
  }
  host->finishShutdown();
  if (lobby) lobby->frame(nowNs());  // tells the lobby the peers are gone
  host->logStats(nowNs());
  std::printf("KFM_SERVER exiting after %llu ticks, %u matches\n",
              static_cast<unsigned long long>(host->stats().ticks), host->stats().matchesCompleted);
  std::fflush(stdout);
  lobby.reset();
  host.reset();
  link.reset();
  transport->reset();
  return EXIT_SUCCESS;
}
