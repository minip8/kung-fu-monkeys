#include "server_host.hpp"

#include "kfm/sim/events.hpp"
#include "kfm/sim/world_state.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace kfm::server {
namespace {

constexpr uint64_t kNsPerSecond = 1'000'000'000ull;

[[nodiscard]] std::string slotList(sim::PlayerMask mask) {
  std::string out;
  for (uint8_t s = 0; s < sim::kMaxPlayers; ++s) {
    if (!sim::maskHas(mask, PlayerId{s})) continue;
    if (!out.empty()) out += ',';
    out += std::to_string(s);
  }
  return out.empty() ? std::string("-") : out;
}

[[nodiscard]] const char* rejectName(uint8_t reason) {
  switch (static_cast<net::RejectReason>(reason)) {
    case net::RejectReason::ProtocolMismatch:
      return "protocol-mismatch";
    case net::RejectReason::ContentMismatch:
      return "content-mismatch";
    case net::RejectReason::BuildMismatch:
      return "build-mismatch";
    case net::RejectReason::BadToken:
      return "bad-token";
    case net::RejectReason::MatchFull:
      return "match-full";
    case net::RejectReason::ServerShuttingDown:
      return "shutting-down";
  }
  return "unknown";
}

[[nodiscard]] const char* disconnectName(uint8_t reason) {
  switch (static_cast<net::DisconnectReason>(reason)) {
    case net::DisconnectReason::Quit:
      return "quit";
    case net::DisconnectReason::Kicked:
      return "kicked";
    case net::DisconnectReason::Timeout:
      return "timeout";
    case net::DisconnectReason::ProtocolError:
      return "protocol-error";
    case net::DisconnectReason::ServerShuttingDown:
      return "shutting-down";
  }
  return "unknown";
}

[[nodiscard]] std::string scoreList(const sim::WorldState& w) {
  std::string out;
  for (uint8_t s = 0; s < sim::kMaxPlayers; ++s) {
    if (!sim::maskHas(w.rules.present, PlayerId{s})) continue;
    if (!out.empty()) out += ' ';
    out += std::format("{}:{}", s, w.rules.scores[s]);
  }
  return out;
}

[[nodiscard]] double perSecond(uint64_t delta, uint64_t ns) {
  return ns == 0 ? 0.0 : static_cast<double>(delta) * 1e9 / static_cast<double>(ns);
}

}  // namespace

