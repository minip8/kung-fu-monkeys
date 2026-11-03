#include "kfm/net/netsim.hpp"

#include "kfm/sim/scripted_inputs.hpp"
#include "kfm/sim/step.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace kfm::net::netsim {

sim::ContentDB endless(const sim::ContentDB& content) {
  sim::ContentDB x = content;
  for (uint8_t i = 0; i < x.modeCount; ++i) {
    x.modes[i].roundsToWin = 255;
    x.modes[i].scoreToWin = 255;
    x.modes[i].timeLimitTicks = 0;
  }
  return x;
}

const sim::ContentDB& endlessContent() {
  static const sim::ContentDB c = endless(sim::defaultContent());
  return c;
}

LinkConditions link(uint64_t latencyNs, uint64_t jitterNs, uint32_t lossPpm) {
  LinkConditions c = LinkConditions::uniform(latencyNs - jitterNs, 2 * jitterNs, lossPpm);
  c.reliable.retransmitNs = std::max<uint64_t>(2 * latencyNs, 20 * kMs);
  return c;
}

ClientConfig tokenConfig(const std::string& token, uint8_t slots) {
  ClientConfig c;
  c.requestedSlots = slots;
  c.setToken(std::as_bytes(std::span(token.data(), token.size())));
  return c;
}

NetSim::NetSim(ServerConfig config, uint64_t seed, const sim::ContentDB& content)
    : content_(&content), net_(seed) {
  server_ = std::make_unique<NetServer>(net_.server(), content, std::move(config));
  sums_.push_back(sim::checksum(server_->world()));
}

ClientRig& NetSim::addClient(const LinkConditions& up, const LinkConditions& down,
                             const ClientConfig& config, Human human, uint64_t humanSeed) {
  ClientRig& r = rigs_.emplace_back();
  r.transport = &net_.connectClient(now_, up, down);
  r.client = std::make_unique<NetClient>(*content_, config);
  r.rng = Rng::seeded(humanSeed != 0 ? humanSeed : 0xC11E47 + rigs_.size());
  r.human = human;
  r.up = up;
  r.down = down;
  r.client->connect(*r.transport, now_);
  return r;
}

void NetSim::reconnect(ClientRig& r) {
  r.transport = &net_.connectClient(now_, r.up, r.down);
  r.client->connect(*r.transport, now_);
}

void NetSim::step() {
  now_ += kMs;
  server_->pump(now_);
  while (now_ >= serverTickAt(serverTicks_)) {
    server_->tick(now_);
    ++serverTicks_;
    sums_.push_back(sim::checksum(server_->world()));
    if (afterServerTick) afterServerTick();
  }
  for (ClientRig& r : rigs_) {
    if (r.active) stepClient(r);
  }
  server_->clearEvents();
}

void NetSim::runFor(uint64_t ns) {
  const uint64_t end = now_ + ns;
  while (now_ < end)
    step();
}

bool NetSim::allRunning() const {
  return std::ranges::all_of(rigs_, [](const ClientRig& r) { return r.client->running(); });
}

void NetSim::stepClient(ClientRig& r) const {
  NetClient& c = *r.client;
  c.beginFrame();
  c.pump(now_);
  c.reconcile();
  const uint32_t n = c.ticksDue(now_);
  for (uint32_t i = 0; i < n; ++i) {
    std::array<sim::PlayerInput, sim::kMaxPlayers> inputs{};
    size_t k = 0;
    for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
      const PlayerId id{static_cast<PlayerId::rep>(s)};
      if (!sim::maskHas(c.localSlots(), id)) continue;
      inputs[k++] = r.human == Human::Scripted
                        ? sim::scriptedInput(r.rng, c.world(), c.matchContent(), id)
                        : sim::PlayerInput{};
    }
    c.tick(std::span<const sim::PlayerInput>(inputs.data(), k));
  }
  static_cast<void>(c.drainEvents());
  if (afterClientFrame) afterClientFrame(r);
}

uint32_t percentile(std::span<const uint64_t> hist, uint32_t p) {
  uint64_t total = 0;
  for (const uint64_t n : hist)
    total += n;
  if (total == 0) return 0;
  const uint64_t want = (total * p + 99) / 100;
  uint64_t seen = 0;
  for (size_t d = 0; d < hist.size(); ++d) {
    seen += hist[d];
    if (seen >= want) return static_cast<uint32_t>(d);
  }
  return static_cast<uint32_t>(hist.size() - 1);
}

