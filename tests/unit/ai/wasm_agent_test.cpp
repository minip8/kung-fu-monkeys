// WasmModule / WasmAgent over the hand-written fixtures in tests/fixtures/bots
// (architecture §8): load-time validation, the act round trip and packing,
// fuel metering, faults, isolation between instances, kfm_log capture and
// determinism. The throughput measurement at the end prints, never asserts.
#include "kfm/ai/wasm_runtime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

using kfm::PlayerId;
using kfm::Tick;
using kfm::ai::AgentFault;
using kfm::ai::LoadErrorKind;
using kfm::ai::Observation;
using kfm::ai::WasmAgent;
using kfm::ai::WasmAgentConfig;
using kfm::ai::WasmModule;
using kfm::ai::WasmRuntime;
using kfm::sim::PlayerInput;

// The fixtures put the observation buffer in their second 64 KiB page.
static_assert(sizeof(Observation) <= size_t{64} * 1024,
              "the bot fixtures' observation buffer is 64 KiB: grow it in tests/fixtures/bots");

std::vector<std::byte> readFixture(const std::string& name) {
  const std::string path = std::string{KFM_BOT_FIXTURES_DIR} + "/" + name + ".wasm";
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::vector<char> chars{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  std::vector<std::byte> bytes(chars.size());
  std::memcpy(bytes.data(), chars.data(), chars.size());
  return bytes;
}

WasmModule loadFixture(const std::string& name, const WasmAgentConfig& config = {}) {
  auto module = WasmModule::load(readFixture(name), config);
  if (!module) FAIL(name << ": " << module.error().message);
  return *module;
}

WasmAgent makeAgent(const std::string& name, const WasmAgentConfig& config = {}, uint64_t seed = 7,
                    PlayerId slot = PlayerId{3}) {
  WasmAgent agent{loadFixture(name, config)};
  agent.init(seed, slot);
  REQUIRE(agent.alive());
  return agent;
}

// act() that must produce an input (REQUIREd); the neutral input otherwise.
PlayerInput mustAct(WasmAgent& agent, const Observation& obs, Tick tick) {
  const std::optional<PlayerInput> in = agent.act(obs, tick);
  REQUIRE(in.has_value());
  return in.value_or(PlayerInput{});
}

Observation patternObservation(uint32_t firstWord) {
  Observation obs{};
  std::memcpy(static_cast<void*>(&obs), &firstWord, sizeof firstWord);
  return obs;
}

}  // namespace

TEST_CASE("PlayerInput packs into the low 40 bits little-endian", "[ai][wasm]") {
  constexpr PlayerInput kIn{.moveDir = 1, .moveMag = 2, .aimDir = 3, .buttons = 4, .subTick = 5};
  STATIC_REQUIRE(kfm::ai::packInput(kIn) == 0x0504030201ull);
  STATIC_REQUIRE(kfm::ai::unpackInput(0x0504030201ull) == kIn);
  STATIC_REQUIRE(!kfm::ai::unpackInput(1ull << 40u).has_value());
  STATIC_REQUIRE(!kfm::ai::unpackInput(1ull << 63u).has_value());
  STATIC_REQUIRE(kfm::ai::unpackInput(0xFFFFFFFFFFull).has_value());
}

TEST_CASE("WasmModule needs a live WasmRuntime", "[ai][wasm]") {
  if (WasmRuntime::active()) SKIP("another test holds a runtime");
  const auto bytes = readFixture("idle");
  auto module = WasmModule::load(bytes);
  REQUIRE_FALSE(module.has_value());
  CHECK(module.error().kind == LoadErrorKind::NoRuntime);
  const WasmRuntime runtime;
  REQUIRE(runtime.ok());
  CHECK(WasmRuntime::active());
  CHECK(WasmModule::load(bytes).has_value());
}

