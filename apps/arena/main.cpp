// kfm_arena: the headless match runner (architecture §8, §9). Bot modules or
// built-in bots by name, seed, arena, mode and power-up set in; each match
// played at full speed; a .kfmr replay (playable by kfm_replay and the
// client) and a result line per match plus a summary line out.
//
//   kfm_arena [--bot SPEC]... [--bots N] | [--team SPEC,SPEC,...]...
//             [--seed S] [--matches M] [--rotate]
//             [--ticks N] [--arena NAME] [--mode NAME] [--loadout NAME]
//             [--powerups LIST] [--memory-on-respawn keep|wipe]
//             [--content DIR] [--fuel N] [--delay T]
//             [--replay out.kfmr | --replay-dir DIR] [--result out.txt]
//             [--json] [--quiet] [--log]
//
//   --bot SPEC     repeatable, fills slots in order from slot 0: ref,
//                  ref:easy|medium|hard or a path to a .wasm module (BotSpec)
//   --bots N       pads to N slots with ref:hard (default: the --bot count,
//                  at least 2)
//   --team LIST    v0.35, repeatable, one per team of a team mode (exactly
//                  its team count, no --bot/--bots): entrant e's
//                  comma-separated specs fill its team's slots in slot order,
//                  the last repeating, every team as large as the longest
//                  list (`--team ref:hard` alone fills a team of that size
//                  with one spec); the result line and the summary add the
//                  teams (arena_core.hpp)
//   --memory-on-respawn keep|wipe
//                  v0.35: what a bot keeps when its eliminated monkey comes
//                  back (wipe: init() again, a module gets a fresh
//                  instance; default keep)
//   --seed S       world seed of match 0; match k uses S + k (default 1);
//                  slot i is init()ed with slotSeed(S + k, i), arena_core.hpp
//   --matches M    a tournament of M matches (default 1)
//   --rotate       match k rotates the bots by k slots (bot b plays slot
//                  (b + k) mod n), so over n matches every bot plays every
//                  slot; the summary then adds the totals per bot (by index
//                  in the --bot order, the padding last) after those per slot.
//                  With --team it rotates the entrants across the sides:
//                  entrant e plays team (e + k) mod T
//   --ticks N      tick cap per match (default 36000, 10 minutes)
//   --fuel N       wasm instructions per tick for every module (default 100000)
//   --delay T      per-slot input delay in ticks (default 0: bot versus bot)
//   --replay F     write match 0's .kfmr to F; --replay-dir D writes every
//                  match to D/match_<k>.kfmr
//   --result F     also write the result lines (and the summary) to F
//   --json         one JSON object per line instead of the text lines: a
//                  match object per match, the summary last; the keys are
//                  append-only (docs/arena-result.md, the ladder parses them)
//   --quiet        no per-match lines on stdout (the summary still prints)
//   --log          print each module's captured kfm_log output at the end
//                  (stderr)
//   --content DIR, --arena, --mode, --loadout, --powerups: as kfm_replay; the content
//                  defaults to the copy built into the binary. v0.82: without
//                  --arena, a mode with an arena pool (`arenas`) plays match k
//                  on pool member (k / T) mod P in content order (T the teams,
//                  or the slots of a free-for-all; P the members), so with
//                  --rotate every entrant plays every side of a member before
//                  the next; --arena NAME plays that arena for every match
//
// Exit codes: 0 ok (a capped match is a result, not an error), 2 bad
// arguments (or content that fails to load), 3 a module failed to load (the
// LoadError message is printed, or a module spec in a build without
// KFM_WASM_BOTS), 4 I/O (a module file, the replay or the result file).
#include "arena_core.hpp"
#include "kfm/ai/agent_host.hpp"
#include "kfm/ai/bot_spec.hpp"
#include "kfm/content/app_content.hpp"
#include "kfm/sim/constants.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace kfm;

constexpr int kExitOk = 0;
constexpr int kExitArgs = 2;
constexpr int kExitModule = 3;
constexpr int kExitIo = 4;

constexpr uint32_t kMaxMatches = 1'000'000;
constexpr uint32_t kMaxTicks = 1'000'000;

