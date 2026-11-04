#include "server_host.hpp"

#include "kfm/ai/agent.hpp"
#include "kfm/ai/agent_host.hpp"
#include "kfm/ai/reference_bot.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/world_state.hpp"

#ifdef KFM_SERVER_WASM_BOTS
#include "kfm/ai/wasm_runtime.hpp"
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <fstream>
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

// A spec as the logs show it: a module by its file name.
[[nodiscard]] std::string specName(const ai::BotSpec& spec) {
  std::string text = ai::toString(spec);
  if (spec.kind == ai::BotKind::Module) {
    const size_t slash = text.find_last_of("/\\");
    if (slash != std::string::npos) text.erase(0, slash + 1);
  }
  return text;
}

#ifdef KFM_SERVER_WASM_BOTS
// The whole file, at most `cap` + 1 bytes (one more than the cap makes the
// module loader refuse it as too large).
[[nodiscard]] std::expected<std::vector<std::byte>, std::string> readFile(const std::string& path,
                                                                          size_t cap) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::unexpected(std::format("cannot open {}", path));
  std::vector<std::byte> out;
  std::array<char, 16384> chunk{};
  while (out.size() <= cap && f) {
    f.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const auto n = static_cast<size_t>(f.gcount());
    for (size_t i = 0; i < n; ++i)
      out.push_back(static_cast<std::byte>(chunk[i]));
  }
  if (f.bad()) return std::unexpected(std::format("cannot read {}", path));
  if (out.size() > cap + 1) out.resize(cap + 1);
  return out;
}
#endif

}  // namespace

// The host's bots: the agents in their slots, their specs, the stats of the
// last stats line and, with KFM_WASM_BOTS, the runtime and the module cache.
struct ServerHost::Bots {
#ifdef KFM_SERVER_WASM_BOTS
  std::optional<ai::WasmRuntime> runtime;
  std::vector<std::pair<std::string, ai::WasmModule>> modules;  // one per distinct path
  std::array<ai::WasmAgent*, sim::kMaxPlayers> wasm{};          // owned by `agents`
  std::array<ai::WasmAgentStats, sim::kMaxPlayers> lastWasm{};

  [[nodiscard]] const ai::WasmModule* module(std::string_view path) const {
    for (const auto& [p, m] : modules)
      if (p == path) return &m;
    return nullptr;
  }
#endif
  ai::AgentHost agents;
  std::array<std::optional<ai::BotSpec>, sim::kMaxPlayers> specs{};
  std::array<ai::AgentSlotStats, sim::kMaxPlayers> lastStats{};
  uint32_t matchIndex{0};  // the NetServer match the agents were initialised for
  uint8_t delay{0};

  // The agent for `spec` (null for a module that is not loaded).
  [[nodiscard]] std::unique_ptr<ai::IAgent> make(const ai::BotSpec& spec, PlayerId slot) {
    if (spec.kind == ai::BotKind::Reference) {
      return std::make_unique<ai::ReferenceBot>(spec.referenceConfig());
    }
#ifdef KFM_SERVER_WASM_BOTS
    if (const ai::WasmModule* m = module(spec.path)) {
      auto agent = std::make_unique<ai::WasmAgent>(*m);
      wasm[slot.value] = agent.get();
      return agent;
    }
#else
    static_cast<void>(slot);
#endif
    return nullptr;
  }
};

std::vector<ai::BotSpec> referenceBots(uint8_t count) {
  return std::vector<ai::BotSpec>(count, ai::BotSpec{});
}

