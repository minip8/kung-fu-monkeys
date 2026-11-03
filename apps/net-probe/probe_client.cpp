// The probe client, see probe_client.hpp.
#include "probe_client.hpp"

#include "kfm/net/transport.hpp"
#include "kfm/net/webrtc_transport.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace kfm::netprobe {
namespace {

constexpr uint64_t kNsPerMs = 1'000'000;
constexpr uint64_t kPingTimeoutNs = 250 * kNsPerMs;

// Sizes cycled through per lane; sequence 0 is the lane maximum.
constexpr std::array<size_t, 7> kUnreliableSizes = {
    net::kMaxUnreliableBytes, kHeaderBytes, 32, 100, 256, 600, 1000};
constexpr std::array<size_t, 7> kReliableSizes = {
    net::kMaxReliableBytes, kHeaderBytes, 64, 512, 1200, 4096, 16384};

[[nodiscard]] constexpr size_t laneIndex(net::Lane lane) {
  return static_cast<size_t>(lane);
}

[[nodiscard]] size_t sizeFor(net::Lane lane, uint32_t seq) {
  const auto& sizes = lane == net::Lane::Reliable ? kReliableSizes : kUnreliableSizes;
  return sizes[seq % sizes.size()];
}

[[nodiscard]] std::byte fill(net::Lane lane, uint32_t seq, size_t offset) {
  return static_cast<std::byte>((laneIndex(lane) * 131U + size_t{seq} * 31U + offset * 7U) & 0xFFU);
}

void putU32(std::span<std::byte> out, uint32_t v) {
  for (size_t i = 0; i < 4; ++i)
    out[i] = static_cast<std::byte>((v >> (8U * i)) & 0xFFU);
}

void putU64(std::span<std::byte> out, uint64_t v) {
  for (size_t i = 0; i < 8; ++i)
    out[i] = static_cast<std::byte>((v >> (8U * i)) & 0xFFU);
}

[[nodiscard]] uint32_t getU32(std::span<const std::byte> in) {
  uint32_t v = 0;
  for (size_t i = 0; i < 4; ++i)
    v |= static_cast<uint32_t>(in[i]) << (8U * i);
  return v;
}

[[nodiscard]] uint64_t getU64(std::span<const std::byte> in) {
  uint64_t v = 0;
  for (size_t i = 0; i < 8; ++i)
    v |= static_cast<uint64_t>(in[i]) << (8U * i);
  return v;
}

[[nodiscard]] std::vector<std::byte> makeMessage(std::byte laneByte, net::Lane lane, uint32_t seq,
                                                 uint64_t nowNs, size_t size) {
  // Never shorter than the header, and written through an explicitly checked
  // pointer: GCC 15 in Release otherwise reports a potential null dereference
  // on the vector's storage under -Werror.
  std::vector<std::byte> m(std::max(size, kHeaderBytes));
  std::byte* const p = m.data();
  if (p == nullptr) return m;  // cannot happen: the size is at least kHeaderBytes
  p[0] = kClientTag;
  p[1] = laneByte;
  putU32(std::span<std::byte>(p + 2, 4), seq);
  putU64(std::span<std::byte>(p + 6, 8), nowNs);
  for (size_t i = kHeaderBytes; i < m.size(); ++i)
    p[i] = fill(lane, seq, i);
  return m;
}

[[nodiscard]] double ms(uint64_t ns) {
  return static_cast<double>(ns) / static_cast<double>(kNsPerMs);
}

// "min/median/p95/max" in ms, or "-" without samples.
[[nodiscard]] std::string rttSummary(std::vector<uint64_t> samples) {
  if (samples.empty()) return "-";
  std::ranges::sort(samples);
  const auto at = [&](double q) {
    const auto i = static_cast<size_t>(q * static_cast<double>(samples.size()));
    return samples[std::min(i, samples.size() - 1)];
  };
  std::array<char, 96> buf{};
  (void)std::snprintf(buf.data(), buf.size(), "%.2f/%.2f/%.2f/%.2f", ms(samples.front()),
                      ms(at(0.5)), ms(at(0.95)), ms(samples.back()));
  return buf.data();
}

}  // namespace

std::vector<std::byte> makeEcho(std::span<const std::byte> message) {
  if (message.size() < kHeaderBytes || message[0] != kClientTag) return {};
  std::vector<std::byte> echo(message.begin(), message.end());
  echo[0] = kEchoTag;
  return echo;
}

std::expected<std::unique_ptr<ProbeClient>, std::string> ProbeClient::create(
    const ProbeConfig& config) {
  net::WebRtcClientConfig clientConfig;
  clientConfig.signalingUrl = config.url;
  clientConfig.connectTimeoutMs = config.timeoutMs;
  auto transport = net::WebRtcTransport::createClient(clientConfig);
  if (!transport) return std::unexpected(transport.error());
  return std::make_unique<ProbeClient>(config, std::move(*transport));
}

