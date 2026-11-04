// ServerHost over real sockets (label `network`): WebRtcTransport server on
// 127.0.0.1 with an ephemeral signaling port and two NetClients over
// WebRtcTransport clients in the same process, all driven from the steady
// clock. They play 5 s of scripted inputs and must converge: running,
// confirmed tick advancing, no checksum mismatch, no resync, no protocol
// error on the server.
#include "kfm/core/rng.hpp"
#include "kfm/net/net_client.hpp"
#include "kfm/net/webrtc_transport.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/scripted_inputs.hpp"
#include "server_host.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace kfm;
using namespace kfm::net;

namespace {

[[nodiscard]] uint64_t nowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

struct Player {
  std::unique_ptr<WebRtcTransport> transport;
  std::unique_ptr<NetClient> client;
  Rng rng{};
  std::optional<uint64_t> runningAtNs;

  void frame(uint64_t now) {
    NetClient& c = *client;
    c.beginFrame();
    c.pump(now);
    c.reconcile();
    for (uint32_t n = c.ticksDue(now); n > 0; --n) {
      std::array<sim::PlayerInput, sim::kMaxPlayers> in{};
      size_t k = 0;
      for (uint8_t s = 0; s < sim::kMaxPlayers; ++s) {
        if (sim::maskHas(c.localSlots(), PlayerId{s}))
          in[k++] = sim::scriptedInput(rng, c.world(), sim::defaultContent(), PlayerId{s});
      }
      c.tick(std::span<const sim::PlayerInput>(in.data(), k));
    }
    static_cast<void>(c.drainEvents());
    if (!runningAtNs && c.running()) runningAtNs = now;
  }
};

}  // namespace

TEST_CASE("ServerHost over WebRtcTransport: two clients converge on a real socket",
          "[server][webrtc]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  WebRtcServerConfig sc;
  sc.signalingPort = 0;
  sc.signalingBindAddress = "127.0.0.1";
  auto serverTransport = WebRtcTransport::createServer(sc);
  REQUIRE(serverTransport.has_value());
  const std::string url = "ws://127.0.0.1:" + std::to_string((*serverTransport)->listenPort());

  std::vector<std::string> lines;
  server::HostConfig hc;
  hc.server.match.seed = 0x50CE7;
  hc.bots = server::referenceBots(1);
  hc.log = [&](std::string_view l) { lines.emplace_back(l); };
  const uint64_t start = nowNs();
  auto created =
      server::ServerHost::create(**serverTransport, sim::defaultContent(), std::move(hc), start);
  if (!created) {
    FAIL(created.error());
    return;
  }
  server::ServerHost& host = **created;

  std::array<Player, 2> players;
  for (size_t i = 0; i < players.size(); ++i) {
    WebRtcClientConfig cc;
    cc.signalingUrl = url;
    auto t = WebRtcTransport::createClient(cc);
    REQUIRE(t.has_value());
    players[i].transport = std::move(*t);
    ClientConfig config;
    const std::string token = "player-" + std::to_string(i);
    config.setToken(std::as_bytes(std::span(token.data(), token.size())));
    players[i].client = std::make_unique<NetClient>(sim::defaultContent(), config);
    players[i].client->connect(*players[i].transport, nowNs());
    players[i].rng = Rng::seeded(0xAB0 + i);
  }

  const auto frame = [&] {
    const uint64_t now = nowNs();
    host.frame(now);
    for (Player& p : players)
      p.frame(now);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  };
  const auto allRunning = [&] {
    return players[0].client->running() && players[1].client->running();
  };
  const uint64_t connectDeadline = nowNs() + 20'000'000'000ull;
  while (!allRunning() && nowNs() < connectDeadline)
    frame();
  for (const std::string& l : lines)
    UNSCOPED_INFO(l);
  REQUIRE(allRunning());

  std::array<Tick, 2> confirmedAtStart{};
  for (size_t i = 0; i < players.size(); ++i)
    confirmedAtStart[i] = players[i].client->confirmedTick().value_or(Tick{});
  const uint64_t playEnd = nowNs() + 5'000'000'000ull;
  while (nowNs() < playEnd)
    frame();

  for (size_t i = 0; i < players.size(); ++i) {
    const NetClient& c = *players[i].client;
    const ClientNetStats& st = c.netStats();
    const Tick confirmed = c.confirmedTick().value_or(Tick{});
    std::printf(
        "[server-webrtc] client %zu: running after %.0f ms, confirmed tick %u -> %u, rtt %.2f "
        "ms, checksums compared %llu, mismatches %llu, resyncs %llu, rollbacks %llu (max depth "
        "%u)\n",
        i, static_cast<double>(players[i].runningAtNs.value_or(start) - start) / 1e6,
        confirmedAtStart[i].value, confirmed.value, static_cast<double>(st.rttNs) / 1e6,
        static_cast<unsigned long long>(st.checksumsCompared),
        static_cast<unsigned long long>(st.checksumMismatches),
        static_cast<unsigned long long>(st.resyncRequests),
        static_cast<unsigned long long>(st.rollbacks), st.maxRollbackDepth);
    INFO("client " << i);
    CHECK(c.running());
    CHECK(confirmed.minus(confirmedAtStart[i]) > int32_t{4 * sim::kTickHz});  // ~300 ticks in 5 s
    CHECK(st.checksumMismatches == 0);
    CHECK(st.resyncRequests == 0);
    CHECK(st.checksumsCompared > 20);
  }
  const ServerStats& ss = host.server().stats();
  std::printf("[server-webrtc] server: ticks %llu, protocol errors %llu, substitutions %llu\n",
              static_cast<unsigned long long>(ss.ticks),
              static_cast<unsigned long long>(ss.protocolErrors),
              static_cast<unsigned long long>(ss.substitutedInputs));
  CHECK(ss.protocolErrors == 0);
  CHECK(ss.joins == 2);
  CHECK(host.server().present() == 0b111);

  host.beginShutdown();
  const uint64_t lingerEnd = nowNs() + 2'000'000'000ull;
  while (nowNs() < lingerEnd && (players[0].client->running() || players[1].client->running()))
    frame();
  host.finishShutdown();
  for (const Player& p : players)
    CHECK(p.client->disconnectReason() == DisconnectReason::ServerShuttingDown);
}