uint64_t nextMatchSeed(uint64_t seed) {
  uint64_t z = seed + 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

ServerHost::ServerHost(net::ITransport& transport, const sim::ContentDB& content, HostConfig config,
                       uint64_t nowNs)
    : config_(std::move(config)),
      startNs_(nowNs),
      nowNs_(nowNs),
      clockBaseNs_(nowNs),
      botSeed_(config_.server.botSeed) {
  server_ = std::make_unique<net::NetServer>(transport, content, config_.server);
  uint8_t placed = 0;
  for (uint8_t s = 0; s < sim::kMaxPlayers && placed < config_.bots; ++s) {
    if (server_->setBot(PlayerId{s}, true)) ++placed;
  }
  lastStatsNs_ = nowNs;
  nextStatsNs_ = config_.statsEveryNs > 0 ? nowNs + config_.statsEveryNs : 0;
  log("start seed=0x{:x} arena={} mode={} bots={} slots={} max_players={} tokens={}",
      config_.server.match.seed, content.arenaAt(config_.server.match.arena).name.view(),
      content.modeAt(config_.server.match.mode).name.view(), placed, slotList(server_->bots()),
      config_.server.maxPlayers, config_.server.acceptToken ? "checked" : "any");
}

uint64_t ServerHost::tickDueAt(uint64_t k) const {
  // Exact: tick k at k·1e9/kTickHz ns after the base, no accumulated rounding.
  return clockBaseNs_ + k * kNsPerSecond / sim::kTickHz;
}

uint64_t ServerHost::nsUntilNextTick(uint64_t nowNs) const {
  const uint64_t due = tickDueAt(clockTicks_);
  return due > nowNs ? due - nowNs : 0;
}

void ServerHost::frame(uint64_t nowNs) {
  nowNs_ = std::max(nowNs_, nowNs);
  server_->pump(nowNs_);
  if (!server_->shuttingDown()) {
    uint32_t ran = 0;
    while (nowNs_ >= tickDueAt(clockTicks_)) {
      if (ran == config_.maxCatchUpTicks) {
        // A stalled host must not run seconds of ticks at once: drop the
        // backlog (the clients' lead control absorbs the jump) and rebase.
        const uint64_t behind = (nowNs_ - tickDueAt(clockTicks_)) * sim::kTickHz / kNsPerSecond;
        stats_.droppedTicks += behind + 1;
        log("clock stalled: dropped {} ticks", behind + 1);
        clockBaseNs_ = nowNs_;
        clockTicks_ = 1;
        break;
      }
      server_->tick(nowNs_);
      ++clockTicks_;
      ++ran;
      ++stats_.ticks;
      onTick();
    }
  }
  logServerEvents();
  server_->clearEvents();
  if (nextStatsNs_ != 0 && nowNs_ >= nextStatsNs_) {
    logStats(nowNs_);
    nextStatsNs_ = std::max(nextStatsNs_ + config_.statsEveryNs, nowNs_);
  }
}

void ServerHost::onTick() {
  const sim::WorldState& w = server_->world();
  for (const sim::SimEvent& e : server_->tickEvents()) {
    switch (e.kind) {
      case sim::EventKind::MatchStarted:
        if (config_.verbose) {
          log("match {} started seed=0x{:x} players={}", server_->matchIndex() + 1,
              server_->matchConfig().seed, slotList(w.rules.present));
        }
        break;
      case sim::EventKind::RoundStarted:
        if (config_.verbose) log("round {} started", e.a);
        break;
      case sim::EventKind::RoundEnded:
        ++stats_.roundsCompleted;
        if (e.player.isValid()) {
          log("round {} won by slot {}; scores {}", e.a, e.player.value, scoreList(w));
        } else {
          log("round {} drawn; scores {}", e.a, scoreList(w));
        }
        break;
      case sim::EventKind::MatchEnded: {
        ++stats_.matchesCompleted;
        const uint64_t seed = server_->matchConfig().seed;
        if (e.player.isValid()) {
          log("match {} won by slot {} after {} rounds; seed=0x{:x} scores {}",
              server_->matchIndex() + 1, e.player.value, w.rules.round, seed, scoreList(w));
        } else {
          // A tied lead when a mode's clock ran out (deathmatch).
          log("match {} drawn after {} rounds; seed=0x{:x} scores {}", server_->matchIndex() + 1,
              w.rules.round, seed, scoreList(w));
        }
        if (config_.exitAfterMatches != 0 && stats_.matchesCompleted >= config_.exitAfterMatches) {
          done_ = true;
          log("exit after {} matches", stats_.matchesCompleted);
          break;
        }
        const uint64_t next = nextMatchSeed(seed);
        botSeed_ = nextMatchSeed(botSeed_);
        const Tick start =
            server_->nextTick().plus(static_cast<int32_t>(config_.restartDelayTicks));
        server_->restartMatch(next, start, botSeed_);
        log("next match seed=0x{:x} at tick {}", next, start.value);
        break;
      }
      default:
        break;
    }
  }
}

void ServerHost::logServerEvents() {
  for (const net::ServerEvent& e : server_->events()) {
    const unsigned peer = e.peer.value;
    switch (e.kind) {
      case net::ServerEventKind::PeerJoined:
        if (e.slots == 0) {
          log("join peer={} spectator", peer);
        } else {
          log("join peer={} slots={}", peer, slotList(e.slots));
        }
        break;
      case net::ServerEventKind::PeerReconnected:
        log("reconnect peer={} slots={}", peer, slotList(e.slots));
        break;
      case net::ServerEventKind::PeerRejected:
        log("reject peer={} reason={}", peer, rejectName(e.reason));
        break;
      case net::ServerEventKind::PeerLeft:
        log("leave peer={} slots={} reason={}", peer, slotList(e.slots), disconnectName(e.reason));
        break;
      case net::ServerEventKind::PeerKicked:
        log("kick peer={} slots={} reason={}", peer, slotList(e.slots), disconnectName(e.reason));
        break;
      case net::ServerEventKind::SlotsReleased:
        log("release slots={}", slotList(e.slots));
        break;
      case net::ServerEventKind::ResyncServed:
        log("resync peer={}", peer);
        break;
      case net::ServerEventKind::MatchRestarted:
        log("match {} starts seed=0x{:x} players={}", server_->matchIndex() + 1,
            server_->matchConfig().seed, slotList(e.slots));
        break;
    }
  }
}

void ServerHost::logStats(uint64_t nowNs) {
  const net::ServerStats& s = server_->stats();
  const net::ServerStats& l = lastStats_;
  const uint64_t ns = nowNs > lastStatsNs_ ? nowNs - lastStatsNs_ : 0;
  const std::vector<net::ServerPeerInfo> peers = server_->peers();
  const auto players = static_cast<unsigned>(
      std::popcount(uint32_t{server_->present()} & ~uint32_t{server_->bots()}));
  log("stats peers={} players={} bots={} ticks={} subst/s={:.2f} in={:.0f}B/s out={:.0f}B/s "
      "joins=+{} reconnects=+{} rejects=+{} kicks=+{} resyncs=+{} protocol_errors=+{} "
      "rate_limited=+{} rejected_inputs=+{} late_inputs/s={:.1f} dropped_ticks=+{}",
      peers.size(), players, std::popcount(static_cast<uint32_t>(server_->bots())),
      s.ticks - l.ticks, perSecond(s.substitutedInputs - l.substitutedInputs, ns),
      perSecond(s.bytesReceived - l.bytesReceived, ns), perSecond(s.bytesSent - l.bytesSent, ns),
      s.joins - l.joins, s.reconnects - l.reconnects, s.rejects - l.rejects, s.kicks - l.kicks,
      s.resyncsServed - l.resyncsServed, s.protocolErrors - l.protocolErrors,
      s.rateLimited - l.rateLimited, s.rejectedInputs - l.rejectedInputs,
      perSecond(s.inputsLate - l.inputsLate, ns),
      stats_.droppedTicks - lastHostStats_.droppedTicks);
  if (config_.verbose) {
    for (const net::ServerPeerInfo& p : peers) {
      log("  peer={} slots={} margin={:.1f} subst={} in={} out={} errors={}", p.peer.value,
          slotList(p.slots), static_cast<double>(p.marginEmaQ8) / 256.0, p.substitutions, p.bytesIn,
          p.bytesOut, p.protocolErrors);
    }
  }
  lastStats_ = s;
  lastHostStats_ = stats_;
  lastStatsNs_ = nowNs;
}

void ServerHost::beginShutdown() {
  if (server_->shuttingDown()) return;
  log("shutting down: disconnecting {} peers", server_->peers().size());
  server_->shutdown();
}

void ServerHost::finishShutdown() {
  server_->closeAll();
  logServerEvents();
  server_->clearEvents();
}

void ServerHost::emit(std::string_view line) const {
  const std::string full =
      std::format("[{:.3f}s tick {}] {}", static_cast<double>(nowNs_ - startNs_) / 1e9,
                  server_ ? server_->nextTick().value : 0u, line);
  if (config_.log) {
    config_.log(full);
    return;
  }
  std::fwrite(full.data(), 1, full.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

}  // namespace kfm::server