ProbeClient::ProbeClient(ProbeConfig config, std::unique_ptr<net::WebRtcTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport)) {
  for (auto& seen : seen_)
    seen.assign(config_.messages, false);
}

bool ProbeClient::tick(uint64_t nowNs) {
  if (done()) return true;
  if (startNs_ == 0) startNs_ = nowNs;
  net::ConnectionEvents events;
  transport_->poll(nowNs, events);
  for (const auto& e : events.span()) {
    if (e.kind == net::ConnectionEventKind::Connected && state_ == ProbeState::Connecting) {
      connectedNs_ = nowNs;
      state_ = config_.pings > 0 ? ProbeState::Pinging : ProbeState::Sending;
      std::printf("connected to %s in %.1f ms\n", config_.url.c_str(), ms(nowNs - startNs_));
      // One oversize message per lane, which the transport must refuse.
      const std::vector<std::byte> tooBigU(net::kMaxUnreliableBytes + 1);
      const std::vector<std::byte> tooBigR(net::kMaxReliableBytes + 1);
      transport_->sendUnreliable(net::kServerPeer, tooBigU);
      transport_->sendReliable(net::kServerPeer, tooBigR);
      expectedSendErrors_ += 2;
    } else if (e.kind == net::ConnectionEventKind::Disconnected) {
      receive();
      finish(nowNs, state_ == ProbeState::Connecting ? "could not connect" : "disconnected");
      return true;
    }
  }
  receive();
  if (state_ == ProbeState::Pinging) ping(nowNs);
  if (state_ == ProbeState::Sending) send(nowNs);

  const auto& u = lanes_[laneIndex(net::Lane::Unreliable)];
  const auto& r = lanes_[laneIndex(net::Lane::Reliable)];
  if (state_ == ProbeState::Draining) {
    const bool reliableDone = r.echoed + r.corrupt >= r.sent;
    const bool allDone = reliableDone && u.echoed + u.corrupt >= u.sent;
    if (allDone || (reliableDone && nowNs - lastSendNs_ >= config_.drainMs * kNsPerMs)) {
      finish(nowNs, nullptr);
      return true;
    }
  }
  if (nowNs - startNs_ >= uint64_t{config_.timeoutMs} * kNsPerMs) {
    finish(nowNs, "timed out");
    return true;
  }
  return false;
}

void ProbeClient::ping(uint64_t nowNs) {
  if (pingPending_ && nowNs - pingSentNs_ < kPingTimeoutNs) return;
  if (pingPending_) {
    ++pingsLost_;
    pingPending_ = false;
  }
  if (pingsSent_ == config_.pings) {
    state_ = ProbeState::Sending;
    std::printf("idle rtt over %u pings (ms, min/med/p95/max): %s, %u lost\n", pingsSent_,
                rttSummary(pingRttNs_).c_str(), pingsLost_);
    return;
  }
  const auto message =
      makeMessage(kPingLane, net::Lane::Unreliable, pingsSent_, nowNs, kHeaderBytes);
  transport_->sendUnreliable(net::kServerPeer, message);
  ++pingsSent_;
  pingPending_ = true;
  pingSentNs_ = nowNs;
}

void ProbeClient::send(uint64_t nowNs) {
  for (const net::Lane lane : {net::Lane::Unreliable, net::Lane::Reliable}) {
    auto& result = lanes_[laneIndex(lane)];
    for (uint32_t i = 0; i < config_.sendsPerTick && result.sent < config_.messages; ++i) {
      const uint32_t seq = result.sent;
      const auto message = makeMessage(static_cast<std::byte>(laneIndex(lane)), lane, seq, nowNs,
                                       sizeFor(lane, seq));
      const uint64_t errorsBefore = transport_->stats().sendErrors;
      if (lane == net::Lane::Reliable) {
        transport_->sendReliable(net::kServerPeer, message);
      } else {
        transport_->sendUnreliable(net::kServerPeer, message);
      }
      // A refused send (the unreliable lane's buffer is full) still counts
      // as sent: its echo is simply missing, like a lost datagram.
      if (transport_->stats().sendErrors != errorsBefore) ++expectedSendErrors_;
      ++result.sent;
      result.bytesSent += message.size();
      result.maxSize = std::max(result.maxSize, message.size());
    }
  }
  const auto& u = lanes_[laneIndex(net::Lane::Unreliable)];
  const auto& r = lanes_[laneIndex(net::Lane::Reliable)];
  if (u.sent == config_.messages && r.sent == config_.messages) {
    state_ = ProbeState::Draining;
    lastSendNs_ = nowNs;
    std::printf("sent %u unreliable + %u reliable messages (%llu + %llu bytes) in %.1f ms\n",
                u.sent, r.sent, static_cast<unsigned long long>(u.bytesSent),
                static_cast<unsigned long long>(r.bytesSent), ms(nowNs - connectedNs_));
  }
}

