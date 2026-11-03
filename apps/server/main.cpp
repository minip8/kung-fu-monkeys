// kfm_server: the headless dedicated server (architecture §3, §6, §13.5).
// One match at a time over WebRtcTransport; clients signal on a plain
// ws:// WebSocket at --bind:--port and then talk WebRTC data channels.
//
//   kfm_server [--port P] [--bind ADDR] [--stun URL] [--seed S] [--bots N]
//              [--bots-seed S] [--max-players N] [--token T]... [--restart-delay S]
//              [--exit-after-seconds N] [--exit-after-matches N] [--stats-every S]
//              [--verbose]
//
// --port P             signaling port (default 9000; 0 = ephemeral, printed)
// --bind ADDR          signaling bind address (default all interfaces)
// --stun URL           "stun:host:port" for the server's ICE (default none)
// --seed S             first match seed (default: from the clock, printed)
// --bots N             server-side bots in the lowest free slots (default 0)
// --bots-seed S        seed of the bots' input generator (default 0xB07B07)
// --max-players N      slots open to clients and bots, 1..10 (default 10)
// --token T            accept only these connection tokens (repeatable;
//                      none = accept every client)
// --restart-delay S    seconds between a match's end and the next (default 3)
// --exit-after-seconds N, --exit-after-matches N   stop cleanly (0 = never)
// --stats-every S      one line of server stat deltas every S seconds
// --verbose            match/round starts, per-peer stats, libdatachannel at Info
//
// Prints "KFM_SERVER listening ws://ADDR:PORT" once the socket is bound. The
// content is sim::defaultContent() and the build id 0, which a client must
// match (Reject otherwise). SIGINT/SIGTERM disconnect every peer with
// ServerShuttingDown and exit 0.
#include "kfm/net/net_server.hpp"
#include "kfm/net/webrtc_transport.hpp"
#include "kfm/sim/content.hpp"
#include "server_host.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace kfm;

struct Options {
  uint16_t port{9000};
  std::string bind;
  std::string stun;
  uint64_t seed{0};
  bool seedSet{false};
  uint32_t bots{0};
  uint64_t botsSeed{0xB07B07};
  uint32_t maxPlayers{sim::kMaxPlayers};
  std::vector<std::string> tokens;
  uint32_t restartDelaySeconds{3};
  uint32_t exitAfterSeconds{0};
  uint32_t exitAfterMatches{0};
  uint32_t statsEverySeconds{0};
  bool verbose{false};
};

constexpr const char* kUsage =
    "usage: kfm_server [--port P] [--bind ADDR] [--stun URL] [--seed S] [--bots N]\n"
    "                  [--bots-seed S] [--max-players N] [--token T]... [--restart-delay S]\n"
    "                  [--exit-after-seconds N] [--exit-after-matches N] [--stats-every S]\n"
    "                  [--verbose]\n";

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
    } else if (arg == "--bind") {
      o.bind = v;
    } else if (arg == "--stun") {
      o.stun = v;
    } else if (arg == "--seed") {
      ok = parseNumber(v, o.seed);
      o.seedSet = true;
    } else if (arg == "--bots") {
      ok = parseNumber(v, o.bots) && o.bots <= sim::kMaxPlayers;
    } else if (arg == "--bots-seed") {
      ok = parseNumber(v, o.botsSeed);
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
    } else {
      std::fprintf(stderr, "error: unknown argument '%s'\n%s", argv[i - 1], kUsage);
      return false;
    }
    if (!ok) {
      std::fprintf(stderr, "error: bad value '%s' for %s\n", argv[i], argv[i - 1]);
      return false;
    }
  }
  return true;
}

volatile std::sig_atomic_t gStop = 0;

extern "C" void onSignal(int /*signal*/) {
  gStop = 1;
}

[[nodiscard]] server::HostConfig hostConfig(const Options& o) {
  server::HostConfig c;
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
  c.bots = static_cast<uint8_t>(o.bots);
  c.restartDelayTicks = o.restartDelaySeconds * sim::kTickHz;
  c.exitAfterMatches = o.exitAfterMatches;
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
  net::WebRtcTransport::initLogging(o.verbose ? net::WebRtcLogLevel::Info
                                              : net::WebRtcLogLevel::Error);

  net::WebRtcServerConfig tc;
  tc.signalingPort = o.port;
  tc.signalingBindAddress = o.bind;
  tc.stunServer = o.stun;
  auto transport = net::WebRtcTransport::createServer(tc);
  if (!transport) {
    std::fprintf(stderr, "error: %s\n", transport.error().c_str());
    return EXIT_FAILURE;
  }
  std::printf("KFM_SERVER listening ws://%s:%u\n", o.bind.empty() ? "0.0.0.0" : o.bind.c_str(),
              unsigned{(*transport)->listenPort()});
  std::fflush(stdout);

  (void)std::signal(SIGINT, onSignal);
  (void)std::signal(SIGTERM, onSignal);

  const uint64_t start = nowNs();
  auto host = std::make_unique<server::ServerHost>(**transport, sim::defaultContent(),
                                                   hostConfig(o), start);
  const uint64_t limit = uint64_t{o.exitAfterSeconds} * 1'000'000'000ull;
  while (gStop == 0 && !host->done() && (limit == 0 || nowNs() - start < limit)) {
    host->frame(nowNs());
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
  host->logStats(nowNs());
  std::printf("KFM_SERVER exiting after %llu ticks, %u matches\n",
              static_cast<unsigned long long>(host->stats().ticks), host->stats().matchesCompleted);
  std::fflush(stdout);
  host.reset();
  transport->reset();
  return EXIT_SUCCESS;
}
