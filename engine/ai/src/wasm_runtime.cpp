// WasmRuntime, WasmModule and WasmAgent over WAMR's classic interpreter
// (architecture §8; the WAMR configuration and its source patch are in
// third_party/wamr/CMakeLists.txt). Single-threaded throughout: WAMR's
// runtime state, the kfm_log registry and the instantiation budget are
// process-wide.
#include "kfm/ai/wasm_runtime.hpp"

#include <kfm_wamr_ext.h>
#include <wasm_export.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <vector>

namespace kfm::ai {

namespace {

constexpr uint32_t kPageBytes = 64u * 1024u;
constexpr uint32_t kErrorBufBytes = 256;

// ---------------------------------------------------------------------------
// The process-wide runtime. WasmRuntime handles, modules and agents each hold
// a reference; WAMR is torn down when the last one goes.
// ---------------------------------------------------------------------------
int gRuntimeRefs = 0;     // every holder
int gRuntimeHandles = 0;  // WasmRuntime objects only (WasmRuntime::active)

struct AgentLogSink {
  std::string* log{nullptr};
  WasmAgentStats* stats{nullptr};
  uint32_t capacity{0};
};

void appendLog(const AgentLogSink& sink, const char* data, uint32_t len) {
  sink.stats->logBytes += len;
  if (sink.capacity == 0) {
    sink.stats->logBytesDropped += len;
    return;
  }
  // Keep the most recent `capacity` bytes.
  if (len >= sink.capacity) {
    sink.stats->logBytesDropped += sink.log->size() + (len - sink.capacity);
    sink.log->assign(data + (len - sink.capacity), sink.capacity);
    return;
  }
  const size_t total = sink.log->size() + len;
  if (total > sink.capacity) {
    const size_t drop = total - sink.capacity;
    sink.stats->logBytesDropped += drop;
    sink.log->erase(0, drop);
  }
  sink.log->append(data, len);
}

// env.kfm_log(ptr: i32, len: i32), registered in WAMR's raw form ("(*~)":
// WAMR checks that [ptr, ptr + len) lies in linear memory, raising an
// out-of-bounds trap otherwise, and passes the host pointer). Each argument
// sits in its own 64-bit slot. The probe instance made at load has no sink
// and discards.
void kfmLogRaw(wasm_exec_env_t env, uint64_t* args) {
  void* data = nullptr;
  uint32_t len = 0;
  std::memcpy(static_cast<void*>(&data), static_cast<const void*>(&args[0]), sizeof data);
  std::memcpy(&len, static_cast<const void*>(&args[1]), sizeof len);
  const auto* sink = static_cast<const AgentLogSink*>(wasm_runtime_get_user_data(env));
  if (sink != nullptr && len > 0) appendLog(*sink, static_cast<const char*>(data), len);
}

// WAMR keeps a pointer to this table until wasm_runtime_destroy.
std::array<NativeSymbol, 1> gNatives{{
    {"kfm_log", reinterpret_cast<void*>(&kfmLogRaw), "(*~)", nullptr},
}};

bool acquireRuntime() {
  if (gRuntimeRefs == 0) {
    // Zeroed like WAMR's own samples; running_mode (no zero enumerator) is set below.
    RuntimeInitArgs args{};  // NOLINT(bugprone-invalid-enum-default-initialization)
    args.mem_alloc_type = Alloc_With_System_Allocator;
    args.running_mode = Mode_Interp;
    if (!wasm_runtime_full_init(&args)) return false;
    // Module and call errors come back as strings; nothing else is worth
    // printing (link warnings for refused imports, for one).
    wasm_runtime_set_log_level(WASM_LOG_LEVEL_FATAL);
    if (!wasm_runtime_register_natives_raw("env", gNatives.data(),
                                           static_cast<uint32_t>(gNatives.size()))) {
      wasm_runtime_destroy();
      return false;
    }
  }
  ++gRuntimeRefs;
  return true;
}

void releaseRuntime() {
  if (--gRuntimeRefs == 0) {
    wasm_runtime_unregister_natives("env", gNatives.data());
    wasm_runtime_destroy();
  }
}

// ---------------------------------------------------------------------------
// Signatures
// ---------------------------------------------------------------------------
struct Signature {
  std::span<const wasm_valkind_t> params;
  std::span<const wasm_valkind_t> results;
};

bool matches(wasm_func_type_t type, const Signature& sig) {
  if (wasm_func_type_get_param_count(type) != sig.params.size() ||
      wasm_func_type_get_result_count(type) != sig.results.size()) {
    return false;
  }
  for (uint32_t i = 0; i < sig.params.size(); ++i) {
    if (wasm_func_type_get_param_valkind(type, i) != sig.params[i]) return false;
  }
  for (uint32_t i = 0; i < sig.results.size(); ++i) {
    if (wasm_func_type_get_result_valkind(type, i) != sig.results[i]) return false;
  }
  return true;
}

constexpr std::array<wasm_valkind_t, 0> kNone{};
constexpr std::array<wasm_valkind_t, 1> kI32{WASM_I32};
constexpr std::array<wasm_valkind_t, 1> kI64{WASM_I64};
constexpr std::array<wasm_valkind_t, 2> kI32I32{WASM_I32, WASM_I32};
constexpr std::array<wasm_valkind_t, 2> kI64I32{WASM_I64, WASM_I32};

struct RequiredExport {
  const char* name{nullptr};
  Signature sig;
  const char* text{nullptr};
};

constexpr std::array<RequiredExport, 4> kExports{{
    {"kfm_abi_version", {kNone, kI32}, "() -> i32"},
    {"kfm_obs_buffer", {kNone, kI32}, "() -> i32"},
    {"kfm_init", {kI64I32, kNone}, "(i64, i32) -> ()"},
    {"kfm_act", {kI32, kI64}, "(i32) -> i64"},
}};
constexpr Signature kLogSignature{kI32I32, kNone};

std::string exceptionText(wasm_module_inst_t inst) {
  const char* ex = wasm_runtime_get_exception(inst);
  if (ex == nullptr) return {};
  std::string_view text{ex};
  constexpr std::string_view kPrefix{"Exception: "};
  if (text.starts_with(kPrefix)) text.remove_prefix(kPrefix.size());
  return std::string{text};
}

bool isOverBudget(std::string_view exception) {
  return exception.contains("instruction limit exceeded");
}

// ---------------------------------------------------------------------------
// One instance of a module: its linear memory, exec env and the four exports.
// ---------------------------------------------------------------------------
struct Instance {
  wasm_module_inst_t inst{nullptr};
  wasm_exec_env_t env{nullptr};
  wasm_function_inst_t abiVersion{nullptr};
  wasm_function_inst_t obsBuffer{nullptr};
  wasm_function_inst_t initFn{nullptr};
  wasm_function_inst_t actFn{nullptr};