void ProbeClient::receive() {
  net::Packet packet;
  while (transport_->recv(packet))
    check(packet);
}

void ProbeClient::check(const net::Packet& packet) {
  auto& result = lanes_[laneIndex(packet.lane)];
  const auto data = packet.data;
  if (packet.lane == net::Lane::Unreliable && data.size() == kHeaderBytes && data[0] == kEchoTag &&
      data[1] == kPingLane) {
    // Only the outstanding ping counts; a late one was already counted lost.
    if (pingPending_ && getU32(data.subspan(2, 4)) + 1 == pingsSent_) {
      pingRttNs_.push_back(packet.receivedNs - std::min(packet.receivedNs, pingSentNs_));
      pingPending_ = false;
    }
    return;
  }
  if (packet.peer != net::kServerPeer || data.size() < kHeaderBytes || data[0] != kEchoTag ||
      data[1] != static_cast<std::byte>(laneIndex(packet.lane))) {
    ++result.corrupt;
    return;
  }
  const uint32_t seq = getU32(data.subspan(2, 4));
  const uint64_t sentNs = getU64(data.subspan(6, 8));
  if (seq >= result.sent || data.size() != sizeFor(packet.lane, seq)) {
    ++result.corrupt;
    return;
  }
  for (size_t i = kHeaderBytes; i < data.size(); ++i) {
    if (data[i] != fill(packet.lane, seq, i)) {
      ++result.corrupt;
      return;
    }
  }
  auto& seen = seen_[laneIndex(packet.lane)];
  if (seen[seq]) {
    ++result.duplicates;
    return;
  }
  seen[seq] = true;
  if (packet.lane == net::Lane::Reliable) {
    if (static_cast<int64_t>(seq) != lastReliableSeq_ + 1) ++result.outOfOrder;
    lastReliableSeq_ = static_cast<int64_t>(seq);
  }
  ++result.echoed;
  result.bytesEchoed += data.size();
  result.rttNs.push_back(packet.receivedNs > sentNs ? packet.receivedNs - sentNs : 0);
}

void ProbeClient::finish(uint64_t nowNs, const char* reason) {
  const auto& u = lanes_[laneIndex(net::Lane::Unreliable)];
  const auto& r = lanes_[laneIndex(net::Lane::Reliable)];
  const net::TransportStats stats = transport_->stats();
  std::string failure = reason != nullptr ? reason : "";
  const auto require = [&](bool ok, const char* what) {
    if (!ok && failure.empty()) failure = what;
  };
  require(r.sent == config_.messages && r.echoed == r.sent, "reliable echoes missing");
  require(r.outOfOrder == 0, "reliable echoes out of order");
  require(r.duplicates == 0, "reliable echoes duplicated");
  require(u.corrupt == 0 && r.corrupt == 0, "corrupt echoes");
  require(u.sent == config_.messages &&
              uint64_t{u.echoed} * 100U >= uint64_t{u.sent} * config_.minUnreliablePercent,
          "too many unreliable echoes lost");
  require(stats.sendErrors == expectedSendErrors_, "unexpected send errors");
  require(!pingRttNs_.empty() || config_.pings == 0, "no ping came back");
  state_ = failure.empty() ? ProbeState::Passed : ProbeState::Failed;

  std::array<char, 768> buf{};
  (void)std::snprintf(
      buf.data(), buf.size(),
      "KFM_NET_PROBE %s%s%s connect_ms=%.1f rtt_idle_ms(min/med/p95/max)=%s pings_lost=%u/%u "
      "unreliable=%u/%u (max %zu B, %u dup) "
      "reliable=%u/%u (max %zu B, %u out of order) corrupt=%u bytes_echoed=%llu "
      "rtt_bulk_unreliable_ms=%s rtt_bulk_reliable_ms=%s send_errors=%llu/%llu "
      "elapsed_ms=%.1f",
      passed() ? "PASS" : "FAIL", failure.empty() ? "" : ": ", failure.c_str(),
      connectedNs_ != 0 ? ms(connectedNs_ - startNs_) : -1.0, rttSummary(pingRttNs_).c_str(),
      pingsLost_, pingsSent_, u.echoed, u.sent, u.maxSize, u.duplicates, r.echoed, r.sent,
      r.maxSize, r.outOfOrder, u.corrupt + r.corrupt,
      static_cast<unsigned long long>(u.bytesEchoed) + r.bytesEchoed, rttSummary(u.rttNs).c_str(),
      rttSummary(r.rttNs).c_str(), static_cast<unsigned long long>(stats.sendErrors),
      static_cast<unsigned long long>(expectedSendErrors_), ms(nowNs - startNs_));
  summary_ = buf.data();
  if (!passed() && !transport_->lastError().empty())
    summary_ += " last_error=\"" + transport_->lastError() + "\"";
}

}  // namespace kfm::netprobe
