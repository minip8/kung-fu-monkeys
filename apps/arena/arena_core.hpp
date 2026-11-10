// kfm_arena's core (architecture §8, §9): one bot match played headless at
// full speed, its result and the result line. A library so the match runner
// is unit-tested without the CLI (tests/unit/arena), like kfm::server_host.
//
// A match: the MatchSetup's bots fill slots 0..n-1 in order (rotated by
// MatchSetup::rotation: bot b plays slot (b + rotation) mod n); initialWorld()
// from the config (players = those slots); every tick AgentHost::fill() writes
// each bot's input into the frame, the frame is recorded, step() runs and
// the events are folded into per-slot counters; the match stops at
// MatchEnded or at the tick cap.
//
// Seeds: the world seed is MatchSetup::config.seed (the CLI's match k uses
// --seed + k); slot i's agent is init()ed with ai::agentSeed(world seed, i),
// the hosts' shared formula (kfm/ai/agent_host.hpp), so a module's seed is
// reproducible from the result line.
//
// Placements: the MatchEnded winner first, then by final score (round wins
// or points, MatchRules::scores) descending, then by fewer
// deaths, then by lower slot. The sim's winner is always the unique top
// scorer, so the first key only makes "winner N" and "place 1" agree by
// construction; a draw or a capped match falls through to the other keys.
// Every slot gets a distinct place 1..n.
//
// Teams (v0.35, kfm_arena --team): an entrant is a team (§8). With
// MatchSetup::teams set, entrant e plays the mode's team (e + rotation) mod T
// (so --rotate moves every entrant through every side), its specs fill that
// team's slots in slot order (the last one repeating), every team has the
// longest list's size and the slots are 0..T·size-1 split round-robin
// (teamsFor: slot s is team s mod T). Team places: the team of the
// MatchEnded winner first, then the higher team score (MatchContent::
// teamScore), then fewer deaths, then the lower team index. The slots keep
// their own places as without teams.
#pragma once

#include "kfm/ai/agent.hpp"
#include "kfm/ai/agent_host.hpp"
#include "kfm/ai/bot_spec.hpp"
#include "kfm/core/types.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/world_state.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace kfm::ai {
class WasmAgent;
}

namespace kfm::arena {

inline constexpr uint32_t kDefaultTickCap = 36'000;  // 10 minutes at 60 Hz
inline constexpr uint32_t kDefaultFuelPerTick = 100'000;
inline constexpr uint64_t kDefaultSeed = 1;

// Whether this build runs bot modules (KFM_WASM_BOTS).
[[nodiscard]] bool wasmBotsAvailable();

enum class BotErrorKind : uint8_t {
  Io,           // the module file could not be read
  Load,         // WasmModule::load refused it (message: the LoadError)
  Unsupported,  // a module spec in a build without KFM_WASM_BOTS
};

struct BotError {
  BotErrorKind kind{BotErrorKind::Load};
  std::string message;  // one line
};

// Builds agents from specs. A module file is read and WasmModule::load()ed
// once per distinct path (the cache); every agent made from it gets its own
// instance and linear memory. Owns the WasmRuntime while modules are loaded.
class BotFactory {
 public:
  explicit BotFactory(uint32_t fuelPerTick = kDefaultFuelPerTick);
  ~BotFactory();
  BotFactory(const BotFactory&) = delete;
  BotFactory& operator=(const BotFactory&) = delete;
  BotFactory(BotFactory&&) = delete;
  BotFactory& operator=(BotFactory&&) = delete;

  // Loads a module spec's file into the cache (a reference spec: nothing to do).
  [[nodiscard]] std::expected<void, BotError> prepare(const ai::BotSpec& spec);

