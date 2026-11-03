// kfm_net_probe: end-to-end check of WebRtcTransport (architecture §6.4)
// between a native server and a native or browser client.
//
//   kfm_net_probe --serve PORT [--bind ADDR] [--exit-after-seconds N] [--verbose]  (native)
//   kfm_net_probe --connect ws://HOST:PORT [--messages N] [--timeout-seconds S]
//                 [--min-unreliable-percent P]
//   kfm_net_probe --loopback [--messages N] [--timeout-seconds S]           (native)
//
// --serve     echo server on ADDR:PORT (default 0.0.0.0; PORT 0 = ephemeral):
//             prints "KFM_NET_PROBE serving ws://ADDR:PORT" once listening,
//             one line per connect/disconnect with per-peer counters, and
//             runs until SIGINT/SIGTERM or N seconds.
// --connect   the client check (probe_client.hpp): N unreliable + N reliable
//             messages, echoes verified, one "KFM_NET_PROBE PASS|FAIL ..."
//             line; exit code 0 on PASS.
// --verbose   (native) libdatachannel logs at Debug instead of Error.
// --loopback  both in one process on 127.0.0.1 and an ephemeral port (the
//             native ctest net_probe_loopback).
//
// In the browser (kfm_net_probe.html) only --connect exists; shell.html turns
// ?connect=ws://127.0.0.1:9000&messages=200 into those arguments. The probe
// runs from emscripten_set_main_loop on the main thread and publishes its
// final line as window.kfmNetProbeResult.
#include "kfm/net/webrtc_transport.hpp"
#include "probe_client.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#include "echo_server.hpp"

#include <csignal>
#include <thread>
#endif

namespace {

using namespace kfm;

enum class Mode : uint8_t { None = 0, Serve, Connect, Loopback };

struct Options {
  Mode mode{Mode::None};
  uint16_t port{0};
  std::string bind{"0.0.0.0"};
  uint32_t exitAfterSeconds{0};  // 0: until a signal
  bool verbose{false};           // native: libdatachannel logs at Debug
  netprobe::ProbeConfig probe;
};

constexpr const char* kUsage =
    "usage: kfm_net_probe --serve PORT [--bind ADDR] [--exit-after-seconds N] [--verbose]\n"
    "       kfm_net_probe --connect ws://HOST:PORT [--messages N] [--timeout-seconds S]\n"
    "                     [--min-unreliable-percent P]\n"
    "       kfm_net_probe --loopback [--messages N] [--timeout-seconds S]\n";

[[nodiscard]] uint64_t nowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

template <typename T>
[[nodiscard]] bool parseNumber(std::string_view v, T& out) {
  const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
  return ec == std::errc{} && end == v.data() + v.size();
}

[[nodiscard]] bool parseArgs(int argc, const char* const* argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool hasValue = i + 1 < argc;
    bool ok = true;
    uint32_t seconds = 0;
    if (arg == "--serve" && hasValue) {
      o.mode = Mode::Serve;
      ok = parseNumber(argv[++i], o.port);
    } else if (arg == "--connect" && hasValue) {
      o.mode = Mode::Connect;
      o.probe.url = argv[++i];
    } else if (arg == "--loopback") {
      o.mode = Mode::Loopback;
    } else if (arg == "--verbose") {
      o.verbose = true;
    } else if (arg == "--bind" && hasValue) {
      o.bind = argv[++i];
    } else if (arg == "--exit-after-seconds" && hasValue) {
      ok = parseNumber(argv[++i], o.exitAfterSeconds);
    } else if (arg == "--messages" && hasValue) {
      ok = parseNumber(argv[++i], o.probe.messages) && o.probe.messages > 0;
    } else if (arg == "--timeout-seconds" && hasValue) {
      ok = parseNumber(argv[++i], seconds) && seconds > 0 && seconds < 3600;
      o.probe.timeoutMs = seconds * 1000U;
    } else if (arg == "--min-unreliable-percent" && hasValue) {
      ok = parseNumber(argv[++i], o.probe.minUnreliablePercent) &&
           o.probe.minUnreliablePercent <= 100;
    } else {
      std::fprintf(stderr, "error: unknown or incomplete argument '%s'\n%s", argv[i], kUsage);
      return false;
    }
    if (!ok) {
      std::fprintf(stderr, "error: bad value '%s' for %s\n", argv[i], argv[i - 1]);
      return false;
    }
  }
  if (o.mode == Mode::None) {
    std::fprintf(stderr, "%s", kUsage);
    return false;
  }
  return true;
}

#ifdef __EMSCRIPTEN__

// The page reads the result from window.kfmNetProbeResult (a JS string).
void publishResult(const std::string& line) {
  std::string script = "window.kfmNetProbeResult = \"";
  for (const char c : line) {
    if (c == '"' || c == '\\') {
      script += '\\';
      script += c;
    } else if (static_cast<unsigned char>(c) >= 0x20) {
      script += c;
    }
  }
  script += "\";";
  emscripten_run_script(script.c_str());
}