uint32_t maxBucket(std::span<const uint64_t> hist) {
  for (size_t d = hist.size(); d > 0; --d) {
    if (hist[d - 1] != 0) return static_cast<uint32_t>(d - 1);
  }
  return 0;
}

void LeadSampler::sample(const NetSim& /*s*/, const ClientRig& r, Tick serverNextTick) {
  if (!r.client->running() || r.client->spectator()) return;
  leadSum += r.client->localTick().minus(serverNextTick);
  targetSum += r.client->netStats().targetLeadTicks;
  ++samples;
}

Report summarise(const NetSim& s, double seconds, const LeadSampler& lead, uint64_t substAfter,
                 double substSeconds) {
  Report r;
  std::array<uint64_t, kRollbackDepthBuckets> hist{};
  uint64_t mispredicted = 0;
  uint64_t rollbacks = 0;
  uint64_t up = 0;
  uint64_t down = 0;
  for (const ClientRig& rig : s.rigs()) {
    const ClientNetStats& st = rig.client->netStats();
    r.mismatches += st.checksumMismatches;
    r.resyncs += st.resyncRequests;
    r.compared += st.checksumsCompared;
    r.overflows += st.rollbackOverflows;
    r.hardJumps += st.hardJumps;
    r.snapshotsLoaded += st.snapshotsLoaded;
    if (rig.client->spectator()) continue;
    ++r.clients;
    for (size_t d = 0; d < hist.size(); ++d)
      hist[d] += st.rollbackDepth[d];
    r.maxDepth = std::max(r.maxDepth, st.maxRollbackDepth);
    mispredicted += st.mispredictedTicks;
    rollbacks += st.rollbacks;
    up += st.bytesSent;
    down += st.bytesReceived;
    r.anchorRollbacks += st.anchorRollbacks;
  }
  const double n = r.clients > 0 ? static_cast<double>(r.clients) : 1.0;
  r.p99Depth = percentile(hist, 99);
  r.p50Depth = percentile(hist, 50);
  r.mispredictionsPerSec = static_cast<double>(mispredicted) / seconds / n;
  r.rollbacksPerSec = static_cast<double>(rollbacks) / seconds / n;
  r.upBytesPerSec = static_cast<double>(up) / seconds / n;
  r.downBytesPerSec = static_cast<double>(down) / seconds / n;
  if (lead.samples > 0) {
    r.meanLead = lead.leadSum / static_cast<double>(lead.samples);
    r.meanTarget = lead.targetSum / static_cast<double>(lead.samples);
  }
  if (substSeconds > 0) {
    r.serverSubstPerSec =
        static_cast<double>(s.server().stats().substitutedInputs - substAfter) / substSeconds / n;
  }
  return r;
}

std::string format(const char* name, const Report& r, double wallSeconds) {
  std::array<char, 1024> buf{};
  const int len = std::snprintf(
      buf.data(), buf.size(),
      "[netcode] %s: rollback depth max %u p99 %u p50 %u; mispredictions/s %.1f, rollbacks/s %.1f "
      "per client; up %.0f B/s, down %.0f B/s per client; checksums compared %llu, mismatches "
      "%llu, resyncs %llu, overflows %llu, hard jumps %llu, rollbacks past the ring %llu; lead "
      "%.2f ticks (target %.2f); server substitutions/s per client %.3f; wall %.2f s",
      name, r.maxDepth, r.p99Depth, r.p50Depth, r.mispredictionsPerSec, r.rollbacksPerSec,
      r.upBytesPerSec, r.downBytesPerSec, static_cast<unsigned long long>(r.compared),
      static_cast<unsigned long long>(r.mismatches), static_cast<unsigned long long>(r.resyncs),
      static_cast<unsigned long long>(r.overflows), static_cast<unsigned long long>(r.hardJumps),
      static_cast<unsigned long long>(r.anchorRollbacks), r.meanLead, r.meanTarget,
      r.serverSubstPerSec, wallSeconds);
  if (len <= 0) return {};
  return {buf.data(), std::min<size_t>(static_cast<size_t>(len), buf.size() - 1)};
}

}  // namespace kfm::net::netsim
