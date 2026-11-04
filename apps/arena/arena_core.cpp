// kfm_arena's core (see arena_core.hpp).
#include "arena_core.hpp"

#include "kfm/ai/agent_host.hpp"
#include "kfm/ai/reference_bot.hpp"
#include "kfm/core/bitstream.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/replay.hpp"
#include "kfm/sim/step.hpp"

#ifdef KFM_WASM_BOTS
#include "kfm/ai/wasm_runtime.hpp"

#include <cstring>
#include <fstream>
#include <map>
#endif

#include <algorithm>
#include <format>
#include <iterator>
#include <numeric>
#include <string_view>
#include <utility>

namespace kfm::arena {

// ---------------------------------------------------------------------------
// BotFactory
// ---------------------------------------------------------------------------

struct BotFactory::Impl {
  uint32_t fuelPerTick{kDefaultFuelPerTick};
#ifdef KFM_WASM_BOTS
  std::unique_ptr<ai::WasmRuntime> runtime;
  std::map<std::string, ai::WasmModule, std::less<>> modules;
#endif
};

bool wasmBotsAvailable() {
#ifdef KFM_WASM_BOTS
  return true;
#else
  return false;
#endif
}

BotFactory::BotFactory(uint32_t fuelPerTick) : impl_(std::make_unique<Impl>()) {
  impl_->fuelPerTick = fuelPerTick;
}

BotFactory::~BotFactory() = default;

std::expected<void, BotError> BotFactory::prepare(const ai::BotSpec& spec) {
  if (spec.kind == ai::BotKind::Reference) return {};
#ifdef KFM_WASM_BOTS
  if (impl_->modules.contains(spec.path)) return {};
  if (!impl_->runtime) {
    impl_->runtime = std::make_unique<ai::WasmRuntime>();
    if (!impl_->runtime->ok()) {
      return std::unexpected(BotError{BotErrorKind::Load, "the wasm runtime failed to initialise"});
    }
  }
  std::ifstream in(spec.path, std::ios::binary);
  if (!in) {
    return std::unexpected(BotError{BotErrorKind::Io, std::format("cannot read {}", spec.path)});
  }
  const std::vector<char> chars{std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>()};
  if (in.bad()) {
    return std::unexpected(BotError{BotErrorKind::Io, std::format("cannot read {}", spec.path)});
  }
  std::vector<std::byte> bytes(chars.size());
  if (!chars.empty()) std::memcpy(bytes.data(), chars.data(), chars.size());
  ai::WasmAgentConfig config{};
  config.fuelPerTick = impl_->fuelPerTick;
  auto module = ai::WasmModule::load(bytes, config);
  if (!module) {
    return std::unexpected(
        BotError{BotErrorKind::Load, std::format("{}: {} ({})", spec.path, module.error().message,
                                                 ai::toString(module.error().kind))});
  }
  impl_->modules.emplace(spec.path, *module);
  return {};
#else
  return std::unexpected(
      BotError{BotErrorKind::Unsupported,
               std::format("{}: this build runs no bot modules (configured with KFM_WASM_BOTS=OFF)",
                           spec.path)});
#endif
}

std::expected<BotFactory::Made, BotError> BotFactory::make(const ai::BotSpec& spec) {
  if (spec.kind == ai::BotKind::Reference) {
    return Made{std::make_unique<ai::ReferenceBot>(spec.referenceConfig()), nullptr};
  }
  if (auto ok = prepare(spec); !ok) return std::unexpected(std::move(ok.error()));
#ifdef KFM_WASM_BOTS
  const auto it = impl_->modules.find(spec.path);
  auto agent = std::make_unique<ai::WasmAgent>(it->second);
  ai::WasmAgent* raw = agent.get();
  return Made{std::move(agent), raw};
#else
  return std::unexpected(BotError{BotErrorKind::Unsupported, "no bot modules in this build"});
#endif
}

// ---------------------------------------------------------------------------
// runMatch
// ---------------------------------------------------------------------------

void assignPlaces(MatchResult& result) {
  std::vector<size_t> order(result.slots.size());
  std::ranges::iota(order, size_t{0});
  const auto isWinner = [&](const SlotResult& s) {
    return result.winner.isValid() && s.slot == result.winner.value;
  };
  std::ranges::sort(order, [&](size_t ia, size_t ib) {
    const SlotResult& a = result.slots[ia];
    const SlotResult& b = result.slots[ib];
    if (isWinner(a) != isWinner(b)) return isWinner(a);
    if (a.score != b.score) return a.score > b.score;
    if (a.deaths != b.deaths) return a.deaths < b.deaths;
    return a.slot < b.slot;
  });
  for (size_t place = 0; place < order.size(); ++place) {
    result.slots[order[place]].place = static_cast<uint8_t>(place + 1);
  }
}

std::expected<MatchResult, BotError> runMatch(const MatchSetup& setup,
                                              const sim::ContentDB& content, BotFactory& factory,
                                              uint32_t index) {
  using namespace kfm::sim;
  const size_t n = setup.bots.size();
  if (n == 0 || n > kMaxPlayers) {
    return std::unexpected(BotError{
        BotErrorKind::Load, std::format("a match needs 1..{} bots, got {}", kMaxPlayers, n)});
  }
  MatchConfig config = setup.config;
  config.players = static_cast<PlayerMask>((1u << n) - 1u);
  const MatchContent match = matchContent(content, config);

  // Slot i plays bot botOf[i]: bot b sits in slot (b + rotation) mod n.
  std::vector<size_t> botOf(n);
  const size_t shift = setup.rotation % n;
  for (size_t i = 0; i < n; ++i)
    botOf[i] = (i + n - shift) % n;

  // Agents first: a failing module leaves no half-played match.
  ai::AgentHost host;
  std::vector<ai::WasmAgent*> wasm(n, nullptr);
  for (size_t i = 0; i < n; ++i) {
    auto made = factory.make(setup.bots[botOf[i]]);
    if (!made) return std::unexpected(std::move(made.error()));
    const auto slot = static_cast<uint8_t>(i);
    wasm[i] = made->wasm;
    host.attach(PlayerId{slot}, std::move(made->agent), slotSeed(config.seed, slot),
                setup.delayTicks);
  }

  MatchResult result;
  result.index = index;
  result.seed = config.seed;
  result.slots.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const ai::BotSpec& bot = setup.bots[botOf[i]];
    result.slots[i].slot = static_cast<uint8_t>(i);
    result.slots[i].botIndex = static_cast<uint8_t>(botOf[i]);
    result.slots[i].bot = ai::toString(bot);
    result.slots[i].module = bot.kind == ai::BotKind::Module;
  }

