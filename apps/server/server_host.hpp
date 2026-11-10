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
// seed derived from the old one; the roster carries over and the bots are
// re-seeded. With exitAfterMatches set, the host reports done() instead of
// restarting once that many matches have ended.
//
// Room flow (v0.42, HostConfig::room, §6.6, room.hpp): instead of the M3
// policy above, the host keeps a Room beside the NetServer. The room starts
// Waiting with the server's settings (match arena, mode, loadout, power-ups
// and bots.size() as the bot count) and no bots placed; while it is not
// Playing the host pumps the NetServer but never ticks it (its clock is
// rebased every frame, so resuming runs no backlog) and frees the slots of
// leavers at once (NetServer::releaseReservations). The host's RoomSettings
// are validated by the Room; the host's Start in Waiting places the bots
// (the first `bots` specs of HostConfig::bots, padded with ref:hard, in the
// lowest free slots, at most as many as there are free slots) and restarts
// the match at the next tick with the room's settings (teams by
// sim::teamsFor, next seeds as after a match end) → Playing. MatchEnded →
// Over (no auto-restart, exitAfterMatches still counts); the host's Start in
// Over removes the bots → Waiting. The room returns to Waiting, removing the
// bots, when its last human slot is gone. Every change goes out as one
// RoomState (NetServer::setRoomState) and one "room ..." log line.
//
// Bots yield (v0.44, §6.6): in the room flow the NetServer queues a
// joiner that only bots' slots could fit (ServerConfig::queueForBotSlots)
// as a spectator. Outside Playing the highest bot gives its slot up at once
// and the joiner gets it. While Playing a bot yields when its monkey is off
// the field: eliminated (waiting for its respawn, or for the next round) or
// at a round end (RoundEnded: every bot qualifies); the highest such bot
// leaves at that tick and its slot is handed to the waiting peer one tick
// later, so the sim sees a leave and a join, never an owner change: a
// respawning mode forfeits the bot's score and spawns the joiner after
// respawnTicks like any late joiner, a rounds mode spawns it at the next
// round start. A slot that comes free any other way (a human leaves, a
// reservation expires) goes to the oldest waiting peer too. A yielded bot
// does not come back in that match: the room's bot count is "bots when
// there is room", applied at the next Start.
//
// Bots (§8): every bot is an ai::BotSpec (the reference bot at a tier, or a
// .wasm module) run by the host's ai::AgentHost, which NetServer calls once
// per tick through ServerConfig::botInputs; the inputs go straight into the
// authoritative frame. The scripted generator is never used here (it stays
// NetServer's default for kfm_netsim and the netcode tests).
//   - Seeds: match k's bot seed is B_0 = server.botSeed, B_k+1 =
//     nextMatchSeed(B_k) (passed to restartMatch); the agent in slot s gets
//     ai::agentSeed(B_k, s) = B_k ^ (0x9E3779B97F4A7C15 · (s + 1)), every
//     host's formula, so a server match and an arena match with the same seeds
//     agree. Every agent is re-initialised (a wasm module: a fresh instance)
//     on the first tick of each new match, before it acts on the new world.
//   - Delay (human-likeness): botDelayTicks fixed, or auto (nullopt): once
//     per second every bot's delay becomes NetServer::humanLeadTicks(), the
//     median estimated lead of the peers holding slots, 0 when none does
//     (bot versus bot: no delay). See ai::AgentHost::setDelay for how a
//     change is absorbed.
//   - Modules: create() prepares every spec in the host's ai::BotFactory
//     (kfm/ai/bot_factory.hpp: each distinct .wasm path read and loaded
//     once, one instance per slot, under the factory's WasmRuntime); a module
//     that fails to load fails create(). Needs KFM_WASM_BOTS (otherwise a
//     module spec is that error).
//   - The stats line ends with the bot delay and one "bot S SPEC acts=+N
//     subst=+M" group per bot, modules adding traps, over-budget ticks and
//     the mean fuel per act over the period.
//
// Shutdown: beginShutdown() sends Disconnect{ServerShuttingDown} to every
// peer; the caller keeps calling frame() (pump only, no ticks) for a short
// linger so the messages leave, then finishShutdown() closes every peer.
#pragma once

#include "kfm/ai/agent_host.hpp"
#include "kfm/ai/bot_spec.hpp"
#include "kfm/core/types.hpp"
#include "kfm/net/net_server.hpp"
#include "kfm/net/transport.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/content.hpp"
#include "room.hpp"

#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kfm::server {

struct HostConfig {
  // Match seed, build id, token check, player cap and the netcode knobs.
  net::ServerConfig server{};
  // Server-side bots, one per spec, in the lowest free slots in order
  // (capped by server.maxPlayers). With `room`: the specs the room's bot
  // count draws from, and their number the room's default count.
  std::vector<ai::BotSpec> bots;
  // v0.42: the room flow (see the header comment) instead of the M3 policy.
  bool room{false};
  // The bots' input delay in ticks; nullopt = auto (the humans' lead).
  std::optional<uint8_t> botDelayTicks;
  // A wasm bot's instruction budget per tick (0 = the runtime default).
  uint32_t botFuelPerTick{0};
  // v0.35: memory_on_respawn for every bot (ai::AgentHost::setRespawnMemory).
  ai::RespawnMemory botMemory{ai::RespawnMemory::Keep};
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
  // Appended to every stats line when set (kfm_server --lobby adds
  // " | lobby registered room=ABCDE ...").
  std::function<std::string(uint64_t nowNs)> statsSuffix;
};

