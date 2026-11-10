// kfm_replay: headless replay tool, the determinism gate and the M1 bench
// (architecture §9). Records empty or scripted replays, prints per-tick
// checksums, verifies a replay against an expected checksum file, and times
// a scripted match, and measures scripted matches for feel tuning (stats.hpp).
// The viewer arrives with M2.
//
//   kfm_replay record-empty <out.kfmr> [--ticks N] [--seed S] [--players P]
//   kfm_replay record-scripted <out.kfmr> [--ticks N] [--seed S] [--players P] [--input-seed I]
//                                         [--stats]
//     record-*, bench and stats also take --arena NAME --mode NAME
//     --loadout NAME (default: the first of each) and --powerups LIST (all,
//     none or comma-separated names; default all); the replay header stores
//     their indices and the set
//   kfm_replay info <in.kfmr>
//   kfm_replay checksums <in.kfmr>              prints "<tick> <hex>" per tick
//   kfm_replay verify <in.kfmr> <expected.txt>  exit 0 when identical
//   kfm_replay bench [--ticks N] [--seed S] [--players P] [--input-seed I]
//   kfm_replay stats [--matches M] [--ticks N] [--seed S] [--players P] [--input-seed I]
//                    [--throw-one-in N] [--dash-one-in N] [--dodge-per-256 N]
//                    [--dash-attack-one-in N] [--quiet]
//     match k uses seed S + k and input seed I + k; --ticks caps each match
//     (default 36000, 10 minutes); the four bot knobs override
//     ScriptedInputConfig (sensitivity checks: is a number the content's or
//     the scripted bot's?); --quiet drops the per-match lines
//   --content DIR (any command): load content/*.toml from DIR instead of the
//     copy built into the binary (architecture §5.10). Recording stores the
//     content hash; playback refuses a replay recorded with other content.
#include "kfm/content/app_content.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/sim/scripted_inputs.hpp"
#include "kfm/sim/sim.hpp"
#include "stats.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace kfm;
using namespace kfm::sim;

struct Args {
  std::string command;
  std::vector<std::string> positional;
  uint32_t ticks{600};
  uint64_t seed{0x4B464D00ULL};
  uint32_t players{10};
  uint64_t inputSeed{0x5C121DULL};  // scripted-input Rng, separate from the world's
  bool ticksSet{false};
  uint32_t matches{20};
  bool stats{false};
  bool quiet{false};
  ScriptedInputConfig bot{};  // stats only; replays always use the defaults
  std::string contentDir;     // empty: the embedded content
  std::string arena;          // names; empty = index 0
  std::string mode;
  std::string loadout;
  std::string powerUps;  // --powerups list; empty = all
};

// The content every command runs with, loaded once in main() (--content or
// the embedded copy), and the arena/mode/loadout indices of --arena/--mode/
// --loadout with the power-up set of --powerups.
content::LoadedContent gContent;
MatchConfig gMatch{};

// Default per-match tick cap of `stats`: 10 minutes.
constexpr uint32_t kStatsDefaultTicks = 36'000;
constexpr uint32_t kMaxMatches = 100'000;

// Upper bound for --ticks: about 4.6 hours of play at 60 Hz and 56 MB of
// replay buffer, far from overflowing replayMaxBytes or exhausting memory.
constexpr uint32_t kMaxTicks = 1'000'000;

constexpr const char* kUsage =
    "usage: kfm_replay <command> ...\n"
    "  record-empty <out.kfmr> [--ticks N] [--seed S] [--players P] [--arena A] [--mode M]\n"
    "               [--loadout L] [--powerups LIST]\n"
    "  record-scripted <out.kfmr> [--ticks N] [--seed S] [--players P] [--input-seed I]\n"
    "                  [--arena A] [--mode M] [--loadout L] [--powerups LIST]\n"
    "  info <in.kfmr>\n"
    "  checksums <in.kfmr>\n"
    "  verify <in.kfmr> <expected.checksums>\n"
    "  bench [--ticks N] [--seed S] [--players P] [--input-seed I]\n"
    "  record-scripted ... --stats   also print feel-tuning statistics of the match\n"
    "  stats [--matches M] [--ticks N] [--seed S] [--players P] [--input-seed I]\n"
    "        [--throw-one-in N] [--dash-one-in N] [--dodge-per-256 N]\n"
    "        [--dash-attack-one-in N] [--quiet]\n"
    "  every command: --content DIR   content/*.toml to use (default: built in)\n"
    "  record-*, bench, stats: --arena A --mode M --loadout L\n"
    "                         --powerups all|none|name,name...\n";

