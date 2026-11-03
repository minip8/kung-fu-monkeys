// NetSession over real sockets (architecture §6.4, §11 M3): a NetServer on
// WebRtcTransport::createServer (ephemeral port on 127.0.0.1) and a
// NetSession on WebRtcTransport::createClient, driven in one process on the
// wall clock. Labelled `network` (excluded by `ctest -LE network`).
#include "core/net_session.hpp"
#include "kfm/net/net_server.hpp"
#include "kfm/net/webrtc_transport.hpp"
#include "kfm/platform/input_event.hpp"
#include "kfm/platform/input_sampler.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/sim/content.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

using namespace kfm;
using client::NetPhase;
using client::NetSession;
using client::NetSessionConfig;

namespace {

[[nodiscard]] uint64_t nowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

constexpr uint64_t kMs = 1'000'000;
constexpr uint64_t kSec = 1'000 * kMs;
constexpr uint64_t kFrameNs = 16'666'667;

}  // namespace

TEST_CASE("a NetSession plays over WebRtcTransport against an in-process NetServer",
          "[client][net][network]") {
  net::WebRtcTransport::initLogging(net::WebRtcLogLevel::Error);
  net::WebRtcServerConfig sc;
  sc.signalingPort = 0;
  sc.signalingBindAddress = "127.0.0.1";
  auto serverTransport = net::WebRtcTransport::createServer(sc);
  REQUIRE(serverTransport.has_value());
  const std::string url = "ws://127.0.0.1:" + std::to_string((*serverTransport)->listenPort());

  net::NetServer server(**serverTransport, sim::defaultContent(), net::ServerConfig{});
  REQUIRE(server.setBot(PlayerId{0}, true));

  net::WebRtcClientConfig cc;
  cc.signalingUrl = url;
  auto clientTransport = net::WebRtcTransport::createClient(cc);
  REQUIRE(clientTransport.has_value());
  net::ITransport* t = clientTransport->get();

  platform::InputSampler lobby;
  REQUIRE(lobby.bind(PlayerId{0}, platform::DeviceBinding::keyboard(0)));
  NetSessionConfig config;
  config.serverLabel = url;
  const uint64_t start = nowNs();
  NetSession session(config, lobby, t, start, std::move(*clientTransport));
  auto rs = std::make_unique<presentation::RenderState>();

  uint64_t serverTicks = 0;
  uint64_t nextFrame = start;
  uint64_t runningSince = 0;
  uint32_t confirmedAtStart = 0;
  bool pressed = false;
  bool released = false;
  // The server ticks on the wall clock; the session runs a host frame every
  // 16.7 ms; 3 s of play once it runs (20 s budget for ICE + DTLS).
  while (true) {
    const uint64_t now = nowNs();
    server.pump(now);
    while (now - start >= serverTicks * kSec / sim::kTickHz) {
      server.tick(now);
      ++serverTicks;
    }
    server.clearEvents();
    if (now >= nextFrame) {
      nextFrame += kFrameNs;
      session.advance(now);
      session.build(static_cast<float>(kFrameNs) * 1e-9f, *rs);
      if (session.running() && runningSince == 0) {
        runningSince = now;
        confirmedAtStart = session.client().confirmedTick().value_or(Tick{}).value;
      }
      if (runningSince != 0 && !pressed && now - runningSince > kSec) {
        platform::InputEvent e{};
        e.kind = platform::InputEventKind::KeyDown;
        e.key = platform::Key::D;
        e.timestampNs = now;
        session.push(e);
        pressed = true;
      }
      if (pressed && !released && now - runningSince > 2 * kSec) {
        platform::InputEvent e{};
        e.kind = platform::InputEventKind::KeyUp;
        e.key = platform::Key::D;
        e.timestampNs = now;
        session.push(e);
        released = true;
      }
    }
    if (runningSince != 0 && now - runningSince > 3 * kSec) break;
    if (runningSince == 0 && now - start > 20 * kSec) break;
    if (session.phase() == NetPhase::Rejected || session.phase() == NetPhase::Disconnected) break;
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }

  INFO(session.statusText());
  REQUIRE(session.running());
  const net::ClientNetStats& st = session.client().netStats();
  const auto confirmed = session.client().confirmedTick();
  REQUIRE(confirmed.has_value());
  std::printf(
      "[client-webrtc] synced in %.0f ms; 3 s running: %llu ticks, confirmed %u -> %u, server "
      "tick %u; RTT %.2f ms (jitter %.2f); rollbacks %llu (max depth %u), mispredicted ticks "
      "%llu, substitutions %llu; checksums compared %llu, mismatches %llu, resyncs %llu; bytes "
      "up %llu down %llu\n",
      static_cast<double>(runningSince - start) * 1e-6, static_cast<unsigned long long>(st.ticks),
      confirmedAtStart, confirmed->value, server.nextTick().value,
      static_cast<double>(st.rttNs) * 1e-6, static_cast<double>(st.jitterNs) * 1e-6,
      static_cast<unsigned long long>(st.rollbacks), st.maxRollbackDepth,
      static_cast<unsigned long long>(st.mispredictedTicks),
      static_cast<unsigned long long>(st.substitutionsSeen),
      static_cast<unsigned long long>(st.checksumsCompared),
      static_cast<unsigned long long>(st.checksumMismatches),
      static_cast<unsigned long long>(st.resyncRequests),
      static_cast<unsigned long long>(st.bytesSent),
      static_cast<unsigned long long>(st.bytesReceived));
  CHECK(confirmed->value > confirmedAtStart + 120);  // ~3 s at 60 Hz, generous margin
  CHECK(st.checksumsCompared > 0);
  CHECK(st.checksumMismatches == 0);
  CHECK(st.resyncRequests == 0);
  CHECK(session.localSlots() == 0b10);
  session.disconnect();
}
