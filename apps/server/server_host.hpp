// ServerHost: the dedicated server's loop around one NetServer (architecture
// §3, §6.5 minus the thread pool, §6.6, §13.5), headless and transport-
// agnostic so it is unit-tested over the loopback and run by kfm_server over
// WebRtcTransport. It owns no clock: the caller passes its monotonic nowNs to
// frame() as often as it likes (kfm_server: about every millisecond).
//
// Per frame: NetServer::pump(now); then every 60 Hz tick due on the host's
// drift-free clock (tick k is due at start + k·1e9/60 ns, at most
// maxCatchUpTicks per frame; a stall longer than that drops the backlog and
// counts it); then the server's events and the ticks' SimEvents become one
// log line each (joins, leaves, kicks, rejects, resyncs, round and match
// results), and every statsEveryNs one line of ServerStats deltas.
//
// Match policy (one match at a time, §11 M3): server-side bots fill the
// lowest free slots at start. When a match ends, the next one is announced
// at once and starts restartDelayTicks later (NetServer::restartMatch) with a
// seed derived from the old one; the roster carries over and the bot Rng is
// re-seeded. With exitAfterMatches set, the host reports done() instead of
// restarting once that many matches have ended.
//
// Shutdown: beginShutdown() sends Disconnect{ServerShuttingDown} to every
// peer; the caller keeps calling frame() (pump only, no ticks) for a short
// linger so the messages leave, then finishShutdown() closes every peer.
#pragma once

#include "kfm/core/types.hpp"
#include "kfm/net/net_server.hpp"
#include "kfm/net/transport.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/content.hpp"

#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

namespace kfm::server {

struct HostConfig {
  // Match seed, build id, token check, player cap and the netcode knobs.
  net::ServerConfig server{};
  // Server-side bots in the lowest free slots (capped by server.maxPlayers).
  uint8_t bots{0};
  // Pause between a MatchEnded and the next match's first tick (3 s).
  uint32_t restartDelayTicks{3 * sim::kTickHz};
  // Report done() after this many matches have ended (0 = never).
  uint32_t exitAfterMatches{0};
  // One stats line every this many ns (0 = never).
  uint64_t statsEveryNs{0};
  // Ticks run per frame at most; a larger backlog is dropped and counted.
  uint32_t maxCatchUpTicks{8};
  // Also log match and round starts and every peer in the stats line.
  bool verbose{false};
  // Receives one line per call, without a trailing newline. Empty: stdout.
  std::function<void(std::string_view)> log;
};

struct HostStats {
  uint64_t ticks{0};
  uint64_t droppedTicks{0};  // backlog beyond maxCatchUpTicks
  uint32_t matchesCompleted{0};
  uint32_t roundsCompleted{0};
};

// The seed of the match after one played with `seed` (splitmix64).
[[nodiscard]] uint64_t nextMatchSeed(uint64_t seed);

class ServerHost {
 public:
  ServerHost(net::ITransport& transport, const sim::ContentDB& content, HostConfig config,
             uint64_t nowNs);
  ~ServerHost() = default;
  ServerHost(const ServerHost&) = delete;
  ServerHost& operator=(const ServerHost&) = delete;
  ServerHost(ServerHost&&) = delete;
  ServerHost& operator=(ServerHost&&) = delete;

  // One host frame at nowNs (monotonic, never decreasing).
  void frame(uint64_t nowNs);
  // Time until the next tick is due (0 when one is due now).
  [[nodiscard]] uint64_t nsUntilNextTick(uint64_t nowNs) const;

  void beginShutdown();
  void finishShutdown();
  [[nodiscard]] bool shuttingDown() const { return server_->shuttingDown(); }
  // exitAfterMatches reached.
  [[nodiscard]] bool done() const { return done_; }

  [[nodiscard]] net::NetServer& server() { return *server_; }
  [[nodiscard]] const net::NetServer& server() const { return *server_; }
  [[nodiscard]] const HostStats& stats() const { return stats_; }
  // The line format of the periodic stats, for a final summary.
  void logStats(uint64_t nowNs);

 private:
  [[nodiscard]] uint64_t tickDueAt(uint64_t k) const;
  void onTick();
  void logServerEvents();
  // One line, prefixed with the host time and the next tick.
  template <class... Args>
  void log(std::format_string<Args...> fmt, Args&&... args) {
    emit(std::format(fmt, std::forward<Args>(args)...));
  }
  void emit(std::string_view line) const;

  HostConfig config_;
  std::unique_ptr<net::NetServer> server_;
  uint64_t startNs_{0};
  uint64_t nowNs_{0};
  uint64_t clockBaseNs_{0};  // tick clockTicks_ is due at clockBaseNs_ + ...
  uint64_t clockTicks_{0};
  uint64_t nextStatsNs_{0};
  uint64_t lastStatsNs_{0};
  net::ServerStats lastStats_{};
  HostStats lastHostStats_{};
  uint64_t botSeed_{0};
  bool done_{false};
  HostStats stats_{};
};

}  // namespace kfm::server