bool parseArgs(int argc, char** argv, Args& a) {
  if (argc < 2) return false;
  a.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto takeValue = [&](const char*& out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    const char* v = nullptr;
    if (arg == "--ticks") {
      if (!takeValue(v)) return false;
      char* end = nullptr;
      const unsigned long long ticks = std::strtoull(v, &end, 0);
      if (end == v || *end != '\0' || ticks > kMaxTicks) {
        std::fprintf(stderr, "error: --ticks must be a number in 0..%u, got '%s'\n", kMaxTicks, v);
        return false;
      }
      a.ticks = static_cast<uint32_t>(ticks);
      a.ticksSet = true;
    } else if (arg == "--seed") {
      if (!takeValue(v)) return false;
      a.seed = std::strtoull(v, nullptr, 0);
    } else if (arg == "--players") {
      if (!takeValue(v)) return false;
      a.players = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
    } else if (arg == "--input-seed") {
      if (!takeValue(v)) return false;
      a.inputSeed = std::strtoull(v, nullptr, 0);
    } else if (arg == "--matches") {
      if (!takeValue(v)) return false;
      char* end = nullptr;
      const unsigned long long matches = std::strtoull(v, &end, 0);
      if (end == v || *end != '\0' || matches == 0 || matches > kMaxMatches) {
        std::fprintf(stderr, "error: --matches must be a number in 1..%u, got '%s'\n", kMaxMatches,
                     v);
        return false;
      }
      a.matches = static_cast<uint32_t>(matches);
    } else if (arg == "--throw-one-in" || arg == "--dash-one-in" || arg == "--dodge-per-256" ||
               arg == "--dash-attack-one-in") {
      if (!takeValue(v)) return false;
      char* end = nullptr;
      const unsigned long long n = std::strtoull(v, &end, 0);
      const unsigned long long max = arg == "--dodge-per-256" ? 255 : 65535;
      if (end == v || *end != '\0' || n > max) {
        std::fprintf(stderr, "error: %s must be a number in 0..%llu, got '%s'\n", argv[i - 1], max,
                     v);
        return false;
      }
      if (arg == "--throw-one-in") a.bot.throwOneIn = static_cast<uint16_t>(n);
      if (arg == "--dash-one-in") a.bot.dashOneIn = static_cast<uint16_t>(n);
      if (arg == "--dodge-per-256") a.bot.dodgePer256 = static_cast<uint8_t>(n);
      if (arg == "--dash-attack-one-in") a.bot.dashAttackOneIn = static_cast<uint16_t>(n);
    } else if (arg == "--content" || arg == "--arena" || arg == "--mode" || arg == "--loadout" ||
               arg == "--powerups") {
      if (!takeValue(v)) return false;
      if (arg == "--content") a.contentDir = v;
      if (arg == "--arena") a.arena = v;
      if (arg == "--mode") a.mode = v;
      if (arg == "--loadout") a.loadout = v;
      if (arg == "--powerups") a.powerUps = v;
    } else if (arg == "--stats") {
      a.stats = true;
    } else if (arg == "--quiet") {
      a.quiet = true;
    } else {
      a.positional.emplace_back(arg);
    }
  }
  return true;
}

bool readFile(const std::string& path, std::vector<std::byte>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  out.resize(raw.size());
  std::memcpy(out.data(), raw.data(), raw.size());
  return true;
}

bool writeFile(const std::string& path, std::span<const std::byte> data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(f);
}

