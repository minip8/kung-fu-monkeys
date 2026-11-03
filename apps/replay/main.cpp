// kfm_replay: headless replay tool and the determinism gate (architecture
// §9). M0 scope: record an empty replay, print per-tick checksums, verify a
// replay against an expected checksum file. The viewer arrives with M2.
//
//   kfm_replay record-empty <out.kfmr> [--ticks N] [--seed S] [--players P]
//   kfm_replay info <in.kfmr>
//   kfm_replay checksums <in.kfmr>              prints "<tick> <hex>" per tick
//   kfm_replay verify <in.kfmr> <expected.txt>  exit 0 when identical
#include "kfm/sim/sim.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
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
};

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
      a.ticks = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
    } else if (arg == "--seed") {
      if (!takeValue(v)) return false;
      a.seed = std::strtoull(v, nullptr, 0);
    } else if (arg == "--players") {
      if (!takeValue(v)) return false;
      a.players = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
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
  const ContentDB& content = defaultContent();
  const uint32_t buildContentHash = contentHash(content);
  // 0 marks a legacy replay recorded before content was hashed (M0).
  if (header.contentHash != 0 && header.contentHash != buildContentHash) {
    std::fprintf(stderr,
                 "error: content hash mismatch: replay 0x%08x, this build 0x%08x "
                 "(recorded with different content)\n",
                 header.contentHash, buildContentHash);
    return false;
  }
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

int cmdRecordEmpty(const Args& a) {
  if (a.positional.size() != 1) {
    std::fprintf(
        stderr, "usage: kfm_replay record-empty <out.kfmr> [--ticks N] [--seed S] [--players P]\n");
    return 2;
  }
  if (a.players == 0 || a.players > kMaxPlayers) {
    std::fprintf(stderr, "error: --players must be 1..%u\n", kMaxPlayers);
    return 2;
  }
  ReplayHeader h;
  h.seed = a.seed;
  h.contentHash = contentHash(defaultContent());
  h.buildHash = 0;
  h.players = static_cast<PlayerMask>((1u << a.players) - 1u);
  h.tickCount = a.ticks;

  std::vector<std::byte> buf(replayMaxBytes(h));
  BitWriter w(buf);
  writeReplayHeader(w, h);
  for (uint32_t t = 0; t < h.tickCount; ++t) {
    InputFrame f;
    f.tick = Tick{t};
    f.present = h.players;
    writeReplayFrame(w, f);
  }
  if (!w.ok()) {
    std::fprintf(stderr, "error: replay buffer overflow\n");
    return 1;
  }
  if (!writeFile(a.positional[0], w.written())) {
    std::fprintf(stderr, "error: cannot write %s\n", a.positional[0].c_str());
    return 1;
  }
  std::printf("wrote %s: %u ticks, %u players, seed 0x%llx, %zu bytes\n", a.positional[0].c_str(),
              h.tickCount, a.players, static_cast<unsigned long long>(h.seed), w.bytesWritten());
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
  std::printf(
      "replay %s\n  version      %u\n  seed         0x%llx\n  content hash 0x%08x\n"
      "  build hash   0x%08x\n  players      0x%03x\n  ticks        %u\n  bytes        %zu\n"
      "  this build's content hash 0x%08x\n",
      a.positional[0].c_str(), kReplayVersion, static_cast<unsigned long long>(h.seed),
      h.contentHash, h.buildHash, h.players, h.tickCount, bytes.size(),
      contentHash(defaultContent()));
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
    std::fprintf(stderr, "usage: kfm_replay <record-empty|info|checksums|verify> ...\n");
    return 2;
  }
  if (a.command == "record-empty") return cmdRecordEmpty(a);
  if (a.command == "info") return cmdInfo(a);
  if (a.command == "checksums") return cmdChecksums(a);
  if (a.command == "verify") return cmdVerify(a);
  std::fprintf(stderr, "error: unknown command '%s'\n", a.command.c_str());
  return 2;
}