  Instance() = default;
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;
  Instance(Instance&&) = delete;
  Instance& operator=(Instance&&) = delete;
  ~Instance() {
    if (env != nullptr) wasm_runtime_destroy_exec_env(env);
    if (inst != nullptr) wasm_runtime_deinstantiate(inst);
  }
};

struct CallResult {
  bool ok{false};
  bool overBudget{false};
  uint32_t fuelUsed{0};
  std::string error;  // WAMR's exception text when !ok
};

// Calls `fn` with `cells` (WAMR's 32-bit argument/result cells) under a
// budget of `fuel` instructions, and reads back how much of it was used.
CallResult call(const Instance& in, wasm_function_inst_t fn, std::span<uint32_t> cells,
                uint32_t argc, uint32_t fuel) {
  wasm_runtime_clear_exception(in.inst);
  wasm_runtime_set_instruction_count_limit(in.env, static_cast<int>(fuel));
  CallResult r;
  r.ok = wasm_runtime_call_wasm(in.env, fn, argc, cells.data());
  // The patched interpreter stores the budget left on return (0 when it ran
  // out); a call that never reached the interpreter leaves the limit as set.
  const int left = kfm_wamr_instructions_left(in.env);
  r.fuelUsed = fuel - static_cast<uint32_t>(std::clamp(left, 0, static_cast<int>(fuel)));
  if (!r.ok) {
    r.error = exceptionText(in.inst);
    r.overBudget = isOverBudget(r.error);
  }
  return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// WasmRuntime
// ---------------------------------------------------------------------------
WasmRuntime::WasmRuntime() : ok_(acquireRuntime()) {
  if (ok_) ++gRuntimeHandles;
}

WasmRuntime::~WasmRuntime() {
  if (ok_) {
    --gRuntimeHandles;
    releaseRuntime();
  }
}

bool WasmRuntime::active() {
  return gRuntimeHandles > 0;
}

// ---------------------------------------------------------------------------
// WasmModule
// ---------------------------------------------------------------------------
struct WasmModule::Impl {
  WasmAgentConfig config;
  // WAMR's loader rewrites parts of the buffer in place and the interpreter
  // runs the code from it, so the module owns a private copy for its whole
  // life.
  std::vector<uint8_t> bytes;
  wasm_module_t module{nullptr};
  bool importsLog{false};
  bool holdsRuntime{false};

  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl() {
    if (module != nullptr) wasm_runtime_unload(module);
    if (holdsRuntime) releaseRuntime();
  }

  // A fresh instance with its memory capped and its start function run
  // under fuelInit; the four exports resolved.
  [[nodiscard]] std::unique_ptr<Instance> instantiate(std::string& error) const {
    auto in = std::make_unique<Instance>();
    InstantiationArgs args{};
    args.default_stack_size = config.stackBytes;
    args.host_managed_heap_size = 0;
    args.max_memory_pages = std::max<uint32_t>(config.maxMemoryBytes / kPageBytes, 1);
    std::array<char, kErrorBufBytes> err{};
    // The start function and __wasm_call_ctors run on an exec env WAMR
    // creates inside the call; the patched runtime gives it this budget.
    kfm_wamr_set_default_instruction_limit(static_cast<int>(config.fuelInit));
    in->inst = wasm_runtime_instantiate_ex(module, &args, err.data(), err.size());
    kfm_wamr_set_default_instruction_limit(-1);
    if (in->inst == nullptr) {
      error = err.data();
      return nullptr;
    }
    in->env = wasm_runtime_create_exec_env(in->inst, config.stackBytes);
    if (in->env == nullptr) {
      error = "out of memory creating the execution environment";
      return nullptr;
    }
    in->abiVersion = wasm_runtime_lookup_function(in->inst, "kfm_abi_version");
    in->obsBuffer = wasm_runtime_lookup_function(in->inst, "kfm_obs_buffer");
    in->initFn = wasm_runtime_lookup_function(in->inst, "kfm_init");
    in->actFn = wasm_runtime_lookup_function(in->inst, "kfm_act");
    if (in->abiVersion == nullptr || in->obsBuffer == nullptr || in->initFn == nullptr ||
        in->actFn == nullptr) {
      error = "export lookup failed";  // checked at load; unreachable
      return nullptr;
    }
    return in;
  }
};

namespace {

std::unexpected<LoadError> fail(LoadErrorKind kind, std::string message) {
  return std::unexpected(LoadError{.kind = kind, .message = std::move(message)});
}

// kfm_abi_version and kfm_obs_buffer on `in`: the version, and whether the
// buffer holds an Observation. Shared by the load probe and WasmAgent::init.
struct AbiCheck {
  AgentFault fault{AgentFault::None};
  std::string message;
  uint32_t obsAddr{0};
};

AbiCheck checkAbi(const Instance& in, uint32_t fuel) {
  std::array<uint32_t, 2> cells{};
  CallResult r = call(in, in.abiVersion, cells, 0, fuel);
  if (!r.ok) {
    return {r.overBudget ? AgentFault::InitOverBudget : AgentFault::InitTrap,
            "kfm_abi_version: " + r.error, 0};
  }
  if (cells[0] != kObservationVersion) {
    return {AgentFault::AbiMismatch,
            "kfm_abi_version() is " + std::to_string(cells[0]) + ", the host speaks " +
                std::to_string(kObservationVersion),
            0};
  }
  cells = {};
  r = call(in, in.obsBuffer, cells, 0, fuel);
  if (!r.ok) {
    return {r.overBudget ? AgentFault::InitOverBudget : AgentFault::InitTrap,
            "kfm_obs_buffer: " + r.error, 0};
  }
  const uint32_t addr = cells[0];
  if (!wasm_runtime_validate_app_addr(in.inst, addr, sizeof(Observation))) {
    wasm_runtime_clear_exception(in.inst);
    return {AgentFault::BadObsBuffer,
            "kfm_obs_buffer() = " + std::to_string(addr) + " has no room for the " +
                std::to_string(sizeof(Observation)) + "-byte observation",
            0};
  }
  return {AgentFault::None, {}, addr};
}

}  // namespace

std::expected<WasmModule, LoadError> WasmModule::load(std::span<const std::byte> bytes,
                                                      const WasmAgentConfig& config) {
  if (!WasmRuntime::active()) return fail(LoadErrorKind::NoRuntime, "no WasmRuntime is alive");
  if (bytes.size() > config.maxModuleBytes) {
    return fail(LoadErrorKind::TooLarge, "module is " + std::to_string(bytes.size()) +
                                             " bytes, the cap is " +
                                             std::to_string(config.maxModuleBytes));
  }
  auto impl = std::make_shared<Impl>();
  impl->config = config;
  if (!acquireRuntime()) return fail(LoadErrorKind::NoRuntime, "WAMR failed to initialise");
  impl->holdsRuntime = true;
  impl->bytes.resize(bytes.size());
  if (!bytes.empty()) std::memcpy(impl->bytes.data(), bytes.data(), bytes.size());

  std::array<char, kErrorBufBytes> err{};
  impl->module = wasm_runtime_load(impl->bytes.data(), static_cast<uint32_t>(impl->bytes.size()),
                                   err.data(), err.size());
  if (impl->module == nullptr) return fail(LoadErrorKind::Malformed, err.data());

  // Imports: env.kfm_log (i32, i32) -> () and nothing else.
  const int32_t imports = wasm_runtime_get_import_count(impl->module);
  for (int32_t i = 0; i < imports; ++i) {
    wasm_import_t imp{};
    wasm_runtime_get_import_type(impl->module, i, &imp);
    const std::string_view mod{imp.module_name != nullptr ? imp.module_name : ""};
    const std::string_view name{imp.name != nullptr ? imp.name : ""};
    const bool isLog =
        imp.kind == WASM_IMPORT_EXPORT_KIND_FUNC && mod == "env" && name == "kfm_log";
    if (!isLog) {
      return fail(LoadErrorKind::BadImport, "imports " + std::string{mod} + "." +
                                                std::string{name} +
                                                "; only env.kfm_log is allowed");
    }
    if (!matches(imp.u.func_type, kLogSignature)) {
      return fail(LoadErrorKind::BadImport,
                  "imports env.kfm_log with a type other than (i32, i32) -> ()");
    }
    impl->importsLog = true;
  }

  // Exports: the four functions with their exact types.
  const int32_t exports = wasm_runtime_get_export_count(impl->module);
  for (const RequiredExport& want : kExports) {
    bool found = false;
    for (int32_t i = 0; i < exports; ++i) {
      wasm_export_t exp{};
      wasm_runtime_get_export_type(impl->module, i, &exp);
      if (exp.name == nullptr || std::string_view{exp.name} != want.name) continue;
      if (exp.kind != WASM_IMPORT_EXPORT_KIND_FUNC) break;
      if (!matches(exp.u.func_type, want.sig)) {
        return fail(LoadErrorKind::BadSignature,
                    std::string{want.name} + " must have type " + want.text);
      }
      found = true;
      break;
    }
    if (!found) {
      return fail(LoadErrorKind::MissingExport,
                  std::string{"missing function export "} + want.name);
    }
  }

  // Memory: there must be one, and its initial size must fit the cap.
  uint64_t initBytes = 0;
  uint64_t maxBytes = 0;
  if (kfm_wamr_module_memory(impl->module, &initBytes, &maxBytes) == 0) {
    return fail(LoadErrorKind::NoMemory, "the module has no linear memory");
  }
  if (initBytes > config.maxMemoryBytes) {
    return fail(LoadErrorKind::MemoryTooLarge, "initial memory is " + std::to_string(initBytes) +
                                                   " bytes, the cap is " +
                                                   std::to_string(config.maxMemoryBytes));
  }

  // A probe instance: runs the start function under fuelInit and asks the
  // module for its ABI version and observation buffer, so a bad module is
  // refused here rather than in the middle of a match.
  std::string error;
  const std::unique_ptr<Instance> probe = impl->instantiate(error);
  if (probe == nullptr) return fail(LoadErrorKind::Instantiate, error);
  const AbiCheck abi = checkAbi(*probe, config.fuelPerTick);
  switch (abi.fault) {
    case AgentFault::None:
      break;
    case AgentFault::AbiMismatch:
      return fail(LoadErrorKind::AbiMismatch, abi.message);
    case AgentFault::BadObsBuffer:
      return fail(LoadErrorKind::BadObsBuffer, abi.message);
    default:
      return fail(LoadErrorKind::Instantiate, abi.message);
  }
  return WasmModule{std::move(impl)};
}

const WasmAgentConfig& WasmModule::config() const {
  return impl_->config;
}

bool WasmModule::importsLog() const {
  return impl_->importsLog;
}

// ---------------------------------------------------------------------------
// WasmAgent
// ---------------------------------------------------------------------------
struct WasmAgent::Impl {
  std::shared_ptr<WasmModule::Impl> module;
  std::unique_ptr<Instance> instance;  // null while dead
  uint32_t obsAddr{0};
  AgentFault lastFault{AgentFault::Dead};
  AgentFault deathCause{AgentFault::Dead};  // why `instance` is null
  std::string lastFaultMessage{"not initialised"};
  WasmAgentStats stats;
  std::string log;
  AgentLogSink sink;  // the exec env's user data; points into this Impl, which never moves

  void kill(AgentFault fault, std::string message) {
    instance.reset();
    lastFault = fault;
    deathCause = fault;
    lastFaultMessage = std::move(message);
  }
};

WasmAgent::WasmAgent(WasmModule module) : impl_(std::make_unique<Impl>()) {
  impl_->module = std::move(module.impl_);
  impl_->sink = AgentLogSink{.log = &impl_->log,
                             .stats = &impl_->stats,
                             .capacity = impl_->module->config.logCaptureBytes};
}

WasmAgent::~WasmAgent() = default;
WasmAgent::WasmAgent(WasmAgent&&) noexcept = default;
WasmAgent& WasmAgent::operator=(WasmAgent&&) noexcept = default;

void WasmAgent::init(uint64_t seed, PlayerId slot) {
  Impl& a = *impl_;
  a.instance.reset();
  std::string error;
  std::unique_ptr<Instance> in = a.module->instantiate(error);
  if (in == nullptr) {
    a.kill(AgentFault::Instantiate, std::move(error));
    return;
  }
  wasm_runtime_set_user_data(in->env, &a.sink);
  const WasmAgentConfig& cfg = a.module->config;
  AbiCheck abi = checkAbi(*in, cfg.fuelPerTick);
  if (abi.fault != AgentFault::None) {
    a.kill(abi.fault, std::move(abi.message));
    return;
  }
  // kfm_init(seed: i64, slot: i32): the i64 takes two cells in host order.
  std::array<uint32_t, 3> cells{};
  std::memcpy(cells.data(), &seed, sizeof seed);
  cells[2] = slot.value;
  const CallResult r =
      call(*in, in->initFn, cells, static_cast<uint32_t>(cells.size()), cfg.fuelInit);
  a.stats.fuelInit = r.fuelUsed;
  if (!r.ok) {
    a.kill(r.overBudget ? AgentFault::InitOverBudget : AgentFault::InitTrap,
           "kfm_init: " + r.error);
    return;
  }
  a.obsAddr = abi.obsAddr;
  a.instance = std::move(in);
  a.lastFault = AgentFault::None;
  a.deathCause = AgentFault::None;
  a.lastFaultMessage.clear();
}

std::optional<sim::PlayerInput> WasmAgent::act(const Observation& obs, Tick tick) {
  Impl& a = *impl_;
  ++a.stats.acts;
  if (a.instance == nullptr) {
    ++a.stats.substitutions;
    a.lastFault = AgentFault::Dead;  // lastFaultMessage still says why
    return std::nullopt;
  }
  const Instance& in = *a.instance;
  // Resolved every tick: memory.grow may have moved the linear memory. The
  // range was validated at init and memory never shrinks.
  void* dst = wasm_runtime_addr_app_to_native(in.inst, a.obsAddr);
  std::memcpy(dst, &obs, sizeof obs);

  std::array<uint32_t, 2> cells{tick.value, 0};
  CallResult r = call(in, in.actFn, cells, 1, a.module->config.fuelPerTick);
  a.stats.fuelLast = r.fuelUsed;
  a.stats.fuelMax = std::max(a.stats.fuelMax, r.fuelUsed);
  a.stats.fuelTotal += r.fuelUsed;
  if (!r.ok) {
    ++a.stats.substitutions;
    if (r.overBudget) {
      ++a.stats.overBudget;
      a.lastFault = AgentFault::OverBudget;
    } else {
      ++a.stats.traps;
      a.lastFault = AgentFault::Trap;
    }
    a.lastFaultMessage = std::move(r.error);
    return std::nullopt;
  }
  uint64_t packed = 0;
  std::memcpy(&packed, cells.data(), sizeof packed);
  const std::optional<sim::PlayerInput> input = unpackInput(packed);
  if (!input) {
    ++a.stats.substitutions;
    ++a.stats.badReturns;
    a.lastFault = AgentFault::BadReturn;
    a.lastFaultMessage = "kfm_act set bits 40..63";
    return std::nullopt;
  }
  a.lastFault = AgentFault::None;
  a.lastFaultMessage.clear();
  return input;
}

bool WasmAgent::alive() const {
  return impl_->instance != nullptr;
}

AgentFault WasmAgent::lastFault() const {
  return impl_->lastFault;
}

AgentFault WasmAgent::deathCause() const {
  return impl_->deathCause;
}

std::string_view WasmAgent::lastFaultMessage() const {
  return impl_->lastFaultMessage;
}

const WasmAgentStats& WasmAgent::stats() const {
  return impl_->stats;
}

std::string WasmAgent::takeLog() {
  std::string out;
  out.swap(impl_->log);
  return out;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
std::string_view toString(LoadErrorKind kind) {
  switch (kind) {
    case LoadErrorKind::NoRuntime:
      return "no-runtime";
    case LoadErrorKind::TooLarge:
      return "too-large";
    case LoadErrorKind::Malformed:
      return "malformed";
    case LoadErrorKind::BadImport:
      return "bad-import";
    case LoadErrorKind::MissingExport:
      return "missing-export";
    case LoadErrorKind::BadSignature:
      return "bad-signature";
    case LoadErrorKind::NoMemory:
      return "no-memory";
    case LoadErrorKind::MemoryTooLarge:
      return "memory-too-large";
    case LoadErrorKind::Instantiate:
      return "instantiate";
    case LoadErrorKind::AbiMismatch:
      return "abi-mismatch";
    case LoadErrorKind::BadObsBuffer:
      return "bad-obs-buffer";
  }
  return "unknown";
}

std::string_view toString(AgentFault fault) {
  switch (fault) {
    case AgentFault::None:
      return "none";
    case AgentFault::Trap:
      return "trap";
    case AgentFault::OverBudget:
      return "over-budget";
    case AgentFault::BadReturn:
      return "bad-return";
    case AgentFault::Dead:
      return "dead";
    case AgentFault::Instantiate:
      return "instantiate";
    case AgentFault::AbiMismatch:
      return "abi-mismatch";
    case AgentFault::BadObsBuffer:
      return "bad-obs-buffer";
    case AgentFault::InitTrap:
      return "init-trap";
    case AgentFault::InitOverBudget:
      return "init-over-budget";
  }
  return "unknown";
}

}  // namespace kfm::ai