constexpr const char* kUsage =
    "usage: kfm_arena [--bot SPEC]... [--bots N] | [--team SPEC,SPEC,...]...\n"
    "                 [--seed S] [--matches M] [--rotate]\n"
    "                 [--ticks N] [--arena NAME] [--mode NAME] [--loadout NAME]\n"
    "                 [--powerups LIST] [--memory-on-respawn keep|wipe]\n"
    "                 [--content DIR] [--fuel N] [--delay T]\n"
    "                 [--replay out.kfmr | --replay-dir DIR] [--result out.txt]\n"
    "                 [--json] [--quiet] [--log]\n"
    "  SPEC: ref | ref:easy | ref:medium | ref:hard | path/to/bot.wasm\n"
    "  --rotate: match k moves bot b to slot (b + k) mod n; the summary adds per-bot totals\n"
    "  --team: one per team of a team mode, its specs fill the team's slots in order\n"
    "  --json: one object per line, the summary last (docs/arena-result.md)\n"
    "  a mode's arena pool: without --arena, match k plays member (k / T) mod P\n";

struct Args {
  std::vector<std::string> bots;
  std::vector<std::string> teams;  // v0.35: one comma-separated list per --team
  ai::RespawnMemory memory{ai::RespawnMemory::Keep};
  std::optional<uint32_t> slots;
  uint64_t seed{arena::kDefaultSeed};
  uint32_t matches{1};
  uint32_t ticks{arena::kDefaultTickCap};
  uint32_t fuel{arena::kDefaultFuelPerTick};
  uint32_t delay{0};
  std::string contentDir;
  std::string arenaName;
  std::string modeName;
  std::string loadoutName;
  std::string powerUps;
  std::string replay;
  std::string replayDir;
  std::string result;
  bool rotate{false};
  bool json{false};
  bool quiet{false};
  bool log{false};
};

// A whole unsigned number in [lo, hi]; prints the error itself.
bool parseNumber(std::string_view flag, const char* text, uint64_t lo, uint64_t hi, uint64_t& out) {
  char* end = nullptr;
  const unsigned long long v = std::strtoull(text, &end, 0);
  if (end == text || *end != '\0' || text[0] == '-' || v < lo || v > hi) {
    std::fprintf(stderr, "error: %.*s must be a number in %llu..%llu, got '%s'\n",
                 static_cast<int>(flag.size()), flag.data(), static_cast<unsigned long long>(lo),
                 static_cast<unsigned long long>(hi), text);
    return false;
  }
  out = v;
  return true;
}

bool parseArgs(int argc, const char* const* argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool takesValue = arg == "--bot" || arg == "--bots" || arg == "--team" ||
                            arg == "--memory-on-respawn" || arg == "--seed" || arg == "--matches" ||
                            arg == "--ticks" || arg == "--fuel" || arg == "--delay" ||
                            arg == "--content" || arg == "--arena" || arg == "--mode" ||
                            arg == "--loadout" || arg == "--powerups" || arg == "--replay" ||
                            arg == "--replay-dir" || arg == "--result";
    if (takesValue) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s needs a value\n", argv[i]);
        return false;
      }
      const char* v = argv[++i];
      uint64_t n = 0;
      if (arg == "--bot") {
        a.bots.emplace_back(v);
      } else if (arg == "--team") {
        a.teams.emplace_back(v);
      } else if (arg == "--memory-on-respawn") {
        const std::string_view m = v;
        if (m == "keep") {
          a.memory = ai::RespawnMemory::Keep;
        } else if (m == "wipe") {
          a.memory = ai::RespawnMemory::Wipe;
        } else {
          std::fprintf(stderr, "error: --memory-on-respawn must be keep or wipe, got '%s'\n", v);
          return false;
        }
      } else if (arg == "--bots") {
        if (!parseNumber(arg, v, 1, sim::kMaxPlayers, n)) return false;
        a.slots = static_cast<uint32_t>(n);
      } else if (arg == "--seed") {
        if (!parseNumber(arg, v, 0, UINT64_MAX, n)) return false;
        a.seed = n;
      } else if (arg == "--matches") {
        if (!parseNumber(arg, v, 1, kMaxMatches, n)) return false;
        a.matches = static_cast<uint32_t>(n);
      } else if (arg == "--ticks") {
        if (!parseNumber(arg, v, 1, kMaxTicks, n)) return false;
        a.ticks = static_cast<uint32_t>(n);
      } else if (arg == "--fuel") {
        if (!parseNumber(arg, v, 1, UINT32_MAX, n)) return false;
        a.fuel = static_cast<uint32_t>(n);
      } else if (arg == "--delay") {
        if (!parseNumber(arg, v, 0, ai::kMaxAgentDelayTicks, n)) return false;
        a.delay = static_cast<uint32_t>(n);
      } else if (arg == "--content") {
        a.contentDir = v;
      } else if (arg == "--arena") {
        a.arenaName = v;
      } else if (arg == "--mode") {
        a.modeName = v;
      } else if (arg == "--loadout") {
        a.loadoutName = v;
      } else if (arg == "--powerups") {
        a.powerUps = v;
      } else if (arg == "--replay") {
        a.replay = v;
      } else if (arg == "--replay-dir") {
        a.replayDir = v;
      } else if (arg == "--result") {
        a.result = v;
      }
    } else if (arg == "--rotate") {
      a.rotate = true;
    } else if (arg == "--json") {
      a.json = true;
    } else if (arg == "--quiet") {
      a.quiet = true;
    } else if (arg == "--log") {
      a.log = true;
    } else if (arg == "--help" || arg == "-h") {
      std::fputs(kUsage, stdout);
      std::exit(kExitOk);
    } else {
      std::fprintf(stderr, "error: unknown argument '%s'\n", argv[i]);
      return false;
    }
  }
  if (!a.teams.empty() && (!a.bots.empty() || a.slots.has_value())) {
    std::fprintf(stderr, "error: --team replaces --bot and --bots\n");
    return false;
  }
  if (!a.replay.empty() && !a.replayDir.empty()) {
    std::fprintf(stderr, "error: --replay and --replay-dir are exclusive\n");
    return false;
  }
  return true;
}