std::unique_ptr<netprobe::ProbeClient>& browserProbe() {
  static std::unique_ptr<netprobe::ProbeClient> probe;
  return probe;
}

void browserFrame() {
  auto& probe = browserProbe();
  if (!probe || !probe->tick(nowNs())) return;
  std::printf("%s\n", probe->summary().c_str());
  publishResult(probe->summary());
  probe->close();
  emscripten_cancel_main_loop();
}

#else

volatile std::sig_atomic_t gStop = 0;

extern "C" void onSignal(int /*signal*/) {
  gStop = 1;
}

constexpr auto kNativeStep = std::chrono::milliseconds(1);

[[nodiscard]] int serve(const Options& o) {
  auto server = netprobe::EchoServer::create(o.port, o.bind);
  if (!server) {
    std::fprintf(stderr, "error: %s\n", server.error().c_str());
    return EXIT_FAILURE;
  }
  std::printf("KFM_NET_PROBE serving ws://%s:%u\n", o.bind.c_str(), unsigned{(*server)->port()});
  std::fflush(stdout);
  (void)std::signal(SIGINT, onSignal);
  (void)std::signal(SIGTERM, onSignal);
  const uint64_t start = nowNs();
  const uint64_t limit = uint64_t{o.exitAfterSeconds} * 1'000'000'000ULL;
  while (gStop == 0 && (limit == 0 || nowNs() - start < limit)) {
    (*server)->tick(nowNs());
    std::this_thread::sleep_for(kNativeStep);
  }
  (*server)->report();
  std::printf("server: exiting\n");
  return EXIT_SUCCESS;
}

[[nodiscard]] int connect(const Options& o, netprobe::EchoServer* server) {
  auto probe = netprobe::ProbeClient::create(o.probe);
  if (!probe) {
    std::printf("KFM_NET_PROBE FAIL: %s\n", probe.error().c_str());
    return EXIT_FAILURE;
  }
  while (!(*probe)->tick(nowNs())) {
    if (server != nullptr) server->tick(nowNs());
    std::this_thread::sleep_for(kNativeStep);
  }
  std::printf("%s\n", (*probe)->summary().c_str());
  std::fflush(stdout);
  const bool passed = (*probe)->passed();
  (*probe)->close();
  if (server != nullptr) {
    // Let the server see the client leave and log its counters.
    const uint64_t deadline = nowNs() + 5'000'000'000ULL;
    while (server->disconnects() < server->connects() && nowNs() < deadline) {
      server->tick(nowNs());
      std::this_thread::sleep_for(kNativeStep);
    }
    server->report();
  }
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

[[nodiscard]] int loopback(Options o) {
  auto server = netprobe::EchoServer::create(0, "127.0.0.1");
  if (!server) {
    std::printf("KFM_NET_PROBE FAIL: %s\n", server.error().c_str());
    return EXIT_FAILURE;
  }
  o.probe.url = "ws://127.0.0.1:" + std::to_string((*server)->port());
  std::printf("KFM_NET_PROBE loopback on %s\n", o.probe.url.c_str());
  return connect(o, server->get());
}

#endif

}  // namespace

int main(int argc, char** argv) {  // NOLINT(misc-const-correctness): main signature
  Options options;
  if (!parseArgs(argc, argv, options)) {
#ifdef __EMSCRIPTEN__
    publishResult("KFM_NET_PROBE FAIL: bad arguments (use ?connect=ws://host:port&messages=N)");
#endif
    return EXIT_FAILURE;
  }
#ifdef __EMSCRIPTEN__
  if (options.mode != Mode::Connect) {
    const std::string line = "KFM_NET_PROBE FAIL: only --connect runs in the browser";
    std::printf("%s\n", line.c_str());
    publishResult(line);
    return EXIT_FAILURE;
  }
  auto probe = netprobe::ProbeClient::create(options.probe);
  if (!probe) {
    const std::string line = "KFM_NET_PROBE FAIL: " + probe.error();
    std::printf("%s\n", line.c_str());
    publishResult(line);
    return EXIT_FAILURE;
  }
  browserProbe() = std::move(*probe);
  std::printf("connecting to %s, %u messages per lane\n", options.probe.url.c_str(),
              options.probe.messages);
  // Driven by requestAnimationFrame; main() returns and the runtime stays.
  emscripten_set_main_loop(browserFrame, 0, false);
  return EXIT_SUCCESS;
#else
  // Teardown noise (SCTP stream resets, TCP closed) is logged at Warning.
  net::WebRtcTransport::initLogging(options.verbose ? net::WebRtcLogLevel::Debug
                                                    : net::WebRtcLogLevel::Error);
  switch (options.mode) {
    case Mode::Serve:
      return serve(options);
    case Mode::Connect:
      return connect(options, nullptr);
    case Mode::Loopback:
      return loopback(options);
    case Mode::None:
      break;
  }
  return EXIT_FAILURE;
#endif
}