struct HostStats {
  uint64_t ticks{0};
  uint64_t droppedTicks{0};  // backlog beyond maxCatchUpTicks
  uint32_t matchesCompleted{0};
  uint32_t roundsCompleted{0};
};

// The seed of the match after one played with `seed` (splitmix64).
[[nodiscard]] uint64_t nextMatchSeed(uint64_t seed);
// `count` reference bots at the hard tier (kfm_server --bots N).
[[nodiscard]] std::vector<ai::BotSpec> referenceBots(uint8_t count);

class ServerHost {
  struct Bots;
  struct Key {
    explicit Key() = default;
  };

 public:
  // Loads the bots' modules and builds the host; the error names the module
  // that failed to load.
  [[nodiscard]] static std::expected<std::unique_ptr<ServerHost>, std::string> create(
      net::ITransport& transport, const sim::ContentDB& content, HostConfig config, uint64_t nowNs);
  // Use create().
  ServerHost(Key key, net::ITransport& transport, const sim::ContentDB& content, HostConfig config,
             uint64_t nowNs, std::unique_ptr<Bots> bots);
  ~ServerHost();
  ServerHost(const ServerHost&) = delete;
  ServerHost& operator=(const ServerHost&) = delete;
  ServerHost(ServerHost&&) = delete;
  ServerHost& operator=(ServerHost&&) = delete;

  // One host frame at nowNs (monotonic, never decreasing).
  void frame(uint64_t nowNs);
  // Time until the next tick is due (0 when one is due now).
  [[nodiscard]] uint64_t nsUntilNextTick(uint64_t nowNs) const;

  // Announces a fresh match starting at the next tick through the same path
  // as a match end (next seeds, bots re-seeded, teams rebalanced), logged
  // with `reason`. kfm_server --lobby calls it when a new room arrives with
  // no human connected (docs/lobby-protocol.md §6 point 5). Ignored while
  // shutting down or done().
  void startNewMatch(std::string_view reason);
  // A new group arrives (kfm_server --lobby, a room assigned with no peer
  // connected): the room flow resets the room to Waiting with the server's
  // default settings (bots removed); the M3 flow calls startNewMatch().
  void openRoom(std::string_view reason);
  // The room, with HostConfig::room (null otherwise).
  [[nodiscard]] const Room* room() const { return room_.get(); }

  void beginShutdown();
  void finishShutdown();
  [[nodiscard]] bool shuttingDown() const { return server_->shuttingDown(); }
  // exitAfterMatches reached.
  [[nodiscard]] bool done() const { return done_; }

  [[nodiscard]] net::NetServer& server() { return *server_; }
  [[nodiscard]] const net::NetServer& server() const { return *server_; }
  [[nodiscard]] const HostStats& stats() const { return stats_; }
  // The input delay the bots run with now.
  [[nodiscard]] uint8_t botDelayTicks() const;
  // The bot spec in a slot (nullopt: not a host bot).
  [[nodiscard]] std::optional<ai::BotSpec> botSpec(PlayerId slot) const;
  // The AgentHost counters of a host bot's slot (zero for any other slot).
  [[nodiscard]] ai::AgentSlotStats agentStats(PlayerId slot) const;
  // The line format of the periodic stats, for a final summary.
  void logStats(uint64_t nowNs);
  // One line in the host's log format (time and tick prefix), for adapters.
  void logLine(std::string_view line) const { emit(line); }

 private:
  [[nodiscard]] uint64_t tickDueAt(uint64_t k) const;
  // restartMatch() with the next match and bot seeds, delayTicks from now.
  void scheduleNextMatch(uint32_t delayTicks);
  void onTick();
  void logServerEvents();
  // The room flow's per-frame work after pump(): reservations, members,
  // requests, the RoomState broadcast.
  void updateRoom();
  void placeRoomBots(uint8_t count);
  void removeRoomBots();
  // v0.44: drops one host bot (its agent detached, its slot freed).
  void removeBot(PlayerId slot);
  // v0.44: the waiting peer's slots, granted or made free by a yield; true
  // when the roster changed. `tickEnded`: called after a tick (onTick).
  bool serveQueue(bool tickEnded, bool roundEnded);
  void startRoomMatch();
  void broadcastRoom();
  [[nodiscard]] std::string roomText() const;
  void fillBots(const sim::WorldState& world, const sim::MatchContent& content,
                sim::InputFrame& frame);
  void updateBotDelay();
  void logDeadBots();
  [[nodiscard]] std::string botStats();
  // One line, prefixed with the host time and the next tick.
  template <class... Args>
  void log(std::format_string<Args...> fmt, Args&&... args) {
    emit(std::format(fmt, std::forward<Args>(args)...));
  }
  void emit(std::string_view line) const;

  HostConfig config_;
  std::unique_ptr<Bots> bots_;
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
  std::unique_ptr<Room> room_;
  net::RoomSettings roomDefaults_{};
  // v0.44: the tick a bot last yielded at; the waiting peer gets the slot
  // once a tick has run without it.
  std::optional<Tick> yieldedAt_;
  bool done_{false};
  HostStats stats_{};
};

}  // namespace kfm::server