std::expected<std::unique_ptr<ServerHost>, std::string> ServerHost::create(
    net::ITransport& transport, const sim::ContentDB& content, HostConfig config, uint64_t nowNs) {
  auto bots = std::make_unique<Bots>();
  for (const ai::BotSpec& spec : config.bots) {
    if (spec.kind != ai::BotKind::Module) continue;
#ifdef KFM_SERVER_WASM_BOTS
    if (!bots->runtime) {
      const ai::WasmRuntime& runtime = bots->runtime.emplace();
      if (!runtime.ok()) return std::unexpected(std::string("the wasm runtime failed"));
    }
    if (bots->module(spec.path) != nullptr) continue;
    ai::WasmAgentConfig wc;
    if (config.botFuelPerTick != 0) wc.fuelPerTick = config.botFuelPerTick;
    const std::expected<std::vector<std::byte>, std::string> bytes =
        readFile(spec.path, wc.maxModuleBytes);
    if (!bytes) return std::unexpected(std::format("bot module {}: {}", spec.path, bytes.error()));
    std::expected<ai::WasmModule, ai::LoadError> m = ai::WasmModule::load(*bytes, wc);
    if (!m) {
      return std::unexpected(std::format("bot module {}: {} ({})", spec.path, m.error().message,
                                         ai::toString(m.error().kind)));
    }
    bots->modules.emplace_back(spec.path, std::move(*m));
#else
    return std::unexpected(std::format(
        "bot module {}: this build has no wasm bot runtime (KFM_WASM_BOTS=OFF)", spec.path));
#endif
  }
  return std::make_unique<ServerHost>(Key{}, transport, content, std::move(config), nowNs,
                                      std::move(bots));
}