  // A WorldState is large: keep it off the stack.
  const auto world = std::make_unique<WorldState>(initialWorld(config));
  EventSink sink;
  std::vector<InputFrame> frames;
  if (setup.recordReplay) frames.reserve(std::min<uint32_t>(setup.tickCap, 1u << 16));

  for (uint32_t t = 0; t < setup.tickCap; ++t) {
    InputFrame frame;
    frame.tick = world->tick;
    frame.present = world->rules.present;
    host.fill(frame, *world, match);
    if (setup.recordReplay) frames.push_back(frame);
    sink.clear();
    step(*world, frame, match, sink);
    ++result.ticks;
    bool ended = false;
    for (const SimEvent& e : sink) {
      if (e.kind == EventKind::PlayerEliminated) {
        if (e.player.isValid() && e.player.value < n) ++result.slots[e.player.value].deaths;
        if (e.a < n && e.a != e.player.value) ++result.slots[e.a].kills;
      } else if (e.kind == EventKind::MatchEnded) {
        ended = true;
        result.outcome = MatchOutcome::Ended;
        result.winner = e.player.isValid() && e.player.value < n ? e.player : PlayerId::invalid();
      }
    }
    if (ended) break;
  }

  for (size_t i = 0; i < n; ++i) {
    SlotResult& s = result.slots[i];
    s.score = world->rules.scores[i];
    const ai::AgentSlotStats stats = host.stats(PlayerId{static_cast<uint8_t>(i)});
    s.acts = stats.acts;
    s.substitutions = stats.substitutions;
#ifdef KFM_WASM_BOTS
    if (wasm[i] != nullptr) {
      const ai::WasmAgentStats& w = wasm[i]->stats();
      s.traps = w.traps;
      s.overBudget = w.overBudget;
      s.badReturns = w.badReturns;
      s.fuelMean = w.acts == 0 ? 0 : w.fuelTotal / w.acts;
      s.fuelMax = w.fuelMax;
      s.fuelInit = w.fuelInit;
      s.logBytes = w.logBytes;
      if (!wasm[i]->alive()) s.dead = std::string(ai::toString(wasm[i]->deathCause()));
      s.log = wasm[i]->takeLog();
    }
#endif
  }
  result.checksum = checksum(*world);
  assignPlaces(result);