TEST_CASE("WasmModule loads the valid fixtures", "[ai][wasm]") {
  const WasmRuntime runtime;
  for (const char* name :
       {"idle",      "echo",       "reads_obs",  "counter",    "work",       "trap",    "spin",
        "logger",    "grow",       "bad_return", "init_trap",  "init_spin",  "leaky",   "bulk_fill",
        "bulk_copy", "table_fill", "table_copy", "table_init", "table_grow", "mem_grow"}) {
    INFO(name);
    CHECK(WasmModule::load(readFixture(name)).has_value());
  }
  CHECK(loadFixture("logger").importsLog());
  CHECK_FALSE(loadFixture("idle").importsLog());
}

TEST_CASE("WasmModule refuses bad modules with the right error", "[ai][wasm]") {
  const WasmRuntime runtime;
  struct Case {
    const char* fixture;
    LoadErrorKind kind;
  };
  for (const Case& c :
       {Case{"abi_mismatch", LoadErrorKind::AbiMismatch},
        Case{"big_memory", LoadErrorKind::MemoryTooLarge},
        Case{"bad_import", LoadErrorKind::BadImport},
        Case{"bad_signature", LoadErrorKind::BadSignature},
        Case{"no_act", LoadErrorKind::MissingExport}, Case{"no_memory", LoadErrorKind::NoMemory},
        Case{"bad_obs_buffer", LoadErrorKind::BadObsBuffer},
        Case{"big_table", LoadErrorKind::TableTooLarge},
        Case{"start_spin", LoadErrorKind::Instantiate}}) {
    INFO(c.fixture);
    auto module = WasmModule::load(readFixture(c.fixture));
    REQUIRE_FALSE(module.has_value());
    CHECK(module.error().kind == c.kind);
    CHECK_FALSE(module.error().message.empty());
    INFO(module.error().message);
    CHECK(kfm::ai::toString(module.error().kind) != "unknown");
  }

  SECTION("the start function is metered") {
    auto module = WasmModule::load(readFixture("start_spin"));
    REQUIRE_FALSE(module.has_value());
    CHECK(module.error().message.find("instruction limit exceeded") != std::string::npos);
  }
  SECTION("not wasm") {
    const std::vector<std::byte> junk(64, std::byte{0x42});
    auto module = WasmModule::load(junk);
    REQUIRE_FALSE(module.has_value());
    CHECK(module.error().kind == LoadErrorKind::Malformed);
    auto empty = WasmModule::load({});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().kind == LoadErrorKind::Malformed);
  }
  SECTION("size cap") {
    WasmAgentConfig config;
    config.maxModuleBytes = 16;
    auto module = WasmModule::load(readFixture("idle"), config);
    REQUIRE_FALSE(module.has_value());
    CHECK(module.error().kind == LoadErrorKind::TooLarge);
  }
  SECTION("memory cap is configurable") {
    WasmAgentConfig config;
    config.maxMemoryBytes = 64u * 1024u;  // the fixtures declare two pages
    auto module = WasmModule::load(readFixture("idle"), config);
    REQUIRE_FALSE(module.has_value());
    CHECK(module.error().kind == LoadErrorKind::MemoryTooLarge);
  }
}