  struct Made {
    std::unique_ptr<ai::IAgent> agent;
    ai::WasmAgent* wasm{nullptr};  // the same agent when it is a module, for its stats
  };
  // A fresh agent for the spec (prepare()s a module first).
  [[nodiscard]] std::expected<Made, BotError> make(const ai::BotSpec& spec);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct MatchSetup {
  // seed, arena, mode, loadout and powerUps; players is set by runMatch from bots.
  sim::MatchConfig config{.seed = kDefaultSeed};
  std::vector<ai::BotSpec> bots;  // 1..kMaxPlayers; bot b plays slot (b + rotation) mod n
  // v0.35: the entrants of a team match (see the header comment); when set,
  // `bots` is ignored. teams.size() must equal the mode's team count.
  std::vector<std::vector<ai::BotSpec>> teams;
  // v0.35: memory_on_respawn for every bot (AgentHost::setRespawnMemory).
  ai::RespawnMemory memory{ai::RespawnMemory::Keep};
  // kfm_arena --rotate sets it to the match index k, so across n matches every
  // bot plays every slot (and with it every spawn position and tie-break order).
  uint32_t rotation{0};
  uint32_t tickCap{kDefaultTickCap};
  uint8_t delayTicks{0};  // AgentHost's per-slot input delay (0: bot versus bot)
  bool recordReplay{false};
};

struct SlotResult {
  uint8_t slot{0};
  // The bot's index in MatchSetup::bots (== slot without rotation); in a
  // team match entrant × team size + its position in the team.
  uint8_t botIndex{0};
  uint8_t team{0};  // MatchConfig::teams[slot] (slot i in a free-for-all)
  std::string bot;  // toString(spec): "ref:hard" or the module path
  bool module{false};
  uint8_t place{0};    // 1..n
  int32_t score{0};    // MatchRules::scores (signed since v0.57: a suicide may go below 0)
  uint32_t kills{0};   // PlayerEliminated with this slot as the killer (not itself)
  uint32_t deaths{0};  // PlayerEliminated with this slot as the victim
  uint32_t acts{0};    // AgentHost counters (acts: ticks with a monkey, v0.35)
  uint32_t substitutions{0};
  uint32_t paused{0};  // v0.35: ticks the slot sat out eliminated (acts + paused = ticks)
  // Module only (WasmAgentStats):
  uint64_t traps{0};
  uint64_t overBudget{0};
  uint64_t badReturns{0};
  uint64_t fuelMean{0};  // fuelTotal / acts, rounded down
  uint32_t fuelMax{0};
  uint32_t fuelInit{0};
  uint64_t logBytes{0};
  std::string dead;  // the death cause (toString(AgentFault)), empty while alive
  std::string log;   // the captured kfm_log output
};

// One team of a team match (v0.35).
struct TeamResult {
  uint8_t team{0};     // the mode's team index (its side)
  uint8_t entrant{0};  // index into MatchSetup::teams
  std::string bots;    // the specs of its slots in slot order, comma-separated
  uint8_t place{0};    // 1..T
  int32_t score{0};    // MatchContent::teamScore (signed since v0.57)
  uint32_t kills{0};   // its slots' kills of enemies
  uint32_t deaths{0};
};

enum class MatchOutcome : uint8_t {
  Ended,  // MatchEnded (winner invalid: a draw)
  Cap,    // the tick cap was reached first
};

struct MatchResult {
  uint32_t index{0};
  uint64_t seed{0};
  uint32_t ticks{0};     // steps run (the replay's tickCount)
  uint32_t checksum{0};  // sim::checksum of the final state (what replaying the .kfmr reaches)
  MatchOutcome outcome{MatchOutcome::Cap};
  PlayerId winner{PlayerId::invalid()};
  std::vector<SlotResult> slots;  // by slot
  std::vector<TeamResult> teams;  // v0.35: by team index, team matches only
  std::vector<std::byte> replay;  // the .kfmr bytes when recordReplay
  // v0.60: the replay header's two hashes, set whether or not a replay is
  // recorded: sim::contentHash of the match's content, and the build hash
  // (0: no build stamps one yet, as in every replay header).
  uint32_t contentHash{0};
  uint32_t buildHash{0};
};

// Plays one match. `index` is only carried into the result. Fails only when
// the setup has no bots or too many, or a bot cannot be built (the
// factory's error).
[[nodiscard]] std::expected<MatchResult, BotError> runMatch(const MatchSetup& setup,
                                                            const sim::ContentDB& content,
                                                            BotFactory& factory,
                                                            uint32_t index = 0);

// Assigns SlotResult::place by the rule above (also used by the tests).
void assignPlaces(MatchResult& result);
// Assigns TeamResult::place by the team rule above.
void assignTeamPlaces(MatchResult& result);

// The totals of a tournament for one slot, or (Summary::bots) for one bot.
struct SlotSummary {
  uint8_t index{0};      // the slot, or the bot's index in MatchSetup::bots
  std::string bot;       // its spec text; for a slot that several bots played: "mixed"
  bool module{false};    // a module played it in some match
  uint32_t wins{0};      // matches this slot is the MatchEnded winner of
  uint64_t placeSum{0};  // for the mean place
  int64_t score{0};
  uint64_t kills{0};
  uint64_t deaths{0};
  uint64_t acts{0};
  uint64_t substitutions{0};
  uint64_t traps{0};
  uint64_t overBudget{0};
  uint64_t badReturns{0};
  uint32_t fuelMax{0};
};

// The totals of a tournament for one entrant of a team match (v0.35).
struct EntrantSummary {
  uint8_t index{0};
  std::string bots;      // its specs
  uint32_t wins{0};      // matches its team won
  uint64_t placeSum{0};  // team places, for the mean
  int64_t score{0};
  uint64_t kills{0};
  uint64_t deaths{0};
};

struct Summary {
  uint32_t matches{0};
  uint32_t ended{0};   // MatchEnded with a winner
  uint32_t draws{0};   // MatchEnded without one
  uint32_t capped{0};  // the tick cap first
  uint64_t ticks{0};
  bool rotated{false};                   // some match put a bot in a slot other than its index
  std::vector<SlotSummary> slots;        // by slot
  std::vector<SlotSummary> bots;         // by bot index
  std::vector<EntrantSummary> entrants;  // v0.35: by entrant, team matches only