uint64_t nextMatchSeed(uint64_t seed) {
  uint64_t z = seed + 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

ServerHost::ServerHost(Key /*key*/, net::ITransport& transport, const sim::ContentDB& content,
                       HostConfig config, uint64_t nowNs, std::unique_ptr<Bots> bots)
    : config_(std::move(config)),
      bots_(std::move(bots)),
      startNs_(nowNs),
      nowNs_(nowNs),
      clockBaseNs_(nowNs),
      botSeed_(config_.server.botSeed) {
  net::ServerConfig sc = config_.server;
  sc.botInputs = [this](const sim::WorldState& world, const sim::MatchContent& match,
                        sim::InputFrame& frame) { fillBots(world, match, frame); };
  server_ = std::make_unique<net::NetServer>(transport, content, std::move(sc));
  Bots& b = *bots_;
  b.delay = config_.botDelayTicks.value_or(0);
  uint8_t placed = 0;
  std::string specs;
  uint8_t s = 0;
  for (const ai::BotSpec& spec : config_.bots) {
    while (s < sim::kMaxPlayers && !server_->setBot(PlayerId{s}, true))
      ++s;
    if (s == sim::kMaxPlayers) break;
    const PlayerId slot{s++};
    b.specs[slot.value] = spec;
    b.agents.attach(slot, b.make(spec, slot), agentSeed(config_.server.botSeed, slot), b.delay);
    ++placed;
    if (!specs.empty()) specs += ',';
    specs += specName(spec);
  }
  lastStatsNs_ = nowNs;
  nextStatsNs_ = config_.statsEveryNs > 0 ? nowNs + config_.statsEveryNs : 0;
  log("start seed=0x{:x} arena={} mode={} power_ups=0x{:04x} bots={} slots={} max_players={} "
      "tokens={} bot_specs={} bot_delay={} bot_seed=0x{:x}",
      config_.server.match.seed, content.arenaAt(config_.server.match.arena).name.view(),
      content.modeAt(config_.server.match.mode).name.view(), config_.server.match.powerUps, placed,
      slotList(server_->bots()), config_.server.maxPlayers,
      config_.server.acceptToken ? "checked" : "any", specs.empty() ? std::string("-") : specs,
      config_.botDelayTicks ? std::to_string(*config_.botDelayTicks) : std::string("auto"),
      config_.server.botSeed);
  logDeadBots();
}

ServerHost::~ServerHost() = default;

uint8_t ServerHost::botDelayTicks() const {
  return bots_->delay;
}

std::optional<ai::BotSpec> ServerHost::botSpec(PlayerId slot) const {
  if (!slot.isValid() || slot.value >= sim::kMaxPlayers) return std::nullopt;
  return bots_->specs[slot.value];
}

void ServerHost::fillBots(const sim::WorldState& world, const sim::MatchContent& content,
                          sim::InputFrame& frame) {
  Bots& b = *bots_;
  if (server_->matchIndex() != b.matchIndex) {
    // The first tick of a new match: every agent starts over on its seed.
    b.matchIndex = server_->matchIndex();
    for (uint8_t s = 0; s < sim::kMaxPlayers; ++s) {
      const PlayerId slot{s};
      if (b.agents.has(slot)) b.agents.reinit(slot, agentSeed(server_->botSeed(), slot));
    }
    logDeadBots();
  }
  b.agents.fill(frame, world, content);
}

void ServerHost::logDeadBots() {
#ifdef KFM_SERVER_WASM_BOTS
  for (uint8_t s = 0; s < sim::kMaxPlayers; ++s) {
    const ai::WasmAgent* a = bots_->wasm[s];
    const std::optional<ai::BotSpec>& spec = bots_->specs[s];
    if (a == nullptr || a->alive() || !spec) continue;
    log("bot {} {} failed to start: {} {}; it stands still this match", s, specName(*spec),
        ai::toString(a->deathCause()), a->lastFaultMessage());
  }
#endif
}

void ServerHost::updateBotDelay() {
  Bots& b = *bots_;
  const uint8_t d = config_.botDelayTicks.value_or(server_->humanLeadTicks());
  if (d == b.delay) return;
  b.delay = d;
  for (uint8_t s = 0; s < sim::kMaxPlayers; ++s)
    b.agents.setDelay(PlayerId{s}, d);
  if (config_.verbose) log("bot delay {} ticks", unsigned{d});
}

std::string ServerHost::botStats() {
  Bots& b = *bots_;
  std::string out = std::format(" bot_delay={}", unsigned{b.delay});
  for (uint8_t s = 0; s < sim::kMaxPlayers; ++s) {
    const std::optional<ai::BotSpec>& spec = b.specs[s];
    if (!spec) continue;
    const ai::AgentSlotStats now = b.agents.stats(PlayerId{s});
    const ai::AgentSlotStats& was = b.lastStats[s];
    out += std::format(" | bot {} {} acts=+{} subst=+{}", s, specName(*spec), now.acts - was.acts,
                       now.substitutions - was.substitutions);
    b.lastStats[s] = now;
#ifdef KFM_SERVER_WASM_BOTS
    if (const ai::WasmAgent* a = b.wasm[s]) {
      const ai::WasmAgentStats& w = a->stats();
      const ai::WasmAgentStats& l = b.lastWasm[s];
      const uint64_t acts = w.acts - l.acts;
      const double fuelMean =
          acts == 0 ? 0.0
                    : static_cast<double>(w.fuelTotal - l.fuelTotal) / static_cast<double>(acts);
      out += std::format(" traps=+{} over_budget=+{} fuel_mean={:.0f}", w.traps - l.traps,
                         w.overBudget - l.overBudget, fuelMean);
      b.lastWasm[s] = w;
    }
#endif
  }
  return out;
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
      // Once a second the bots' delay follows the humans' lead (§8).
      if (stats_.ticks % sim::kTickHz == 0) updateBotDelay();
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
      "rate_limited=+{} rejected_inputs=+{} late_inputs/s={:.1f} dropped_ticks=+{}{}",
      peers.size(), players, std::popcount(static_cast<uint32_t>(server_->bots())),
      s.ticks - l.ticks, perSecond(s.substitutedInputs - l.substitutedInputs, ns),
      perSecond(s.bytesReceived - l.bytesReceived, ns), perSecond(s.bytesSent - l.bytesSent, ns),
      s.joins - l.joins, s.reconnects - l.reconnects, s.rejects - l.rejects, s.kicks - l.kicks,
      s.resyncsServed - l.resyncsServed, s.protocolErrors - l.protocolErrors,
      s.rateLimited - l.rateLimited, s.rejectedInputs - l.rejectedInputs,
      perSecond(s.inputsLate - l.inputsLate, ns), stats_.droppedTicks - lastHostStats_.droppedTicks,
      botStats());
  if (config_.verbose) {
    for (const net::ServerPeerInfo& p : peers) {
      log("  peer={} slots={} margin={:.1f} lead={:.1f} subst={} in={} out={} errors={}",
          p.peer.value, slotList(p.slots), static_cast<double>(p.marginEmaQ8) / 256.0,
          static_cast<double>(p.leadEmaQ8) / 256.0, p.substitutions, p.bytesIn, p.bytesOut,
          p.protocolErrors);
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