// Runs a replay and calls onTick(tick, checksum) for the state at every tick
// 0..tickCount inclusive (the last call is the state after the final step).
template <class F>
bool runReplay(std::span<const std::byte> bytes, ReplayHeader& header, F&& onTick) {
  BitReader r(bytes);
  if (!readReplayHeader(r, header)) {
    std::fprintf(stderr, "error: not a KFMR replay (bad magic/version/header)\n");
    return false;
  }
  const ContentDB& db = gContent.db;
  if (header.contentHash != gContent.hash) {
    std::fprintf(stderr,
                 "error: content hash mismatch: replay 0x%08x, loaded 0x%08x "
                 "(recorded with different content)\n",
                 header.contentHash, gContent.hash);
    return false;
  }
  if (header.arena >= db.arenaCount || header.mode >= db.modeCount) {
    std::fprintf(stderr, "error: replay arena %u / mode %u not in the content\n", header.arena,
                 header.mode);
    return false;
  }
  const MatchContent content = matchContent(db, header.matchConfig());
  WorldState world = initialWorld(header.matchConfig());
  EventSink sink;
  onTick(world.tick, checksum(world));
  for (uint32_t i = 0; i < header.tickCount; ++i) {
    InputFrame frame;
    if (!readReplayFrame(r, frame)) {
      std::fprintf(stderr, "error: replay truncated at tick %u\n", i);
      return false;
    }
    if (frame.tick != world.tick) {
      std::fprintf(stderr, "error: frame tick %u does not match world tick %u\n", frame.tick.value,
                   world.tick.value);
      return false;
    }
    sink.clear();
    step(world, frame, content, sink);
    onTick(world.tick, checksum(world));
  }
  return true;
}

bool checkPlayers(const Args& a) {
  if (a.players == 0 || a.players > kMaxPlayers) {
    std::fprintf(stderr, "error: --players must be 1..%u\n", kMaxPlayers);
    return false;
  }
  return true;
}

ReplayHeader headerFor(const Args& a) {
  ReplayHeader h;
  h.seed = a.seed;
  h.contentHash = gContent.hash;
  h.buildHash = 0;
  h.players = static_cast<PlayerMask>((1u << a.players) - 1u);
  h.arena = gMatch.arena;
  h.mode = gMatch.mode;
  h.powerUps = gMatch.powerUps;
  h.loadout = gMatch.loadout;
  h.teams = gMatch.teams;
  h.tickCount = a.ticks;
  return h;
}

// Writes a replay whose frame for each tick comes from makeFrame(tick).
template <class MakeFrame>
int writeReplay(const Args& a, const ReplayHeader& h, MakeFrame&& makeFrame) {
  std::vector<std::byte> buf(replayMaxBytes(h));
  BitWriter w(buf);
  writeReplayHeader(w, h);
  for (uint32_t t = 0; t < h.tickCount; ++t) {
    writeReplayFrame(w, makeFrame(Tick{t}));
  }
  if (!w.ok()) {
    std::fprintf(stderr, "error: replay buffer overflow\n");
    return 1;
  }
  if (!writeFile(a.positional[0], w.written())) {
    std::fprintf(stderr, "error: cannot write %s\n", a.positional[0].c_str());
    return 1;
  }
  std::printf(
      "wrote %s: %u ticks, %u players, seed 0x%llx, arena %s, mode %s, loadout %s, power-ups %s, "
      "%zu bytes\n",
      a.positional[0].c_str(), h.tickCount, a.players, static_cast<unsigned long long>(h.seed),
      gContent.db.arenaAt(h.arena).name.cStr(), gContent.db.modeAt(h.mode).name.cStr(),
      gContent.db.loadoutAt(h.loadout).name.cStr(),
      content::powerUpListText(gContent.db, h.powerUps).c_str(), w.bytesWritten());
  return 0;
}

int cmdRecordEmpty(const Args& a) {
  if (a.positional.size() != 1) {
    std::fprintf(
        stderr, "usage: kfm_replay record-empty <out.kfmr> [--ticks N] [--seed S] [--players P]\n");
    return 2;
  }
  if (!checkPlayers(a)) return 2;
  const ReplayHeader h = headerFor(a);
  return writeReplay(a, h, [&](Tick t) {
    InputFrame f;
    f.tick = t;
    f.present = h.players;
    return f;
  });
}

