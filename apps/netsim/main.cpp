// kfm_netsim: N clients and one server in one process over the loopback
// emulator (architecture §4, §6.4, §9, §11 "netsim with 10 clients"),
// driven by the deterministic 1 ms scheduler of kfm::netsim, the same one
// the netcode tests use. A run is a pure function of its arguments.
//
//   kfm_netsim [--clients N] [--rtt MS] [--jitter MS] [--loss PCT] [--dup PCT]
//              [--reorder PCT] [--reorder-delay MS] [--seconds S] [--warmup S]
//              [--seed S] [--input-delay T] [--spectators N] [--join-at S]
//              [--cut C:D[:AT]] [--csv PATH] [--quiet]
//
// --clients N        playing clients, 1..10, one slot each (default 10)
// --rtt MS           round-trip time; each direction gets RTT/2 (default 100)
// --jitter MS        ± per direction, uniform (default 10)
// --loss PCT         loss per direction and lane, percent (reliable loss
//                    costs a retransmission of max(RTT, 20 ms)) (default 2)
// --dup PCT          duplicated unreliable packets, percent (default 0)
// --reorder PCT      unreliable packets held back by --reorder-delay so later
//                    ones overtake them, percent (default 0; delay 20 ms)
// --seconds S        simulated run length (default 30)
// --warmup S         lead and substitution numbers ignore the first S
//                    seconds (default min(10, seconds / 3))
// --seed S           network, match and fake-human seed (default 1)
// --input-delay T    fixed input delay, 0..3 ticks (default 0)
// --spectators N     extra clients without a slot (default 0)
// --join-at S        the last playing client joins at S seconds, mid-match
// --cut C:D[:AT]     cut client C's link (0-based) for D seconds at AT
//                    seconds (default: half the run), then reconnect it with
//                    its token (within 10 s it reclaims its slot)
// --csv PATH         one row per client per simulated second (kCsvHeader);
//                    `client` is the join order: players, spectators, then
//                    a --join-at client; rollbacks, depths, mispredicted,
//                    substitutions and bytes are deltas over that second
// --quiet            only the summary
//
// Prints the soak summary of architecture §11 (rollback depth max/p99/p50,
// mispredictions/s and rollbacks/s per client, checksums compared,
// mismatches, resyncs, lead against target, server substitutions/s, bytes/s
// up and down per client before transport overhead) and "KFM_NETSIM PASS" or
// "KFM_NETSIM FAIL: ..."; exits non-zero on any checksum mismatch, resync,
// rollback overflow, a run without a compared checksum, or a client that is
// not running at the end, so it doubles as a CI gate.
#include "kfm/net/netsim.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace kfm;
using namespace kfm::net;
using namespace kfm::net::netsim;

struct Cut {
  uint32_t client{0};
  double seconds{0};
  std::optional<double> at;
};

struct Options {
  uint32_t clients{10};
  double rttMs{100};
  double jitterMs{10};
  double lossPct{2};
  double dupPct{0};
  double reorderPct{0};
  double reorderDelayMs{20};
  double seconds{30};
  std::optional<double> warmup;
  uint64_t seed{1};
  uint32_t inputDelay{0};
  uint32_t spectators{0};
  std::optional<double> joinAt;
  std::optional<Cut> cut;
  std::string csv;
  bool quiet{false};
};

constexpr const char* kUsage =
    "usage: kfm_netsim [--clients N] [--rtt MS] [--jitter MS] [--loss PCT] [--dup PCT]\n"
    "                  [--reorder PCT] [--reorder-delay MS] [--seconds S] [--warmup S]\n"
    "                  [--seed S] [--input-delay T] [--spectators N] [--join-at S]\n"
    "                  [--cut C:D[:AT]] [--csv PATH] [--quiet]\n";

constexpr const char* kCsvHeader =
    "second,server_tick,client,running,local_tick,lead,target_lead,margin_q8,rate_ppm,"
    "rollbacks,depth_max,depth_p99,mispredicted,substitutions,bytes_up,bytes_down,"
    "checksums_compared,mismatches,resyncs\n";

template <typename T>
[[nodiscard]] bool parseInt(std::string_view v, T& out) {
  int base = 10;
  if (v.starts_with("0x") || v.starts_with("0X")) {
    v.remove_prefix(2);
    base = 16;
  }
  if (v.empty()) return false;
  const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), out, base);
  return ec == std::errc{} && end == v.data() + v.size();
}

