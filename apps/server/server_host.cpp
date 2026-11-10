#include "server_host.hpp"

#include "kfm/ai/agent.hpp"
#include "kfm/ai/agent_host.hpp"
#include "kfm/ai/bot_factory.hpp"
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

}  // namespace

// The host's bots: the factory (and its module cache), the agents in their
// slots, their specs, the stats of the last stats line and, with
// KFM_WASM_BOTS, the module agents for their stats.
struct ServerHost::Bots {
  explicit Bots(uint32_t fuelPerTick) : factory(fuelPerTick) {}
  ai::BotFactory factory;
#ifdef KFM_SERVER_WASM_BOTS
  std::array<ai::WasmAgent*, sim::kMaxPlayers> wasm{};  // owned by `agents`
  std::array<ai::WasmAgentStats, sim::kMaxPlayers> lastWasm{};
#endif
  ai::AgentHost agents;
  std::array<std::optional<ai::BotSpec>, sim::kMaxPlayers> specs{};
  std::array<ai::AgentSlotStats, sim::kMaxPlayers> lastStats{};
  uint32_t matchIndex{0};  // the NetServer match the agents were initialised for
  uint8_t delay{0};

  // The agent for `spec` (null for one the factory cannot make; create()
  // prepared every configured spec, so that does not happen).
  [[nodiscard]] std::unique_ptr<ai::IAgent> make(const ai::BotSpec& spec, PlayerId slot) {
    std::expected<ai::BotFactory::Made, ai::BotError> made = factory.make(spec);
    if (!made) return nullptr;
#ifdef KFM_SERVER_WASM_BOTS
    wasm[slot.value] = made->wasm;
#else
    static_cast<void>(slot);
#endif
    return std::move(made->agent);
  }
};

std::vector<ai::BotSpec> referenceBots(uint8_t count) {
  return std::vector<ai::BotSpec>(count, ai::BotSpec{});
}