bool writeFile(const std::filesystem::path& path, const std::vector<std::byte>& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(f);
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(misc-const-correctness): main signature
  Args a;
  if (!parseArgs(argc, argv, a)) {
    std::fputs(kUsage, stderr);
    return kExitArgs;
  }

  const auto parseSpec = [](const std::string& text) {
    const std::optional<ai::BotSpec> spec = ai::parseBotSpec(text);
    if (!spec) {
      std::fprintf(stderr, "error: '%s' is not a bot (ref, ref:easy|medium|hard or a .wasm path)\n",
                   text.c_str());
    }
    return spec;
  };
  // The bots, slot 0 first, padded with ref:hard; or the teams (v0.35).
  std::vector<ai::BotSpec> bots;
  std::vector<std::vector<ai::BotSpec>> teams;
  for (const std::string& list : a.teams) {
    std::vector<ai::BotSpec>& team = teams.emplace_back();
    size_t from = 0;
    while (true) {
      const size_t comma = list.find(',', from);
      const std::optional<ai::BotSpec> spec = parseSpec(
          list.substr(from, comma == std::string::npos ? std::string::npos : comma - from));
      if (!spec) return kExitArgs;
      team.push_back(*spec);
      bots.push_back(*spec);  // for the module cache below
      if (comma == std::string::npos) break;
      from = comma + 1;
    }
  }
  if (teams.empty()) {
    for (const std::string& text : a.bots) {
      const std::optional<ai::BotSpec> spec = parseSpec(text);
      if (!spec) return kExitArgs;
      bots.push_back(*spec);
    }
    const size_t slots = a.slots.value_or(std::max<size_t>(bots.size(), 2));
    if (bots.size() > slots || bots.size() > sim::kMaxPlayers) {
      std::fprintf(stderr, "error: %zu bots for %zu slots (at most %u)\n", bots.size(), slots,
                   sim::kMaxPlayers);
      return kExitArgs;
    }
    bots.resize(slots, ai::BotSpec{});  // BotSpec{} is ref:hard
  }

  // Embedded unless --content: a result must not depend on files next to the
  // binary.
  content::LoadedContent loaded;
  content::Diagnostics diag;
  const bool ok = content::loadAppContent(a.contentDir, {}, loaded, diag);
  if (!diag.items().empty()) std::fputs(diag.format().c_str(), stderr);
  if (!ok) {
    std::fprintf(stderr, "error: the content failed to load\n");
    return kExitArgs;
  }
  arena::MatchSetup setup;
  std::string error;
  if (!content::resolveMatchNames(loaded.db, a.arenaName, a.modeName, setup.config, error) ||
      !content::resolveLoadoutName(loaded.db, a.loadoutName, setup.config, error) ||
      !content::resolvePowerUpList(loaded.db, a.powerUps, setup.config.powerUps, error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return kExitArgs;
  }
  setup.bots = bots;
  setup.memory = a.memory;
  if (!teams.empty()) {
    // v0.35: a team mode with exactly one --team per team.
    const sim::ModeDef& mode = loaded.db.modeAt(setup.config.mode);
    if (mode.teams < 2 || teams.size() != mode.teams) {
      std::fprintf(stderr, "error: mode %.*s has %u teams, got %zu --team\n",
                   static_cast<int>(mode.name.view().size()), mode.name.view().data(),
                   unsigned{mode.teams}, teams.size());
      return kExitArgs;
    }
    size_t size = 0;
    for (const std::vector<ai::BotSpec>& t : teams)
      size = std::max(size, t.size());
    if (teams.size() * size > sim::kMaxPlayers) {
      std::fprintf(stderr, "error: %zu teams of %zu make more than %u slots\n", teams.size(), size,
                   sim::kMaxPlayers);
      return kExitArgs;
    }
    setup.teams = teams;
  }
  setup.tickCap = a.ticks;
  setup.delayTicks = static_cast<uint8_t>(a.delay);
  // v0.82: without --arena a mode's arena pool is cycled, match k on member
  // (k / T) mod P (T: the teams, or the slots of a free-for-all), so with
  // --rotate every entrant plays every side of a member before the next.
  const arena::ArenaCycle cycle = arena::arenaCycle(loaded.db, setup, !a.arenaName.empty());

  // Every module loads before the first match.
  arena::BotFactory factory(a.fuel);
  for (const ai::BotSpec& spec : bots) {
    if (auto prepared = factory.prepare(spec); !prepared) {
      std::fprintf(stderr, "error: %s\n", prepared.error().message.c_str());
      return prepared.error().kind == arena::BotErrorKind::Io ? kExitIo : kExitModule;
    }
  }

  if (!a.replayDir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(a.replayDir, ec);
    if (ec) {
      std::fprintf(stderr, "error: cannot create %s: %s\n", a.replayDir.c_str(),
                   ec.message().c_str());
      return kExitIo;
    }
  }
  std::ofstream resultFile;
  if (!a.result.empty()) {
    resultFile.open(a.result, std::ios::trunc);
    if (!resultFile) {
      std::fprintf(stderr, "error: cannot write %s\n", a.result.c_str());
      return kExitIo;
    }
  }
  const auto emit = [&](const std::string& line, bool toStdout) {
    if (toStdout) std::printf("%s\n", line.c_str());
    if (resultFile.is_open()) resultFile << line << '\n';
  };

  arena::Summary summary;
  std::vector<std::string> logs;
  for (uint32_t k = 0; k < a.matches; ++k) {
    setup.config.seed = a.seed + k;
    setup.rotation = a.rotate ? k : 0;
    setup.config.arena = cycle.arenaFor(k);
    std::filesystem::path replayPath;
    if (!a.replayDir.empty()) {
      replayPath = std::filesystem::path(a.replayDir) / std::format("match_{}.kfmr", k);
    } else if (k == 0 && !a.replay.empty()) {
      replayPath = a.replay;
    }
    setup.recordReplay = !replayPath.empty();
    auto result = arena::runMatch(setup, loaded.db, factory, k);
    if (!result) {
      std::fprintf(stderr, "error: %s\n", result.error().message.c_str());
      return result.error().kind == arena::BotErrorKind::Io ? kExitIo : kExitModule;
    }
    if (setup.recordReplay && (result->replay.empty() || !writeFile(replayPath, result->replay))) {
      std::fprintf(stderr, "error: cannot write %s\n", replayPath.string().c_str());
      return kExitIo;
    }
    emit(a.json ? arena::formatResultJson(*result) : arena::formatResultLine(*result), !a.quiet);
    summary.add(*result);
    if (a.log) {
      for (const arena::SlotResult& s : result->slots) {
        if (!s.module || s.log.empty()) continue;
        logs.push_back(std::format("--- log match {} slot {} {} ({} bytes) ---\n{}{}", k, s.slot,
                                   s.bot, s.logBytes, s.log, s.log.ends_with('\n') ? "" : "\n"));
      }
    }
  }
  emit(a.json ? arena::formatSummaryJson(summary) : arena::formatSummaryLine(summary), true);
  if (resultFile.is_open()) {
    resultFile.flush();
    if (!resultFile) {
      std::fprintf(stderr, "error: cannot write %s\n", a.result.c_str());
      return kExitIo;
    }
  }
  std::fflush(stdout);
  for (const std::string& text : logs)
    std::fputs(text.c_str(), stderr);
  return kExitOk;
}