// statsStep() plus a shadow world advanced by step(): the statistics are
// only reported while both checksums agree (stats.hpp). Heap-allocated by
// callers: it holds a whole WorldState.
struct StatsRun {
  explicit StatsRun(const WorldState& start) : shadow(start) {}

  void step(WorldState& world, const InputFrame& f, EventSink& sink) {
    const MatchContent content = matchContent(gContent.db, gMatch);
    replay::statsStep(world, f, content, sink, collector);
    shadowSink.clear();
    sim::step(shadow, f, content, shadowSink);
    if (mismatchTick < 0 && checksum(shadow) != checksum(world)) {
      mismatchTick = static_cast<int64_t>(f.tick.value);
    }
    collector.afterStep(world, sink);
  }

  [[nodiscard]] bool ok() const {
    if (mismatchTick < 0) return true;
    std::fprintf(stderr,
                 "error: statsStep diverged from step() at tick %lld; make statsStep() in "
                 "apps/replay/stats.cpp match engine/sim/src/step.cpp\n",
                 static_cast<long long>(mismatchTick));
    return false;
  }

  WorldState shadow;
  EventSink shadowSink;
  replay::StatsCollector collector;
  int64_t mismatchTick{-1};
};

// Scripted inputs read the state, so the match is simulated while recording;
// playback then reproduces it from the inputs alone.
int cmdRecordScripted(const Args& a) {
  if (a.positional.size() != 1) {
    std::fprintf(stderr,
                 "usage: kfm_replay record-scripted <out.kfmr> [--ticks N] [--seed S] "
                 "[--players P] [--input-seed I]\n");
    return 2;
  }
  if (!checkPlayers(a)) return 2;
  const ReplayHeader h = headerFor(a);
  const MatchContent content = matchContent(gContent.db, h.matchConfig());
  WorldState world = initialWorld(h.matchConfig());
  Rng inputs = Rng::seeded(a.inputSeed);
  EventSink sink;
  std::unique_ptr<StatsRun> stats;
  if (a.stats) stats = std::make_unique<StatsRun>(world);
  const int rc = writeReplay(a, h, [&](Tick t) {
    const InputFrame f = scriptedFrame(inputs, world, content);
    KFM_CHECK(f.tick == t);
    sink.clear();
    if (stats) {
      stats->step(world, f, sink);
    } else {
      step(world, f, content, sink);
    }
    return f;
  });
  if (rc == 0) {
    std::printf("  input seed 0x%llx, final tick %u, round %u, phase %u\n",
                static_cast<unsigned long long>(a.inputSeed), world.tick.value, world.rules.round,
                static_cast<unsigned>(world.rules.phase));
  }
  if (rc == 0 && stats) {
    if (!stats->ok()) return 1;
    stats->collector.finish(world);
    replay::printSummary(stats->collector.stats(), a.players, content);
  }
  return rc;
}

// Times scripted matches: input generation + step(), and step() alone. A
// match that ends is replaced by a fresh one (next seed), so every timed tick
// is live gameplay rather than an idle MatchEnded.
int cmdBench(const Args& a) {
  if (!checkPlayers(a)) return 2;
  using Clock = std::chrono::steady_clock;
  const ReplayHeader h = headerFor(a);
  const MatchContent content = matchContent(gContent.db, h.matchConfig());
  WorldState world = initialWorld(h.matchConfig());
  Rng inputs = Rng::seeded(a.inputSeed);
  EventSink sink;
  uint64_t events = 0;
  uint32_t matches = 1;
  Clock::duration stepTime{};
  const Clock::time_point begin = Clock::now();
  for (uint32_t t = 0; t < a.ticks; ++t) {
    if (world.rules.phase == MatchPhase::MatchEnded) {
      MatchConfig next = h.matchConfig();
      next.seed = h.seed + matches;
      world = initialWorld(next);
      ++matches;
    }
    const InputFrame f = scriptedFrame(inputs, world, content);
    sink.clear();
    const Clock::time_point s0 = Clock::now();
    step(world, f, content, sink);
    stepTime += Clock::now() - s0;
    events += sink.count;
  }
  const Clock::duration total = Clock::now() - begin;
  const double totalUs = std::chrono::duration<double, std::micro>(total).count();
  const double stepUs = std::chrono::duration<double, std::micro>(stepTime).count();
  const double ticks = a.ticks == 0 ? 1.0 : static_cast<double>(a.ticks);
  std::printf(
      "bench: %u players, %u ticks, %u matches, %llu events, checksum %08x\n"
      "  step + scripted inputs: %.0f ticks/s, %.2f us/tick\n"
      "  step only:              %.0f ticks/s, %.2f us/tick\n",
      a.players, a.ticks, matches, static_cast<unsigned long long>(events), checksum(world),
      ticks / (totalUs * 1e-6), totalUs / ticks, ticks / (stepUs * 1e-6), stepUs / ticks);
  return 0;
}