std::expected<std::unique_ptr<ServerHost>, std::string> ServerHost::create(
    net::ITransport& transport, const sim::ContentDB& content, HostConfig config, uint64_t nowNs) {
  auto bots = std::make_unique<Bots>(config.botFuelPerTick);
  // Every module loads before the server starts: one that does not refuses it.
  for (const ai::BotSpec& spec : config.bots) {
    if (std::expected<void, ai::BotError> ok = bots->factory.prepare(spec); !ok) {
      return std::unexpected(std::format("bot module: {}", ok.error().message));
    }
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
  sc.queueForBotSlots = config_.room;  // v0.44: bots yield in the room flow
  sc.botInputs = [this](const sim::WorldState& world, const sim::MatchContent& match,
                        sim::InputFrame& frame) { fillBots(world, match, frame); };
  server_ = std::make_unique<net::NetServer>(transport, content, std::move(sc));
  Bots& b = *bots_;
  b.delay = config_.botDelayTicks.value_or(0);
  b.agents.setRespawnMemory(config_.botMemory);
  uint8_t placed = 0;
  std::string specs;
  uint8_t s = 0;
  if (config_.room) {
    // v0.42: the room starts Waiting; its Start places the bots.
    roomDefaults_ =
        roomSettingsOf(config_.server.match,
                       static_cast<uint8_t>(std::min<size_t>(
                           config_.bots.size(),
                           config_.server.maxPlayers > 0 ? config_.server.maxPlayers - 1 : 0)));
    if (!validRoomSettings(content, roomDefaults_, config_.server.maxPlayers)) {
      roomDefaults_ = roomSettingsOf(sim::MatchConfig{}, 0);
    }
    room_ = std::make_unique<Room>(content, config_.server.maxPlayers, roomDefaults_);
  }
  const std::vector<ai::BotSpec> none;
  for (const ai::BotSpec& spec : config_.room ? none : config_.bots) {
    while (s < sim::kMaxPlayers && !server_->setBot(PlayerId{s}, true))
      ++s;
    if (s == sim::kMaxPlayers) break;
    const PlayerId slot{s++};
    b.specs[slot.value] = spec;
    b.agents.attach(slot, b.make(spec, slot), ai::agentSeed(config_.server.botSeed, slot), b.delay);
    ++placed;
    if (!specs.empty()) specs += ',';
    specs += specName(spec);
  }
  lastStatsNs_ = nowNs;
  nextStatsNs_ = config_.statsEveryNs > 0 ? nowNs + config_.statsEveryNs : 0;
  log("start seed=0x{:x} arena={} mode={} loadout={} power_ups=0x{:04x} bots={} slots={} "
      "max_players={} tokens={} bot_specs={} bot_delay={} bot_seed=0x{:x}",
      config_.server.match.seed, content.arenaAt(config_.server.match.arena).name.view(),
      content.modeAt(config_.server.match.mode).name.view(),
      content.loadoutAt(config_.server.match.loadout).name.view(), config_.server.match.powerUps,
      placed, slotList(server_->bots()), config_.server.maxPlayers,
      config_.server.acceptToken ? "checked" : "any", specs.empty() ? std::string("-") : specs,
      config_.botDelayTicks ? std::to_string(*config_.botDelayTicks) : std::string("auto"),
      config_.server.botSeed);
  logDeadBots();
  if (room_) {
    log("room waiting {}", roomText());
    broadcastRoom();
  }
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
      if (b.agents.has(slot)) b.agents.reinit(slot, ai::agentSeed(server_->botSeed(), slot));
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
    // v0.35: ticks sat out eliminated (the lifecycle), when there were any.
    if (now.paused != was.paused) out += std::format(" paused=+{}", now.paused - was.paused);
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
  if (room_) updateRoom();
  if (room_ && !room_->playing()) {
    // No match runs: nothing ticks, and the clock resumes from now.
    clockBaseNs_ = nowNs_;
    clockTicks_ = 0;
  } else if (!server_->shuttingDown()) {
    uint32_t ran = 0;
    while (nowNs_ >= tickDueAt(clockTicks_) && (!room_ || room_->playing())) {
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
  bool roundEnded = false;
  // v0.35: the calls each bot hears, from the step that just ran.
  bots_->agents.observe(server_->tickEvents(), w,
                        sim::matchContent(server_->content(), server_->matchConfig()));
  // " (team t)" in a team mode (v0.32), else nothing.
  const auto teamText = [&](PlayerId p) -> std::string {
    const sim::MatchConfig& c = server_->matchConfig();
    if (server_->content().modeAt(c.mode).teams == 0 || p.value >= sim::kMaxPlayers) return {};
    return std::format(" (team {})", c.teams[p.value]);
  };
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
        roundEnded = true;
        if (e.player.isValid()) {
          log("round {} won by slot {}{}; scores {}", e.a, e.player.value, teamText(e.player),
              scoreList(w));
        } else {
          log("round {} drawn; scores {}", e.a, scoreList(w));
        }
        break;
      case sim::EventKind::MatchEnded: {
        ++stats_.matchesCompleted;
        const uint64_t seed = server_->matchConfig().seed;
        if (e.player.isValid()) {
          log("match {} won by slot {}{} after {} rounds; seed=0x{:x} scores {}",
              server_->matchIndex() + 1, e.player.value, teamText(e.player), w.rules.round, seed,
              scoreList(w));
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
        if (room_) {
          // v0.42: the room shows the match over; the host plays again.
          if (room_->matchEnded(e.player)) {
            log("room over {}", roomText());
            broadcastRoom();
          }
          break;
        }
        scheduleNextMatch(config_.restartDelayTicks);
        break;
      }
      default:
        break;
    }
  }
  if (room_ && room_->playing() && serveQueue(true, roundEnded)) broadcastRoom();
}

void ServerHost::scheduleNextMatch(uint32_t delayTicks) {
  const uint64_t next = nextMatchSeed(server_->matchConfig().seed);
  botSeed_ = nextMatchSeed(botSeed_);
  const Tick start = server_->nextTick().plus(static_cast<int32_t>(delayTicks));
  // v0.32: the next match rebalances a team mode's teams over the
  // slots present now (later joiners still land on the smallest team).
  sim::MatchConfig nextConfig = server_->matchConfig();
  nextConfig.seed = next;
  nextConfig.teams =
      sim::teamsFor(server_->content().modeAt(nextConfig.mode), server_->world().rules.present);
  server_->restartMatch(nextConfig, start, botSeed_);
  log("next match seed=0x{:x} at tick {}", next, start.value);
}

void ServerHost::startNewMatch(std::string_view reason) {
  if (server_->shuttingDown() || done_) return;
  log("new match: {}", reason);
  scheduleNextMatch(0);
}

void ServerHost::openRoom(std::string_view reason) {
  if (!room_) {
    startNewMatch(reason);
    return;
  }
  if (server_->shuttingDown() || done_) return;
  removeRoomBots();
  room_->reset(roomDefaults_);
  log("room reset: {}; waiting {}", reason, roomText());
  broadcastRoom();
}

// --- the room (v0.42) -------------------------------------------------------------

std::string ServerHost::roomText() const {
  const net::RoomSettings& r = room_->settings();
  const sim::ContentDB& db = server_->content();
  const PlayerId host = room_->host();
  return std::format("arena={} mode={} loadout={} power_ups=0x{:04x} bots={} host={} humans={}",
                     db.arenaAt(r.arena).name.view(), db.modeAt(r.mode).name.view(),
                     db.loadoutAt(r.loadout).name.view(), r.powerUps, unsigned{r.bots},
                     host.isValid() ? std::to_string(host.value) : std::string("-"),
                     slotList(server_->humanSlots()));
}

void ServerHost::broadcastRoom() {
  net::RoomStateMsg m;
  m.phase = room_->phase();
  m.host = room_->host();
  m.settings = room_->settings();
  m.humans = server_->humanSlots();
  m.bots = server_->bots();
  m.winner = room_->winner();
  for (uint8_t s = 0; s < sim::kMaxPlayers; ++s) {
    if (sim::maskHas(m.humans, PlayerId{s})) m.names[s] = server_->nickname(PlayerId{s});
  }
  if (m.host.isValid() && !sim::maskHas(m.humans, m.host)) m.host = PlayerId::invalid();
  server_->setRoomState(m);
}

void ServerHost::placeRoomBots(uint8_t count) {
  Bots& b = *bots_;
  for (uint8_t k = 0; k < count; ++k) {
    const sim::PlayerMask free = server_->freeSlots();
    if (free == 0) break;
    const PlayerId slot{static_cast<uint8_t>(std::countr_zero(static_cast<uint32_t>(free)))};
    if (!server_->setBot(slot, true)) break;
    const ai::BotSpec spec = k < config_.bots.size() ? config_.bots[k] : ai::BotSpec{};
    b.specs[slot.value] = spec;
    b.lastStats[slot.value] = {};
    b.agents.attach(slot, b.make(spec, slot), ai::agentSeed(botSeed_, slot), b.delay);
  }
}

void ServerHost::removeBot(PlayerId slot) {
  Bots& b = *bots_;
  static_cast<void>(server_->setBot(slot, false));
  static_cast<void>(b.agents.detach(slot));
  b.specs[slot.value].reset();
#ifdef KFM_SERVER_WASM_BOTS
  b.wasm[slot.value] = nullptr;
#endif
}

bool ServerHost::serveQueue(bool tickEnded, bool roundEnded) {
  const net::PeerId peer = server_->oldestQueued();
  if (!peer.isValid()) return false;
  const int want = server_->queuedSlots(peer);
  const auto freeCount = [&] { return std::popcount(static_cast<uint32_t>(server_->freeSlots())); };
  // A yield made free a slot: the sim must step one tick without it first.
  const bool settled = !yieldedAt_ || !room_->playing() || server_->nextTick() > *yieldedAt_;
  if (freeCount() >= want && settled) {
    const net::PeerId p = peer;
    if (!server_->grantQueued(p)) return false;
    yieldedAt_.reset();
    log("room slot granted to peer={}", p.value);
    return true;
  }
  if (freeCount() >= want) return false;  // granted after the next tick
  bool changed = false;
  if (!room_->playing()) {
    // Waiting or Over: no tick runs, the highest bots go now.
    while (freeCount() < want && server_->bots() != 0) {
      const auto s = static_cast<uint8_t>(31 - std::countl_zero(uint32_t{server_->bots()}));
      log("bot {} yields its slot to peer={}", s, peer.value);
      removeBot(PlayerId{s});
      changed = true;
    }
    if (freeCount() >= want && server_->grantQueued(peer)) {
      log("room slot granted to peer={}", peer.value);
    }
    return changed;
  }
  if (!tickEnded) return false;
  // Playing: one bot whose monkey is off the field, or any at a round end.
  const sim::WorldState& w = server_->world();
  for (int s = sim::kMaxPlayers - 1; s >= 0; --s) {
    const PlayerId slot{static_cast<uint8_t>(s)};
    if (!sim::maskHas(server_->bots(), slot) || !bots_->specs[slot.value]) continue;
    if (!roundEnded && w.players.isAliveIndex(slot.value)) continue;
    log("bot {} yields its slot to peer={}{}", s, peer.value,
        roundEnded ? " at the round end" : " (eliminated)");
    removeBot(slot);
    yieldedAt_ = server_->nextTick();
    return true;
  }
  return false;
}

void ServerHost::removeRoomBots() {
  for (uint8_t s = 0; s < sim::kMaxPlayers; ++s) {
    if (bots_->specs[s]) removeBot(PlayerId{s});
  }
}

void ServerHost::startRoomMatch() {
  const net::RoomSettings& r = room_->settings();
  placeRoomBots(r.bots);
  sim::MatchConfig c = server_->matchConfig();
  c.seed = nextMatchSeed(c.seed);
  c.arena = r.arena;
  c.mode = r.mode;
  c.loadout = r.loadout;
  c.powerUps = r.powerUps;
  c.teams = sim::teamsFor(server_->content().modeAt(c.mode), server_->present());
  botSeed_ = nextMatchSeed(botSeed_);
  server_->restartMatch(c, server_->nextTick(), botSeed_);
  log("room start seed=0x{:x} {}", c.seed, roomText());
}

void ServerHost::updateRoom() {
  // Outside a match nobody waits to reconnect: a leaver's slot is free.
  if (!room_->playing()) server_->releaseReservations();
  struct Request {
    net::ServerEventKind kind;
    sim::PlayerMask slots;
    net::RoomSettings settings;
  };
  std::vector<Request> requests;
  for (const net::ServerEvent& e : server_->events()) {
    if (e.kind == net::ServerEventKind::RoomSettingsRequested ||
        e.kind == net::ServerEventKind::StartRequested) {
      const sim::PlayerMask slots =
          server_->peerInfo(e.peer)
              .transform([](const net::ServerPeerInfo& p) { return p.slots; })
              .value_or(sim::PlayerMask{0});
      requests.push_back(Request{e.kind, slots, e.settings});
    }
  }
  const net::RoomPhase before = room_->phase();
  bool dirty = room_->updateMembers(server_->humanSlots());
  if (before != net::RoomPhase::Waiting && room_->phase() == net::RoomPhase::Waiting) {
    removeRoomBots();  // the last human left
    log("room empty: waiting {}", roomText());
  }
  for (const Request& q : requests) {
    if (q.kind == net::ServerEventKind::RoomSettingsRequested) {
      const bool ok = room_->requestSettings(q.slots, q.settings);
      log("room settings {} {}", ok ? "applied" : "refused", roomText());
      dirty = true;  // answered with the state either way
      continue;
    }
    switch (room_->requestStart(q.slots)) {
      case Room::StartResult::Ignored:
        break;
      case Room::StartResult::Start:
        startRoomMatch();
        dirty = true;
        break;
      case Room::StartResult::BackToWaiting:
        removeRoomBots();
        log("room waiting {}", roomText());
        dirty = true;
        break;
    }
  }
  // v0.44: a waiting joiner takes a free slot, or (outside Playing) a bot's.
  if (serveQueue(false, false)) dirty = true;
  if (dirty) {
    static_cast<void>(room_->updateMembers(server_->humanSlots()));
    broadcastRoom();
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
      case net::ServerEventKind::PeerQueued:
        log("queue peer={} waits for {} slot{}", peer, unsigned{e.reason},
            e.reason == 1 ? "" : "s");
        break;
      case net::ServerEventKind::RoomSettingsRequested:
      case net::ServerEventKind::StartRequested:
        if (config_.verbose) {
          log("{} peer={} slots={}",
              e.kind == net::ServerEventKind::StartRequested ? "start request" : "settings request",
              peer, slotList(e.slots));
        }
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
      "rate_limited=+{} rejected_inputs=+{} late_inputs/s={:.1f} dropped_ticks=+{}{}{}",
      peers.size(), players, std::popcount(static_cast<uint32_t>(server_->bots())),
      s.ticks - l.ticks, perSecond(s.substitutedInputs - l.substitutedInputs, ns),
      perSecond(s.bytesReceived - l.bytesReceived, ns), perSecond(s.bytesSent - l.bytesSent, ns),
      s.joins - l.joins, s.reconnects - l.reconnects, s.rejects - l.rejects, s.kicks - l.kicks,
      s.resyncsServed - l.resyncsServed, s.protocolErrors - l.protocolErrors,
      s.rateLimited - l.rateLimited, s.rejectedInputs - l.rejectedInputs,
      perSecond(s.inputsLate - l.inputsLate, ns), stats_.droppedTicks - lastHostStats_.droppedTicks,
      botStats(), config_.statsSuffix ? config_.statsSuffix(nowNs) : std::string());
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