  if (setup.recordReplay) {
    ReplayHeader h;
    h.seed = config.seed;
    h.contentHash = contentHash(content);
    h.buildHash = 0;
    h.players = config.players;
    h.arena = config.arena;
    h.mode = config.mode;
    h.powerUps = config.powerUps;
    h.loadout = config.loadout;
    h.tickCount = result.ticks;
    result.replay.resize(replayMaxBytes(h));
    BitWriter w(result.replay);
    writeReplayHeader(w, h);
    for (const InputFrame& f : frames)
      writeReplayFrame(w, f);
    result.replay.resize(w.ok() ? w.bytesWritten() : 0);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------

void Summary::add(const MatchResult& result) {
  ++matches;
  ticks += result.ticks;
  if (result.outcome == MatchOutcome::Cap) {
    ++capped;
  } else if (result.winner.isValid()) {
    ++ended;
  } else {
    ++draws;
  }
  const bool won = result.winner.isValid();
  const auto fold = [&](std::vector<SlotSummary>& rows, uint8_t index, const SlotResult& s) {
    if (rows.size() <= index) rows.resize(size_t{index} + 1);
    SlotSummary& t = rows[index];
    const bool first = t.bot.empty();
    t.index = index;
    if (first) {
      t.bot = s.bot;
    } else if (t.bot != s.bot) {
      t.bot = "mixed";
    }
    t.module = t.module || s.module;
    if (won && result.winner.value == s.slot) ++t.wins;
    t.placeSum += s.place;
    t.score += s.score;
    t.kills += s.kills;
    t.deaths += s.deaths;
    t.acts += s.acts;
    t.substitutions += s.substitutions;
    t.traps += s.traps;
    t.overBudget += s.overBudget;
    t.badReturns += s.badReturns;
    t.fuelMax = std::max(t.fuelMax, s.fuelMax);
  };
  for (const SlotResult& s : result.slots) {
    fold(slots, s.slot, s);
    fold(bots, s.botIndex, s);
    if (s.botIndex != s.slot) rotated = true;
  }
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

namespace {

std::string_view outcomeText(MatchOutcome o) {
  return o == MatchOutcome::Ended ? "ended" : "cap";
}

std::string winnerText(PlayerId w) {
  return w.isValid() ? std::to_string(w.value) : std::string("none");
}

// A JSON string literal: quotes, backslashes and control characters escaped.
std::string jsonString(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          std::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned>(c));
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
  return out;
}

double meanPlace(const SlotSummary& s, uint32_t matches) {
  return matches == 0 ? 0.0 : static_cast<double>(s.placeSum) / static_cast<double>(matches);
}

}  // namespace

std::string formatResultLine(const MatchResult& r) {
  std::string out =
      std::format("match {} seed {} ticks {} checksum {:08x} result {} winner {}", r.index, r.seed,
                  r.ticks, r.checksum, outcomeText(r.outcome), winnerText(r.winner));
  for (const SlotResult& s : r.slots) {
    std::format_to(std::back_inserter(out),
                   " | slot {} bot {} {} place {} score {} kills {} deaths {} acts {} subs {}",
                   s.slot, s.botIndex, s.bot, s.place, s.score, s.kills, s.deaths, s.acts,
                   s.substitutions);
    if (s.module) {
      std::format_to(
          std::back_inserter(out),
          " traps {} over {} bad {} fuel_mean {} fuel_max {} fuel_init {} log {} dead {}", s.traps,
          s.overBudget, s.badReturns, s.fuelMean, s.fuelMax, s.fuelInit, s.logBytes,
          s.dead.empty() ? std::string_view("no") : std::string_view(s.dead));
    }
  }
  return out;
}

std::string formatResultJson(const MatchResult& r) {
  std::string out = std::format(
      R"({{"match":{},"seed":{},"ticks":{},"checksum":"{:08x}","result":"{}","winner":{})", r.index,
      r.seed, r.ticks, r.checksum, outcomeText(r.outcome),
      r.winner.isValid() ? std::to_string(r.winner.value) : "null");
  out += R"(,"slots":[)";
  for (size_t i = 0; i < r.slots.size(); ++i) {
    const SlotResult& s = r.slots[i];
    if (i != 0) out.push_back(',');
    std::format_to(std::back_inserter(out),
                   R"({{"slot":{},"bot":{},"spec":{},"module":{},"place":{},"score":{},)"
                   R"("kills":{},"deaths":{},"acts":{},"subs":{})",
                   s.slot, s.botIndex, jsonString(s.bot), s.module, s.place, s.score, s.kills,
                   s.deaths, s.acts, s.substitutions);
    if (s.module) {
      std::format_to(std::back_inserter(out),
                     R"(,"traps":{},"over":{},"bad":{},"fuel_mean":{},"fuel_max":{},)"
                     R"("fuel_init":{},"log":{},"dead":{})",
                     s.traps, s.overBudget, s.badReturns, s.fuelMean, s.fuelMax, s.fuelInit,
                     s.logBytes, s.dead.empty() ? std::string("null") : jsonString(s.dead));
    }
    out.push_back('}');
  }
  out += "]}";
  return out;
}

std::string formatSummaryLine(const Summary& sum) {
  const uint64_t ticksMean = sum.matches == 0 ? 0 : sum.ticks / sum.matches;
  std::string out = std::format("summary matches {} ended {} draws {} cap {} ticks_mean {}",
                                sum.matches, sum.ended, sum.draws, sum.capped, ticksMean);
  const auto rows = [&](std::string_view label, const std::vector<SlotSummary>& totals) {
    for (const SlotSummary& s : totals) {
      std::format_to(std::back_inserter(out),
                     " | {} {} {} wins {} mean_place {:.2f} score {} kills {} deaths {} acts {} "
                     "subs {}",
                     label, s.index, s.bot, s.wins, meanPlace(s, sum.matches), s.score, s.kills,
                     s.deaths, s.acts, s.substitutions);
      if (s.module) {
        std::format_to(std::back_inserter(out), " traps {} over {} bad {} fuel_max {}", s.traps,
                       s.overBudget, s.badReturns, s.fuelMax);
      }
    }
  };
  rows("slot", sum.slots);
  if (sum.rotated) rows("bot", sum.bots);
  return out;
}

std::string formatSummaryJson(const Summary& sum) {
  const uint64_t ticksMean = sum.matches == 0 ? 0 : sum.ticks / sum.matches;
  std::string out =
      std::format(R"({{"summary":true,"matches":{},"ended":{},"draws":{},"cap":{},"ticks_mean":{})",
                  sum.matches, sum.ended, sum.draws, sum.capped, ticksMean);
  const auto rows = [&](std::string_view key, std::string_view label,
                        const std::vector<SlotSummary>& totals) {
    std::format_to(std::back_inserter(out), R"(,"{}":[)", key);
    for (size_t i = 0; i < totals.size(); ++i) {
      const SlotSummary& s = totals[i];
      if (i != 0) out.push_back(',');
      std::format_to(std::back_inserter(out),
                     R"({{"{}":{},"spec":{},"module":{},"wins":{},"mean_place":{:.2f},"score":{},)"
                     R"("kills":{},"deaths":{},"acts":{},"subs":{})",
                     label, s.index, jsonString(s.bot), s.module, s.wins, meanPlace(s, sum.matches),
                     s.score, s.kills, s.deaths, s.acts, s.substitutions);
      if (s.module) {
        std::format_to(std::back_inserter(out), R"(,"traps":{},"over":{},"bad":{},"fuel_max":{})",
                       s.traps, s.overBudget, s.badReturns, s.fuelMax);
      }
      out.push_back('}');
    }
    out.push_back(']');
  };
  rows("slots", "slot", sum.slots);
  if (sum.rotated) rows("bots", "bot", sum.bots);
  out.push_back('}');
  return out;
}

}  // namespace kfm::arena