[[nodiscard]] bool parseReal(std::string_view v, double& out) {
  const std::string s(v);
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return !s.empty() && end == s.c_str() + s.size() && out >= 0 && out < 1e9;
}

[[nodiscard]] bool parseCut(std::string_view v, Cut& c) {
  const size_t a = v.find(':');
  if (a == std::string_view::npos) return false;
  const size_t b = v.find(':', a + 1);
  if (!parseInt(v.substr(0, a), c.client)) return false;
  if (!parseReal(v.substr(a + 1, b == std::string_view::npos ? b : b - a - 1), c.seconds))
    return false;
  if (b != std::string_view::npos) {
    double at = 0;
    if (!parseReal(v.substr(b + 1), at)) return false;
    c.at = at;
  }
  return c.seconds > 0;
}

[[nodiscard]] bool parseArgs(int argc, const char* const* argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--quiet") {
      o.quiet = true;
      continue;
    }
    if (arg == "--help" || arg == "-h" || i + 1 >= argc) {
      std::fprintf(stderr, "%s", kUsage);
      return false;
    }
    const std::string_view v = argv[++i];
    double real = 0;
    bool ok = true;
    if (arg == "--clients") {
      ok = parseInt(v, o.clients) && o.clients >= 1 && o.clients <= sim::kMaxPlayers;
    } else if (arg == "--rtt") {
      ok = parseReal(v, o.rttMs);
    } else if (arg == "--jitter") {
      ok = parseReal(v, o.jitterMs);
    } else if (arg == "--loss") {
      ok = parseReal(v, o.lossPct) && o.lossPct <= 100;
    } else if (arg == "--dup") {
      ok = parseReal(v, o.dupPct) && o.dupPct <= 100;
    } else if (arg == "--reorder") {
      ok = parseReal(v, o.reorderPct) && o.reorderPct <= 100;
    } else if (arg == "--reorder-delay") {
      ok = parseReal(v, o.reorderDelayMs);
    } else if (arg == "--seconds") {
      ok = parseReal(v, o.seconds) && o.seconds > 0;
    } else if (arg == "--warmup") {
      ok = parseReal(v, real);
      o.warmup = real;
    } else if (arg == "--seed") {
      ok = parseInt(v, o.seed);
    } else if (arg == "--input-delay") {
      ok = parseInt(v, o.inputDelay) && o.inputDelay <= kMaxInputDelayTicks;
    } else if (arg == "--spectators") {
      ok = parseInt(v, o.spectators) && o.spectators <= 32;
    } else if (arg == "--join-at") {
      ok = parseReal(v, real);
      o.joinAt = real;
    } else if (arg == "--cut") {
      Cut c;
      ok = parseCut(v, c);
      o.cut = c;
    } else if (arg == "--csv") {
      o.csv = v;
    } else {
      std::fprintf(stderr, "error: unknown argument '%s'\n%s", argv[i - 1], kUsage);
      return false;
    }
    if (!ok) {
      std::fprintf(stderr, "error: bad value '%s' for %s\n", argv[i], argv[i - 1]);
      return false;
    }
  }
  if (const std::optional<Cut> cut = o.cut; cut && cut->client >= o.clients) {
    std::fprintf(stderr, "error: --cut client %u does not exist (--clients %u)\n", cut->client,
                 o.clients);
    return false;
  }
  return true;
}

[[nodiscard]] uint64_t toNs(double seconds) {
  return static_cast<uint64_t>(seconds * 1e9);
}