// Scripted matches for feel tuning: match k uses seed + k and input seed + k
// and runs until MatchEnded or the tick cap.
int cmdStats(const Args& a) {
  if (!checkPlayers(a) || !a.positional.empty()) {
    if (!a.positional.empty()) std::fputs(kUsage, stderr);
    return 2;
  }
  const ReplayHeader h = headerFor(a);
  const MatchContent content = matchContent(gContent.db, h.matchConfig());
  const uint32_t cap = a.ticksSet ? a.ticks : kStatsDefaultTicks;
  replay::Stats total;
  for (uint32_t m = 0; m < a.matches; ++m) {
    const uint64_t seed = a.seed + m;
    const uint64_t inputSeed = a.inputSeed + m;
    MatchConfig config = h.matchConfig();
    config.seed = seed;
    WorldState world = initialWorld(config);
    Rng inputs = Rng::seeded(inputSeed);
    EventSink sink;
    const auto run = std::make_unique<StatsRun>(world);
    for (uint32_t t = 0; t < cap && world.rules.phase != MatchPhase::MatchEnded; ++t) {
      const InputFrame f = scriptedFrame(inputs, world, content, a.bot);
      sink.clear();
      run->step(world, f, sink);
    }
    if (!run->ok()) return 1;
    run->collector.finish(world);
    if (!a.quiet) replay::printMatchLine(m, seed, inputSeed, run->collector.stats());
    total.merge(run->collector.stats());
  }
  replay::printSummary(total, a.players, content);
  return 0;
}

int cmdInfo(const Args& a) {
  std::vector<std::byte> bytes;
  if (a.positional.size() != 1 || !readFile(a.positional[0], bytes)) {
    std::fprintf(stderr, "usage: kfm_replay info <in.kfmr>\n");
    return 2;
  }
  BitReader r(bytes);
  ReplayHeader h;
  if (!readReplayHeader(r, h)) {
    std::fprintf(stderr, "error: not a KFMR replay\n");
    return 1;
  }
  // Names only mean something when the replay's content is the loaded one.
  const bool same = h.contentHash == gContent.hash;
  const auto nameOr = [&](bool ok, const ContentName& n) {
    return std::string(ok ? n.view() : std::string_view("?"));
  };
  const std::string arena =
      nameOr(same && h.arena < gContent.db.arenaCount, gContent.db.arenaAt(h.arena).name);
  const std::string mode =
      nameOr(same && h.mode < gContent.db.modeCount, gContent.db.modeAt(h.mode).name);
  const std::string loadout =
      nameOr(same && h.loadout < gContent.db.loadoutCount, gContent.db.loadoutAt(h.loadout).name);
  const std::string powerUps = same ? content::powerUpListText(gContent.db, h.powerUps) : "?";
  std::printf(
      "replay %s\n  version      %u\n  seed         0x%llx\n  content hash 0x%08x%s\n"
      "  build hash   0x%08x\n  players      0x%03x\n  arena        %u (%s)\n"
      "  mode         %u (%s)\n  loadout      %u (%s)\n  power-ups    0x%04x (%s)\n"
      "  ticks        %u\n  bytes        %zu\n  loaded %s\n",
      a.positional[0].c_str(), kReplayVersion, static_cast<unsigned long long>(h.seed),
      h.contentHash, same ? " (matches the loaded content)" : " (differs from the loaded content)",
      h.buildHash, h.players, h.arena, arena.c_str(), h.mode, mode.c_str(), h.loadout,
      loadout.c_str(), h.powerUps, powerUps.c_str(), h.tickCount, bytes.size(),
      gContent.describe().c_str());
  return 0;
}