TEST_CASE("WasmAgent act round trip", "[ai][wasm]") {
  const WasmRuntime runtime;

  SECTION("idle returns the neutral input") {
    WasmAgent agent = makeAgent("idle");
    const PlayerInput in = mustAct(agent, Observation{}, Tick{0});
    CHECK(in == PlayerInput{});
    CHECK(agent.lastFault() == AgentFault::None);
  }
  SECTION("echo unpacks every byte and reads the tick") {
    WasmAgent agent = makeAgent("echo");
    const PlayerInput even = mustAct(agent, Observation{}, Tick{42});
    CHECK(even == PlayerInput{.moveDir = 64,
                              .moveMag = 255,
                              .aimDir = 42,
                              .buttons = kfm::sim::buttonBit(kfm::sim::Button::Throw),
                              .subTick = 128});
    const PlayerInput odd = mustAct(agent, Observation{}, Tick{301});
    CHECK(odd ==
          PlayerInput{
              .moveDir = 64, .moveMag = 255, .aimDir = 301 & 255, .buttons = 0, .subTick = 0});
    CHECK(agent.stats().acts == 2);
    CHECK(agent.stats().substitutions == 0);
  }
  SECTION("the observation is written before every act") {
    WasmAgent agent = makeAgent("reads_obs");
    for (const uint32_t word : {0x11223344u, 0xA1B2C3D4u, 0u}) {
      const PlayerInput in = mustAct(agent, patternObservation(word), Tick{1});
      CHECK(kfm::ai::packInput(in) == word);
    }
  }
  SECTION("kfm_init receives the seed and the slot") {
    WasmAgent agent = makeAgent("counter", {}, 0x1234'5678'9ABC'DEF0ull, PlayerId{5});
    const PlayerInput in = mustAct(agent, Observation{}, Tick{0});
    CHECK(in.moveDir == 1);
    CHECK(in.moveMag == 5);
    CHECK(in.aimDir == 0xF0);
  }
}

TEST_CASE("WasmAgent meters fuel per tick", "[ai][wasm]") {
  const WasmRuntime runtime;

  SECTION("a cheap act reports a small fuel use") {
    WasmAgent agent = makeAgent("echo");
    REQUIRE(agent.act(Observation{}, Tick{0}).has_value());
    const uint32_t fuel = agent.stats().fuelLast;
    CHECK(fuel > 0);
    CHECK(fuel < 100);
    CHECK(agent.stats().fuelTotal == fuel);
    CHECK(agent.stats().fuelMax == fuel);
    CHECK(agent.stats().fuelInit > 0);
    WARN("echo: " << fuel << " instructions per act, kfm_init " << agent.stats().fuelInit);
  }
  SECTION("fuel is exact and linear in the work done") {
    WasmAgentConfig config;
    config.fuelPerTick = 1'000'000;
    WasmAgent agent = makeAgent("work", config);
    std::vector<uint32_t> fuel;
    for (const uint32_t n : {0u, 10u, 20u, 1000u}) {
      REQUIRE(agent.act(Observation{}, Tick{n}).has_value());
      fuel.push_back(agent.stats().fuelLast);
    }
    const uint32_t perIteration = fuel[1] - fuel[0];
    CHECK(perIteration > 0);
    CHECK(fuel[2] - fuel[1] == perIteration);
    CHECK(fuel[3] - fuel[0] == 100 * perIteration);
    CHECK(agent.stats().fuelMax == fuel[3]);
  }
  SECTION("an endless loop is over budget, uses exactly the budget, and the next tick recovers") {
    WasmAgentConfig config;
    config.fuelPerTick = 5'000;
    WasmAgent agent = makeAgent("spin", config);
    CHECK_FALSE(agent.act(Observation{}, Tick{0}).has_value());
    CHECK(agent.lastFault() == AgentFault::OverBudget);
    CHECK(agent.stats().fuelLast == config.fuelPerTick);
    CHECK(agent.stats().overBudget == 1);
    CHECK(agent.stats().substitutions == 1);
    CHECK(agent.alive());

    const PlayerInput next = mustAct(agent, Observation{}, Tick{1});  // odd ticks return at once
    CHECK(next.moveDir == 1);
    CHECK(agent.lastFault() == AgentFault::None);
    const uint32_t recovered = agent.stats().fuelLast;
    CHECK(recovered < 100);

    CHECK_FALSE(agent.act(Observation{}, Tick{2}).has_value());
    CHECK(agent.stats().overBudget == 2);
    CHECK(agent.stats().fuelTotal == 2ull * config.fuelPerTick + recovered);
    CHECK(agent.stats().fuelMax == config.fuelPerTick);
  }
}

TEST_CASE("WasmAgent charges bulk and table ops for their length", "[ai][wasm]") {
  // The fuel of an op whose host work grows with an operand must grow with it, so a
  // metered budget bounds host work, not only the opcode count (docs/bot-abi.md §1.4).
  const WasmRuntime runtime;

  // One act() per n; the fuel of that act. The fixture does base work plus one bulk/table
  // op of length n, so fuel(n) - fuel(0) is the op's charge for n.
  const auto fuelCurve = [](const char* fixture, const std::vector<uint32_t>& ns,
                            const WasmAgentConfig& config) {
    WasmAgent agent = makeAgent(fixture, config);
    std::vector<uint32_t> fuel;
    for (const uint32_t n : ns) {
      REQUIRE(agent.act(Observation{}, Tick{n}).has_value());
      fuel.push_back(agent.stats().fuelLast);
    }
    return fuel;
  };

  WasmAgentConfig wide;
  wide.fuelPerTick = 1'000'000;

  SECTION("memory.copy costs one unit per eight bytes, and a small copy still works") {
    const std::vector<uint32_t> fuel = fuelCurve("bulk_copy", {0, 800, 1600, 8000}, wide);
    CHECK(fuel[1] - fuel[0] == 100);   // 800 bytes / 8
    CHECK(fuel[2] - fuel[1] == 100);   // 1600 - 800 = 800 bytes / 8
    CHECK(fuel[3] - fuel[0] == 1000);  // 8000 bytes / 8
    CHECK(fuel[0] < 100);              // the base work alone is a handful of opcodes
  }
  SECTION("table.fill costs one unit per element") {
    const std::vector<uint32_t> fuel = fuelCurve("table_fill", {0, 100, 200, 1000}, wide);
    CHECK(fuel[1] - fuel[0] == 100);
    CHECK(fuel[2] - fuel[1] == 100);
    CHECK(fuel[3] - fuel[0] == 1000);
  }
  SECTION("table.copy costs one unit per element") {
    const std::vector<uint32_t> fuel = fuelCurve("table_copy", {0, 100, 200, 1000}, wide);
    CHECK(fuel[1] - fuel[0] == 100);
    CHECK(fuel[2] - fuel[1] == 100);
    CHECK(fuel[3] - fuel[0] == 1000);
  }
  SECTION("table.init costs one unit per element") {
    const std::vector<uint32_t> fuel = fuelCurve("table_init", {0, 2, 4, 8}, wide);
    CHECK(fuel[1] - fuel[0] == 2);
    CHECK(fuel[2] - fuel[1] == 2);
    CHECK(fuel[3] - fuel[0] == 8);
  }
  SECTION("table.grow charges for the elements it adds") {
    // Cumulative on one instance, so compare fresh instances (init wipes the table).
    WasmAgent zero = makeAgent("table_grow", wide);
    WasmAgent many = makeAgent("table_grow", wide);
    REQUIRE(zero.act(Observation{}, Tick{0}).has_value());
    REQUIRE(many.act(Observation{}, Tick{10'000}).has_value());
    CHECK(many.stats().fuelLast - zero.stats().fuelLast == 10'000);
  }
  SECTION("table.grow past the maximum does no work and is not charged") {
    WasmAgent agent = makeAgent("table_grow", wide);
    // A grow that cannot fit the table's maximum returns -1 and fills nothing.
    const PlayerInput in = mustAct(agent, Observation{}, Tick{100'000});
    CHECK(kfm::ai::packInput(in) == 0xFFFFFFFFu);
    CHECK(agent.stats().fuelLast < 100);
  }
  SECTION("memory.grow charges for the pages it adds") {
    WasmAgent one = makeAgent("mem_grow", wide);
    WasmAgent ten = makeAgent("mem_grow", wide);
    REQUIRE(one.act(Observation{}, Tick{1}).has_value());
    REQUIRE(ten.act(Observation{}, Tick{10}).has_value());
    CHECK(ten.stats().fuelLast - one.stats().fuelLast == 9u * 8192u);
  }
  SECTION("memory.grow past the cap does no work and is not charged") {
    WasmAgent agent = makeAgent("mem_grow", wide);
    const PlayerInput in = mustAct(agent, Observation{}, Tick{1000});  // 1000 > 254 free pages
    CHECK(kfm::ai::packInput(in) == 0xFFFFFFFFu);
    CHECK(agent.stats().fuelLast < 100);
  }
}

TEST_CASE("WasmAgent bounds a bulk-op flood with the budget", "[ai][wasm]") {
  // A module that loops maximal fills/copies goes over budget after bounded work, rather
  // than driving hundreds of GiB of memset under a handful of opcodes.
  const WasmRuntime runtime;

  SECTION("a 16 MiB memory.fill loop is over budget after no work (per tick)") {
    // Each fill would charge 16 MiB / 8 = 2 Mi units, more than a whole tick's budget, so
    // the first fill never runs: the budget is spent, the agent survives, and the test
    // returns at once instead of memset-ing for seconds.
    WasmAgent agent = makeAgent("bulk_fill");  // default fuelPerTick = 100k
    CHECK_FALSE(agent.act(Observation{}, Tick{0}).has_value());
    CHECK(agent.lastFault() == AgentFault::OverBudget);
    CHECK(agent.stats().fuelLast == agent.stats().fuelMax);
    CHECK(agent.stats().fuelLast == 100'000u);  // exactly the budget, like any over-budget tick
    CHECK(agent.stats().overBudget == 1);
    CHECK(agent.alive());
  }
  SECTION("a full-table table.fill loop is over budget after bounded work") {
    WasmAgentConfig config;
    config.fuelPerTick = 50'000;  // a 65536-element fill charges 65536, more than this
    WasmAgent agent = makeAgent("table_fill", config);
    CHECK_FALSE(agent.act(Observation{}, Tick{65'536}).has_value());
    CHECK(agent.lastFault() == AgentFault::OverBudget);
    CHECK(agent.stats().fuelLast == config.fuelPerTick);
  }
  SECTION("a 16 MiB memory.fill loop in kfm_init is bounded by the init budget") {
    WasmAgentConfig config;
    config.fuelInit = 5'000'000;  // a few 2 Mi-unit fills, then over budget: the agent dies
    WasmAgent agent{loadFixture("init_bulk", config)};
    agent.init(1, PlayerId{0});
    CHECK_FALSE(agent.alive());
    CHECK(agent.deathCause() == AgentFault::InitOverBudget);
    CHECK(agent.stats().fuelInit == config.fuelInit);
  }
}

TEST_CASE("WasmAgent faults cost the tick, not the agent", "[ai][wasm]") {
  const WasmRuntime runtime;

  SECTION("trap") {
    WasmAgent agent = makeAgent("trap");
    for (uint32_t t = 0; t < 3; ++t)
      CHECK_FALSE(agent.act(Observation{}, Tick{t}).has_value());
    CHECK(agent.lastFault() == AgentFault::Trap);
    CHECK(agent.lastFaultMessage().find("unreachable") != std::string_view::npos);
    CHECK(agent.stats().traps == 3);
    CHECK(agent.stats().substitutions == 3);
    CHECK(agent.stats().acts == 3);
    CHECK(agent.alive());
  }
  SECTION("bits above the five input bytes") {
    WasmAgent agent = makeAgent("bad_return");
    CHECK_FALSE(agent.act(Observation{}, Tick{0}).has_value());
    CHECK(agent.lastFault() == AgentFault::BadReturn);
    CHECK(agent.stats().badReturns == 1);
    CHECK(agent.stats().substitutions == 1);
  }
}

TEST_CASE("WasmAgent restores an exported __stack_pointer after a fault", "[ai][wasm]") {
  const WasmRuntime runtime;
  WasmAgent agent = makeAgent("leaky");
  constexpr uint64_t kStackTop = 32768;
  CHECK(kfm::ai::packInput(mustAct(agent, Observation{}, Tick{0})) == kStackTop);
  for (uint32_t t = 1; t < 8; t += 2) {
    CHECK_FALSE(agent.act(Observation{}, Tick{t}).has_value());  // traps with a frame pushed
    CHECK(kfm::ai::packInput(mustAct(agent, Observation{}, Tick{t + 1})) == kStackTop);
  }
  CHECK(agent.stats().traps == 4);
}

TEST_CASE("WasmAgent init faults leave the agent dead", "[ai][wasm]") {
  const WasmRuntime runtime;

  SECTION("act before init") {
    WasmAgent agent{loadFixture("idle")};
    CHECK_FALSE(agent.alive());
    CHECK_FALSE(agent.act(Observation{}, Tick{0}).has_value());
    CHECK(agent.lastFault() == AgentFault::Dead);
    CHECK(agent.deathCause() == AgentFault::Dead);
    CHECK(agent.stats().substitutions == 1);
  }
  SECTION("kfm_init traps") {
    WasmAgent agent{loadFixture("init_trap")};
    agent.init(1, PlayerId{0});
    CHECK_FALSE(agent.alive());
    CHECK(agent.lastFault() == AgentFault::InitTrap);
    CHECK(agent.deathCause() == AgentFault::InitTrap);
    for (uint32_t t = 0; t < 4; ++t)
      CHECK_FALSE(agent.act(Observation{}, Tick{t}).has_value());
    CHECK(agent.lastFault() == AgentFault::Dead);
    CHECK(agent.deathCause() == AgentFault::InitTrap);
    CHECK(agent.stats().acts == 4);
    CHECK(agent.stats().substitutions == 4);
    CHECK(agent.stats().fuelTotal == 0);  // the module was never called
  }
  SECTION("kfm_init runs over its budget") {
    WasmAgentConfig config;
    config.fuelInit = 20'000;
    WasmAgent agent{loadFixture("init_spin", config)};
    agent.init(1, PlayerId{0});
    CHECK_FALSE(agent.alive());
    CHECK(agent.deathCause() == AgentFault::InitOverBudget);
    CHECK(agent.stats().fuelInit == config.fuelInit);
  }
}

TEST_CASE("WasmAgent instances never share memory", "[ai][wasm]") {
  const WasmRuntime runtime;
  const WasmModule module = loadFixture("counter");
  WasmAgent a{module};
  WasmAgent b{module};
  a.init(1, PlayerId{0});
  b.init(2, PlayerId{1});
  REQUIRE(a.alive());
  REQUIRE(b.alive());
  for (uint32_t t = 0; t < 3; ++t)
    REQUIRE(a.act(Observation{}, Tick{t}).has_value());
  const PlayerInput bFirst = mustAct(b, Observation{}, Tick{0});
  CHECK(bFirst.moveDir == 1);
  CHECK(bFirst.moveMag == 1);
  const PlayerInput aFourth = mustAct(a, Observation{}, Tick{3});
  CHECK(aFourth.moveDir == 4);
  CHECK(aFourth.moveMag == 0);

  SECTION("a second init is a wipe") {
    a.init(1, PlayerId{0});
    const PlayerInput again = mustAct(a, Observation{}, Tick{0});
    CHECK(again.moveDir == 1);
  }
}

TEST_CASE("WasmAgent captures kfm_log, bounded", "[ai][wasm]") {
  const WasmRuntime runtime;
  const std::string line = "hello from wasm\n";

  SECTION("default capture") {
    WasmAgent agent = makeAgent("logger");
    for (uint32_t t = 0; t < 3; ++t)
      REQUIRE(agent.act(Observation{}, Tick{t}).has_value());
    std::string three = line;
    three += line;
    three += line;
    CHECK(agent.takeLog() == three);
    CHECK(agent.takeLog().empty());
    CHECK(agent.stats().logBytes == 3 * line.size());
    CHECK(agent.stats().logBytesDropped == 0);
  }
  SECTION("the most recent bytes are kept and the rest counted") {
    WasmAgentConfig config;
    config.logCaptureBytes = 40;
    WasmAgent agent = makeAgent("logger", config);
    for (uint32_t t = 0; t < 10; ++t)
      REQUIRE(agent.act(Observation{}, Tick{t}).has_value());
    const std::string log = agent.takeLog();
    CHECK(log.size() == 40);
    const std::string all = [&] {
      std::string s;
      for (int i = 0; i < 10; ++i)
        s += line;
      return s;
    }();
    CHECK(log == all.substr(all.size() - 40));
    CHECK(agent.stats().logBytes == 160);
    CHECK(agent.stats().logBytesDropped == 120);
  }
  SECTION("capture off") {
    WasmAgentConfig config;
    config.logCaptureBytes = 0;
    WasmAgent agent = makeAgent("logger", config);
    REQUIRE(agent.act(Observation{}, Tick{0}).has_value());
    CHECK(agent.takeLog().empty());
    CHECK(agent.stats().logBytesDropped == line.size());
  }
  SECTION("an out-of-bounds log pointer traps") {
    WasmAgent agent = makeAgent("logger");
    CHECK_FALSE(agent.act(Observation{}, Tick{999}).has_value());
    CHECK(agent.lastFault() == AgentFault::Trap);
    CHECK(agent.stats().traps == 1);
    REQUIRE(agent.act(Observation{}, Tick{1000}).has_value());
  }
}

TEST_CASE("WasmAgent caps memory growth and follows a moved buffer", "[ai][wasm]") {
  const WasmRuntime runtime;
  WasmAgentConfig config;
  config.maxMemoryBytes = 4u * 64u * 1024u;  // the fixture starts at two pages
  WasmAgent agent = makeAgent("grow", config);
  std::vector<uint32_t> results;
  for (uint32_t t = 0; t < 4; ++t) {
    const PlayerInput in = mustAct(agent, patternObservation(0x50u + t), Tick{t});
    const uint64_t packed = kfm::ai::packInput(in);
    results.push_back(static_cast<uint32_t>(packed));
    CHECK(in.subTick == 0x50u + t);  // the observation, read after the growth
  }
  CHECK(results == std::vector<uint32_t>{2, 3, 0xFFFFFFFFu, 0xFFFFFFFFu});
}

TEST_CASE("WasmAgent is deterministic", "[ai][wasm]") {
  const WasmRuntime runtime;
  struct Trace {
    std::vector<uint64_t> outputs;
    std::vector<uint32_t> fuel;
    bool operator==(const Trace&) const = default;
  };
  const auto run = [](const char* name) {
    WasmAgentConfig config;
    config.fuelPerTick = 200'000;
    WasmAgent agent = makeAgent(name, config, 99, PlayerId{2});
    Trace trace;
    for (uint32_t t = 0; t < 64; ++t) {
      const auto in = agent.act(patternObservation(t * 2654435761u), Tick{t * 37});
      trace.outputs.push_back(in ? kfm::ai::packInput(in.value_or(PlayerInput{})) : ~0ull);
      trace.fuel.push_back(agent.stats().fuelLast);
    }
    return trace;
  };
  for (const char* name : {"echo", "counter", "work", "reads_obs", "spin"}) {
    INFO(name);
    CHECK(run(name) == run(name));
  }
}

TEST_CASE("Classic interpreter throughput", "[ai][wasm][bench]") {
  const WasmRuntime runtime;
  WasmAgentConfig config;
  config.fuelPerTick = 1'000'000'000;
  WasmAgent agent = makeAgent("work", config);
  constexpr uint32_t kIterations = 4'000'000;
  REQUIRE(agent.act(Observation{}, Tick{1}).has_value());  // warm up
  const auto start = std::chrono::steady_clock::now();
  REQUIRE(agent.act(Observation{}, Tick{kIterations}).has_value());
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const double seconds = std::chrono::duration<double>(elapsed).count();
  const double fuel = agent.stats().fuelLast;
  WARN("classic interpreter: " << fuel / 1e6 << " M instructions in " << seconds * 1e3
                               << " ms = " << fuel / seconds / 1e6
                               << " M instructions/s; 100k fuel = " << 1e5 / (fuel / seconds) * 1e6
                               << " us");
}