[[nodiscard]] uint32_t toPpm(double pct) {
  return static_cast<uint32_t>(pct * 10'000.0);
}

[[nodiscard]] LinkConditions conditions(const Options& o) {
  const auto oneWay = static_cast<uint64_t>(o.rttMs * 1e6 / 2);
  const uint64_t jitter = std::min(static_cast<uint64_t>(o.jitterMs * 1e6), oneWay);
  LinkConditions c = link(oneWay, jitter, toPpm(o.lossPct));
  c.unreliable.duplicatePpm = toPpm(o.dupPct);
  c.unreliable.reorderPpm = toPpm(o.reorderPct);
  c.unreliable.reorderDelayNs = static_cast<uint64_t>(o.reorderDelayMs * 1e6);
  return c;
}

// What a client had at the previous per-second sample.
struct Sample {
  std::array<uint64_t, kRollbackDepthBuckets> depth{};
  uint64_t rollbacks{0};
  uint64_t mispredicted{0};
  uint64_t substitutions{0};
  uint64_t up{0};
  uint64_t down{0};
};

class CsvWriter {
 public:
  explicit CsvWriter(const std::string& path) {
    if (path.empty()) return;
    file_ = std::fopen(path.c_str(), "w");  // NOLINT(cppcoreguidelines-owning-memory)
    if (file_ != nullptr) std::fputs(kCsvHeader, file_);
  }
  ~CsvWriter() {
    if (file_ != nullptr) std::fclose(file_);  // NOLINT(cppcoreguidelines-owning-memory)
  }
  CsvWriter(const CsvWriter&) = delete;
  CsvWriter& operator=(const CsvWriter&) = delete;
  CsvWriter(CsvWriter&&) = delete;
  CsvWriter& operator=(CsvWriter&&) = delete;

  [[nodiscard]] bool open() const { return file_ != nullptr; }

  void row(uint64_t second, Tick serverTick, size_t index, const ClientRig& rig, Sample& last) {
    const NetClient& c = *rig.client;
    const ClientNetStats& st = c.netStats();
    std::array<uint64_t, kRollbackDepthBuckets> delta{};
    for (size_t d = 0; d < delta.size(); ++d)
      delta[d] = st.rollbackDepth[d] - last.depth[d];
    if (file_ != nullptr) {
      std::fprintf(
          file_, "%llu,%u,%zu,%d,%u,%d,%d,%d,%u,%llu,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
          static_cast<unsigned long long>(second), serverTick.value, index, c.running() ? 1 : 0,
          c.localTick().value, st.leadTicks, st.targetLeadTicks, st.arrivalMarginQ8, st.ratePpm,
          static_cast<unsigned long long>(st.rollbacks - last.rollbacks), maxBucket(delta),
          percentile(delta, 99),
          static_cast<unsigned long long>(st.mispredictedTicks - last.mispredicted),
          static_cast<unsigned long long>(st.substitutionsSeen - last.substitutions),
          static_cast<unsigned long long>(st.bytesSent - last.up),
          static_cast<unsigned long long>(st.bytesReceived - last.down),
          static_cast<unsigned long long>(st.checksumsCompared),
          static_cast<unsigned long long>(st.checksumMismatches),
          static_cast<unsigned long long>(st.resyncRequests));
    }
    last.depth = st.rollbackDepth;
    last.rollbacks = st.rollbacks;
    last.mispredicted = st.mispredictedTicks;
    last.substitutions = st.substitutionsSeen;
    last.up = st.bytesSent;
    last.down = st.bytesReceived;
  }

 private:
  std::FILE* file_{nullptr};
};

[[nodiscard]] int run(const Options& o) {
  const auto wallStart = std::chrono::steady_clock::now();
  ServerConfig sc;
  sc.match.seed = o.seed;
  NetSim s(sc, o.seed);
  const LinkConditions l = conditions(o);
  ClientConfig base;
  base.inputDelayTicks = static_cast<uint8_t>(o.inputDelay);

  const uint64_t end = toNs(o.seconds);
  const uint64_t warmup = toNs(o.warmup.value_or(std::min(10.0, o.seconds / 3)));
  const uint32_t startNow = o.joinAt ? o.clients - 1 : o.clients;
  const auto addPlayer = [&](uint32_t i) {
    ClientConfig c = tokenConfig("player-" + std::to_string(i));
    c.inputDelayTicks = base.inputDelayTicks;
    s.addClient(l, l, c, Human::Scripted, o.seed * 1'000 + i + 1);
  };
  for (uint32_t i = 0; i < startNow; ++i)
    addPlayer(i);
  for (uint32_t i = 0; i < o.spectators; ++i) {
    const ClientConfig c = tokenConfig("spectator-" + std::to_string(i), 0);
    s.addClient(l, l, c, Human::Idle);
  }
  if (!o.quiet) {
    std::printf(
        "kfm_netsim: %u clients + %u spectators, rtt %.0f ms ±%.0f ms, loss %.2f%%, dup %.2f%%, "
        "reorder %.2f%%, %.0f s, seed %llu, input delay %u\n",
        o.clients, o.spectators, o.rttMs, o.jitterMs, o.lossPct, o.dupPct, o.reorderPct, o.seconds,
        static_cast<unsigned long long>(o.seed), o.inputDelay);
  }

  CsvWriter csv(o.csv);
  if (!o.csv.empty() && !csv.open()) {
    std::fprintf(stderr, "error: cannot write %s\n", o.csv.c_str());
    return EXIT_FAILURE;
  }
  std::vector<Sample> samples;
  LeadSampler lead;
  s.afterClientFrame = [&](ClientRig& r) {
    if (s.now() >= warmup) lead.sample(s, r, s.server().nextTick());
  };

  // A client index maps to its rig; the late joiner is appended later.
  std::vector<ClientRig*> players;
  players.reserve(o.clients);
  for (uint32_t i = 0; i < startNow; ++i)
    players.push_back(&s.rigs()[i]);
  const uint64_t joinAt = o.joinAt ? toNs(*o.joinAt) : 0;
  const Cut cut = o.cut.value_or(Cut{});
  const uint64_t cutAt = toNs(cut.at.value_or(o.seconds / 2));
  const uint64_t cutEnd = cutAt + toNs(cut.seconds);
  bool joined = !o.joinAt;
  bool cutDone = !o.cut;
  bool reconnected = !o.cut;
  uint64_t substAfter = 0;
  bool warm = false;
  uint64_t nextSecond = 1;

  while (s.now() < end) {
    s.step();
    const uint64_t now = s.now();
    if (!warm && now >= warmup) {
      warm = true;
      substAfter = s.server().stats().substitutedInputs;
    }
    if (!joined && now >= joinAt) {
      joined = true;
      addPlayer(o.clients - 1);
      // Keep players[] in client order: the joiner is the last playing client.
      ClientRig* joiner = &s.rigs().back();
      players.push_back(joiner);
      if (!o.quiet)
        std::printf("[%.3f s] client %u joins\n", static_cast<double>(now) / 1e9, o.clients - 1);
    }
    if (!cutDone && now >= cutAt && cut.client < players.size()) {
      cutDone = true;
      s.network().cut(players[cut.client]->transport->self());
      if (!o.quiet)
        std::printf("[%.3f s] client %u cut for %.1f s\n", static_cast<double>(now) / 1e9,
                    cut.client, cut.seconds);
    }
    if (cutDone && !reconnected && now >= cutEnd) {
      reconnected = true;
      s.reconnect(*players[cut.client]);
      if (!o.quiet)
        std::printf("[%.3f s] client %u reconnects\n", static_cast<double>(now) / 1e9, cut.client);
    }
    if (now >= nextSecond * kSec) {
      samples.resize(s.rigs().size());
      for (size_t i = 0; i < s.rigs().size(); ++i)
        csv.row(nextSecond, s.server().nextTick(), i, s.rigs()[i], samples[i]);
      ++nextSecond;
    }
  }
  s.afterClientFrame = nullptr;

  const double wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
  const double measured = static_cast<double>(end - std::min(end, warmup)) / 1e9;
  const Report r = summarise(s, static_cast<double>(s.now()) / 1e9, lead, substAfter, measured);
  std::printf("%s\n", format("netsim", r, wall).c_str());

  std::string failure;
  const auto fail = [&](const std::string& why) {
    if (!failure.empty()) failure += "; ";
    failure += why;
  };
  if (r.mismatches != 0) fail(std::to_string(r.mismatches) + " checksum mismatches");
  if (r.resyncs != 0) fail(std::to_string(r.resyncs) + " resyncs");
  if (r.overflows != 0) fail(std::to_string(r.overflows) + " rollback overflows");
  if (r.compared == 0) fail("no checksum compared");
  for (size_t i = 0; i < s.rigs().size(); ++i) {
    if (!s.rigs()[i].client->running()) fail("client " + std::to_string(i) + " not running");
  }
  if (failure.empty()) {
    std::printf("KFM_NETSIM PASS\n");
    return EXIT_SUCCESS;
  }
  std::printf("KFM_NETSIM FAIL: %s\n", failure.c_str());
  return EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(misc-const-correctness): main signature
  Options o;
  if (!parseArgs(argc, argv, o)) return EXIT_FAILURE;
  return run(o);
}