  void add(const MatchResult& result);
};

// v0.35, a team match: "winner_team T|none" follows "winner W", then one
// segment per team before the slots:
//   ... winner 3 winner_team 1 | team 0 entrant 1 ref:hard,ref:hard place 2 score 1 kills 4
//   deaths 6 | team 1 entrant 0 bots/x.wasm,bots/x.wasm place 1 score 3 kills 6 deaths 4 |
//   slot 0 ...
// and every slot segment carries "team T" after its bot text. The summary
// appends " | entrant E SPECS wins W mean_place X score S kills K deaths D"
// per entrant; the JSON forms add "winner_team", "teams":[...] (before
// "slots"), a slot's "team" and the summary's "entrants":[...].
//
// The result line (no trailing newline), fields in this order:
//   match 0 seed 7 ticks 5432 checksum 0c0ffee1 result ended winner 1 | slot 0 bot 0
//   ref:hard place 2 score 1 kills 3 deaths 2 acts 5432 subs 0 | slot 1 bot 1 bots/x.wasm
//   place 1 score 3 kills 5 deaths 1 acts 5432 subs 3 traps 1 over 2 bad 0 fuel_mean 61234
//   fuel_max 62731 fuel_init 13 log 0 dead no
// checksum: the final state's, 8 hex digits; result: ended (MatchEnded) or
// cap; winner: a slot, or none (a draw, the cap).
// The module fields (traps .. dead) follow a module slot only; dead is the
// death cause (init-trap, ...) or no.
[[nodiscard]] std::string formatResultLine(const MatchResult& result);
// bot: the bot's index in MatchSetup::bots (the --bot order, padding last).
// The same as one JSON object on one line:
//   {"match":0,"seed":7,"ticks":5432,"checksum":"0c0ffee1","result":"ended","winner":1,
//   "slots":[{"slot":0,"bot":0,"spec":"ref:hard","module":false,"place":2,...},...],
//   "content_hash":"5e1f00d2","build_hash":"00000000"}
// winner and dead are null when none; module slots carry the module fields.
// The JSON objects are a contract with the ladder (docs/arena-result.md):
// keys are only ever appended, never renamed, removed or retyped. The text
// lines are for humans and make no such promise.
[[nodiscard]] std::string formatResultJson(const MatchResult& result);
// The tournament line:
//   summary matches 20 ended 19 draws 1 cap 0 ticks_mean 5432 | slot 0 ref:hard wins 7
//   mean_place 1.30 score 21 kills 40 deaths 31 acts 108640 subs 0 | slot 1 x.wasm ...
//   subs 3 traps 0 over 3 bad 0 fuel_max 62731
// A rotated tournament labels a slot that several bots played "mixed" and
// appends the same totals per bot, by index in the --bot order:
//   ... | slot 1 mixed wins 5 ... | bot 0 ref:hard wins 9 mean_place 1.80 ... | bot 1 ...
// The JSON: {"summary":true,...,"slots":[{"slot":0,"spec":"ref:hard",...},...]}, and
// when rotated "bots":[{"bot":0,"spec":"ref:hard",...},...] after "slots".
[[nodiscard]] std::string formatSummaryLine(const Summary& summary);
[[nodiscard]] std::string formatSummaryJson(const Summary& summary);

}  // namespace kfm::arena
