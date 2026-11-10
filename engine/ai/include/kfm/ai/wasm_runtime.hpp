// The wasm bot runtime (architecture §8): WasmRuntime, WasmModule and
// WasmAgent, the IAgent that runs a sandboxed WebAssembly bot module, metered
// in wasm instructions per tick. Built only with KFM_WASM_BOTS (native; the
// browser instantiates bot modules through its own WebAssembly API). The
// runtime underneath is WAMR 2.4.5's classic interpreter (kfm::wamr,
// third_party/wamr); nothing in this header depends on it.
//
// Bot ABI v1 (docs/bot-abi.md). A module imports nothing but, optionally,
// `env.kfm_log(ptr: i32, len: i32)`; a module importing anything else is
// refused at load. No WASI. A `start` function may run at instantiation, under
// the init budget. Exports, checked by name and signature at load:
//   kfm_abi_version() -> i32        must equal kObservationVersion
//   kfm_obs_buffer() -> i32         address in linear memory of at least
//                                   sizeof(Observation) bytes; the host
//                                   writes the observation there each tick
//   kfm_init(seed: i64, slot: i32)  once per instance, before the first act
//   kfm_act(tick: i32) -> i64       the PlayerInput packed little-endian in
//                                   the low 40 bits (packInput); bits 40..63
//                                   set is a fault (BadReturn)
//
// Shadow stack: compiled modules (clang, rustc) keep their C stack in linear
// memory behind a mutable i32 global, `__stack_pointer`. A kfm_act cut short
// by a trap or the budget never pops its frames; when the module exports
// that global (wasm-ld: --export=__stack_pointer, or --export-all), the
// host restores the value it had before the call, so a fault costs the tick
// and nothing else. Without the export the module's own state after a fault
// is its own business.
//
// Threading: single-threaded, like the whole client path. WAMR's runtime
// state is process-wide (native registry, allocator, the instantiation
// budget), so every call on these classes must come from one thread.
//
// Memory growth: a module's memory is capped at maxMemoryBytes at
// instantiation (memory.grow past it returns -1 to the module, it never
// traps). Growth can move the linear memory, so the host resolves the
// observation buffer's address to a host pointer again before every act;
// the wasm address itself stays valid because memory never shrinks.
#pragma once

#include "kfm/ai/agent.hpp"
#include "kfm/ai/observation.hpp"
#include "kfm/core/types.hpp"
#include "kfm/sim/input.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace kfm::ai {

// Host settings, applied per module at load and to every agent made from it.
// Instruction budgets count wasm instructions as WAMR's classic interpreter
// meters them (one per executed opcode plus one per function entry), plus
// the extra cost of an opcode whose work grows with an operand (bulk memory
// and table ops, memory.grow and table.grow: docs/bot-abi.md §1.4); they
// never depend on the machine or on wall-clock time.
struct WasmAgentConfig {
  // kfm_act's budget per tick. Over it, the tick is lost (OverBudget).
  uint32_t fuelPerTick{100'000};
  // The budget of instantiation (a start function, an exported
  // __wasm_call_ctors) and, separately, of each kfm_init call. Over it, the
  // load is refused (probe) or the agent is dead (init).
  uint32_t fuelInit{10'000'000};
  // Size cap of the module file.
  uint32_t maxModuleBytes{4u << 20};
  // Linear memory cap: a module whose declared initial memory exceeds it is
  // refused at load; its maximum is clamped to it.
  uint32_t maxMemoryBytes{16u << 20};
  // Table cap: a module whose tables would allocate more elements than this
  // in all (each table's initial size, or its maximum when the module can
  // grow it) is refused at load. Instantiation fills them outside any
  // budget.
  uint32_t maxTableElements{1u << 16};
  // The interpreter's operand/call stack per instance (outside linear
  // memory; overflow traps).
  uint32_t stackBytes{64u << 10};
  // kfm_log capture per agent: the last logCaptureBytes bytes are kept, the
  // rest counted in WasmAgentStats::logBytesDropped. 0 discards everything.
  uint32_t logCaptureBytes{4u << 10};
};

// kfm_act's return value: the five PlayerInput bytes little-endian in the
// low 40 bits (byte 0 moveDir, 1 moveMag, 2 aimDir, 3 buttons, 4 subTick).
[[nodiscard]] constexpr uint64_t packInput(const sim::PlayerInput& in) {
  return static_cast<uint64_t>(in.moveDir) | (static_cast<uint64_t>(in.moveMag) << 8u) |
         (static_cast<uint64_t>(in.aimDir) << 16u) | (static_cast<uint64_t>(in.buttons) << 24u) |
         (static_cast<uint64_t>(in.subTick) << 32u);
}

// nullopt when any of bits 40..63 is set.
[[nodiscard]] constexpr std::optional<sim::PlayerInput> unpackInput(uint64_t packed) {
  if ((packed >> 40u) != 0) return std::nullopt;
  const auto byte = [packed](unsigned i) { return static_cast<uint8_t>(packed >> (8u * i)); };
  return sim::PlayerInput{.moveDir = byte(0),
                          .moveMag = byte(1),
                          .aimDir = byte(2),
                          .buttons = byte(3),
                          .subTick = byte(4)};
}

// Process-wide WAMR initialisation (and the kfm_log native). Hold one for as
// long as modules are loaded; WasmModule::load fails without one. Nested
// instances share the runtime (reference counted); modules and agents keep
// it alive themselves, so destruction order does not matter.
class WasmRuntime {
 public:
  WasmRuntime();
  ~WasmRuntime();
  WasmRuntime(const WasmRuntime&) = delete;
  WasmRuntime& operator=(const WasmRuntime&) = delete;
  WasmRuntime(WasmRuntime&&) = delete;
  WasmRuntime& operator=(WasmRuntime&&) = delete;

