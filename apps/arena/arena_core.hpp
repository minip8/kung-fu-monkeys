// kfm_arena's core (architecture §8, §9): one bot match played headless at
// full speed, its result and the result line. A library so the match runner
// is unit-tested without the CLI (tests/unit/arena), like kfm::server_host.
//
// A match: the MatchSetup's bots fill slots 0..n-1 in order; initialWorld()
// from the config (players = those slots); every tick AgentHost::fill() writes
// each bot's input into the frame, the frame is recorded, step() runs and
// the events are folded into per-slot counters; the match stops at
// MatchEnded or at the tick cap.
//
// Seeds: the world seed is MatchSetup::config.seed (the CLI's match k uses
// --seed + k); slot i's agent is init()ed with slotSeed(world seed, i), a
// fixed formula so a module's seed is reproducible from the result line.
//
// Placements: the MatchEnded winner first, then by final score (round wins
// or deathmatch eliminations, MatchRules::scores) descending, then by fewer
// deaths, then by lower slot. The sim's winner is always the unique top
// scorer, so the first key only makes "winner N" and "place 1" agree by
// construction; a draw or a capped match falls through to the other keys.
// Every slot gets a distinct place 1..n.
#pragma once

#include "kfm/ai/agent.hpp"
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

// The seed slot `slot` is init()ed with: the world seed mixed with the
// golden-ratio constant times (slot + 1), so slots differ and slot 0 does not
// get the world seed itself.
[[nodiscard]] constexpr uint64_t slotSeed(uint64_t matchSeed, uint8_t slot) {
  return matchSeed ^ (0x9E3779B97F4A7C15ULL * (uint64_t{slot} + 1));
}

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
  // seed, arena, mode and powerUps; players is set by runMatch from bots.
  sim::MatchConfig config{.seed = kDefaultSeed};
  std::vector<ai::BotSpec> bots;  // slot i plays bots[i]; 1..kMaxPlayers
  uint32_t tickCap{kDefaultTickCap};
  uint8_t delayTicks{0};  // AgentHost's per-slot input delay (0: bot versus bot)
  bool recordReplay{false};
};

struct SlotResult {
  uint8_t slot{0};
  std::string bot;  // toString(spec): "ref:hard" or the module path
  bool module{false};
  uint8_t place{0};  // 1..n
  uint32_t score{0};
  uint32_t kills{0};   // PlayerEliminated with this slot as the killer (not itself)
  uint32_t deaths{0};  // PlayerEliminated with this slot as the victim
  uint32_t acts{0};    // AgentHost counters
  uint32_t substitutions{0};
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
  std::vector<std::byte> replay;  // the .kfmr bytes when recordReplay
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

// The per-slot totals of a tournament.
struct SlotSummary {
  uint8_t slot{0};
  std::string bot;
  bool module{false};
  uint32_t wins{0};      // matches this slot is the MatchEnded winner of
  uint64_t placeSum{0};  // for the mean place
  uint64_t score{0};
  uint64_t kills{0};
  uint64_t deaths{0};
  uint64_t acts{0};
  uint64_t substitutions{0};
  uint64_t traps{0};
  uint64_t overBudget{0};
  uint64_t badReturns{0};
  uint32_t fuelMax{0};
};

struct Summary {
  uint32_t matches{0};
  uint32_t ended{0};   // MatchEnded with a winner
  uint32_t draws{0};   // MatchEnded without one
  uint32_t capped{0};  // the tick cap first
  uint64_t ticks{0};
  std::vector<SlotSummary> slots;

  void add(const MatchResult& result);
};

// The result line (no trailing newline), fields in this order:
//   match 0 seed 7 ticks 5432 checksum 0c0ffee1 result ended winner 1 | slot 0 ref:hard
//   place 2 score 1 kills 3 deaths 2 acts 5432 subs 0 | slot 1 bots/x.wasm place 1 score 3
//   kills 5 deaths 1 acts 5432 subs 3 traps 1 over 2 bad 0 fuel_mean 61234 fuel_max 62731
//   fuel_init 13 log 0 dead no
// checksum: the final state's, 8 hex digits; result: ended (MatchEnded) or
// cap; winner: a slot, or none (a draw, the cap).
// The module fields (traps .. dead) follow a module slot only; dead is the
// death cause (init-trap, ...) or no.
[[nodiscard]] std::string formatResultLine(const MatchResult& result);
// The same as one JSON object on one line:
//   {"match":0,"seed":7,"ticks":5432,"checksum":"0c0ffee1","result":"ended","winner":1,
//   "slots":[{"slot":0,"bot":"ref:hard","module":false,"place":2,...},...]}
// winner and dead are null when none; module slots carry the module fields.
[[nodiscard]] std::string formatResultJson(const MatchResult& result);
// The tournament line:
//   summary matches 20 ended 19 draws 1 cap 0 ticks_mean 5432 | slot 0 ref:hard wins 7
//   mean_place 1.30 score 21 kills 40 deaths 31 acts 108640 subs 0 | slot 1 x.wasm ...
//   subs 3 traps 0 over 3 bad 0 fuel_max 62731
[[nodiscard]] std::string formatSummaryLine(const Summary& summary);
[[nodiscard]] std::string formatSummaryJson(const Summary& summary);

}  // namespace kfm::arena