int cmdChecksums(const Args& a) {
  std::vector<std::byte> bytes;
  if (a.positional.size() != 1 || !readFile(a.positional[0], bytes)) {
    std::fprintf(stderr, "usage: kfm_replay checksums <in.kfmr>\n");
    return 2;
  }
  ReplayHeader h;
  const bool ok =
      runReplay(bytes, h, [](Tick t, uint32_t c) { std::printf("%u %08x\n", t.value, c); });
  return ok ? 0 : 1;
}

int cmdVerify(const Args& a) {
  std::vector<std::byte> bytes;
  if (a.positional.size() != 2 || !readFile(a.positional[0], bytes)) {
    std::fprintf(stderr, "usage: kfm_replay verify <in.kfmr> <expected.checksums>\n");
    return 2;
  }
  std::ifstream expected(a.positional[1]);
  if (!expected) {
    std::fprintf(stderr, "error: cannot read %s\n", a.positional[1].c_str());
    return 2;
  }
  uint32_t mismatches = 0;
  uint32_t compared = 0;
  bool truncated = false;
  ReplayHeader h;
  const bool ran = runReplay(bytes, h, [&](Tick t, uint32_t c) {
    unsigned long expTick = 0;
    unsigned long expSum = 0;
    if (!(expected >> expTick >> std::hex >> expSum >> std::dec)) {
      truncated = true;
      return;
    }
    ++compared;
    if (expTick != t.value || expSum != c) {
      if (mismatches == 0) {
        std::fprintf(stderr, "DESYNC at tick %u: expected %lu %08lx, got %u %08x\n", t.value,
                     expTick, expSum, t.value, c);
      }
      ++mismatches;
    }
  });
  if (!ran) return 1;
  if (truncated) {
    std::fprintf(stderr, "error: expected file has fewer lines than the replay has ticks\n");
    return 1;
  }
  unsigned long extraTick = 0;
  if (expected >> extraTick) {
    std::fprintf(stderr, "error: expected file has more lines than the replay has ticks\n");
    return 1;
  }
  if (mismatches != 0) {
    std::fprintf(stderr, "FAIL: %u of %u ticks mismatched\n", mismatches, compared);
    return 1;
  }
  std::printf("OK: %u ticks, %u checksums identical\n", h.tickCount, compared);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parseArgs(argc, argv, a)) {
    std::fputs(kUsage, stderr);
    return 2;
  }
  // Embedded unless --content: the determinism gate must not depend on files
  // next to the binary (and runs the same under node).
  content::Diagnostics diag;
  const bool loaded = content::loadAppContent(a.contentDir, {}, gContent, diag);
  if (!diag.items().empty()) std::fputs(diag.format().c_str(), stderr);
  if (!loaded) {
    std::fprintf(stderr, "error: the content failed to load\n");
    return 1;
  }
  std::string error;
  if (!content::resolveMatchNames(gContent.db, a.arena, a.mode, gMatch, error) ||
      !content::resolveLoadoutName(gContent.db, a.loadout, gMatch, error) ||
      !content::resolvePowerUpList(gContent.db, a.powerUps, gMatch.powerUps, error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 2;
  }
  if (a.command == "record-empty") return cmdRecordEmpty(a);
  if (a.command == "record-scripted") return cmdRecordScripted(a);
  if (a.command == "bench") return cmdBench(a);
  if (a.command == "stats") return cmdStats(a);
  if (a.command == "info") return cmdInfo(a);
  if (a.command == "checksums") return cmdChecksums(a);
  if (a.command == "verify") return cmdVerify(a);
  std::fprintf(stderr, "error: unknown command '%s'\n%s", a.command.c_str(), kUsage);
  return 2;
}