  // false when WAMR failed to initialise (out of memory).
  [[nodiscard]] bool ok() const { return ok_; }
  // Whether any WasmRuntime is alive.
  [[nodiscard]] static bool active();

 private:
  bool ok_{false};
};

enum class LoadErrorKind : uint8_t {
  NoRuntime,       // no WasmRuntime alive
  TooLarge,        // over maxModuleBytes
  Malformed,       // WAMR's loader refused the bytes (not wasm, invalid, unsupported feature)
  BadImport,       // imports something other than env.kfm_log (i32, i32) -> ()
  MissingExport,   // a required export is absent or not a function
  BadSignature,    // a required export has the wrong type
  NoMemory,        // no linear memory
  MemoryTooLarge,  // declared initial memory over maxMemoryBytes
  Instantiate,   // instantiation failed: start function trapped or ran over fuelInit, out of memory
  AbiMismatch,   // kfm_abi_version() != kObservationVersion
  BadObsBuffer,  // kfm_obs_buffer() does not point at sizeof(Observation) bytes of memory
  TableTooLarge,  // its tables would allocate more than maxTableElements elements
};

struct LoadError {
  LoadErrorKind kind{LoadErrorKind::Malformed};
  std::string message;  // one line, for logs and the arena's result
};

[[nodiscard]] std::string_view toString(LoadErrorKind kind);

// A validated bot module: the bytes, loaded and checked once (size, imports,
// exports and their signatures, memory cap, and, on a probe instance, the
// start function, the ABI version and the observation buffer). Cheap to
// copy: copies share the loaded module, and each WasmAgent made from it gets
// its own instance and linear memory.
class WasmModule {
 public:
  [[nodiscard]] static std::expected<WasmModule, LoadError> load(
      std::span<const std::byte> bytes, const WasmAgentConfig& config = {});

  [[nodiscard]] const WasmAgentConfig& config() const;
  [[nodiscard]] bool importsLog() const;  // whether the module imports env.kfm_log

  struct Impl;

 private:
  explicit WasmModule(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
  std::shared_ptr<Impl> impl_;
  friend class WasmAgent;
};

enum class AgentFault : uint8_t {
  None,
  // act():
  Trap,  // the module trapped in kfm_act (unreachable, out-of-bounds access, stack overflow, bad
         // kfm_log)
  OverBudget,  // kfm_act ran over fuelPerTick
  BadReturn,   // kfm_act set bits 40..63
  Dead,        // init() never ran or failed: the module is not called
  // init() (the agent is dead afterwards):
  Instantiate,     // instantiation failed (start function trap / over fuelInit, out of memory)
  AbiMismatch,     // kfm_abi_version() changed its mind since load
  BadObsBuffer,    // kfm_obs_buffer() no longer valid
  InitTrap,        // kfm_abi_version, kfm_obs_buffer or kfm_init trapped
  InitOverBudget,  // ... or ran over its budget
};

[[nodiscard]] std::string_view toString(AgentFault fault);

struct WasmAgentStats {
  uint64_t acts{0};           // act() calls
  uint64_t substitutions{0};  // act() calls that returned nullopt (every fault, and a dead agent)
  uint64_t traps{0};          // AgentFault::Trap
  uint64_t overBudget{0};     // AgentFault::OverBudget
  uint64_t badReturns{0};     // AgentFault::BadReturn
  uint32_t fuelLast{0};   // instructions the latest kfm_act used (== fuelPerTick when over budget)
  uint32_t fuelMax{0};    // the most any kfm_act used
  uint64_t fuelTotal{0};  // sum over every kfm_act call
  uint32_t fuelInit{0};   // instructions the latest kfm_init used
  uint64_t logBytes{0};   // bytes passed to kfm_log
  uint64_t logBytesDropped{0};  // of those, not kept (over logCaptureBytes)
};

// One bot module instance in one slot: its own linear memory, globals,
// budget and stats, even when several agents share a WasmModule.
class WasmAgent final : public IAgent {
 public:
  explicit WasmAgent(WasmModule module);
  ~WasmAgent() override;
  WasmAgent(const WasmAgent&) = delete;
  WasmAgent& operator=(const WasmAgent&) = delete;
  // A moved-from agent may only be destroyed or assigned to.
  WasmAgent(WasmAgent&&) noexcept;
  WasmAgent& operator=(WasmAgent&&) noexcept;

  // Instantiates the module afresh (new linear memory: a second init() is a
  // wipe), resolves the observation buffer and calls kfm_init(seed, slot)
  // under fuelInit. A fault leaves the agent dead: every act() returns
  // nullopt without calling into the module, until a successful init().
  void init(uint64_t seed, PlayerId slot) override;
  // Copies `obs` into the module's buffer and calls kfm_act(tick) under
  // fuelPerTick. nullopt on any fault (the host repeats the last input).
  std::optional<sim::PlayerInput> act(const Observation& obs, Tick tick) override;

  [[nodiscard]] bool alive() const;
  // The fault of the latest init() or act() (None when it succeeded; Dead
  // for an act() on a dead agent).
  [[nodiscard]] AgentFault lastFault() const;
  // Why the agent is dead: the init() fault, Dead before the first init(),
  // None while alive.
  [[nodiscard]] AgentFault deathCause() const;
  // WAMR's exception text for lastFault(), e.g. "unreachable"; empty for None.
  [[nodiscard]] std::string_view lastFaultMessage() const;
  [[nodiscard]] const WasmAgentStats& stats() const;
  // The captured kfm_log output so far (at most logCaptureBytes, the most
  // recent bytes), which is cleared.
  [[nodiscard]] std::string takeLog();

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace kfm::ai
