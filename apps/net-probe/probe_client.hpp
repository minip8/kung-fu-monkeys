// The client side of the WebRTC probe (architecture §6.4), shared by the
// native kfm_net_probe --connect and the browser kfm_net_probe.html: connect
// to an echo server, send N unreliable and N reliable messages of varied
// sizes (the lane maximum included), verify every echo byte for byte, and
// summarise counts and round-trip times in one line.
//
// Wire format of a probe message (little endian):
//
//   [0]      kClientTag (client -> server) or kEchoTag (server -> client)
//   [1]      lane (0 unreliable, 1 reliable), or 2 for a ping (unreliable lane)
//   [2..5]   sequence number, per lane, from 0
//   [6..13]  send time on the client's clock (ns)
//   [14..]   fill bytes, a function of (lane, sequence, offset)
//
// The echo server replaces the first byte with kEchoTag and sends the rest
// back unchanged on the same lane, so an echo is exactly as long as the
// message: a message at the lane maximum is exercised in both directions.
//
// The probe first measures an idle round-trip time with pings: one small
// message on the unreliable lane at a time, the next once its echo is back
// (or after kPingTimeoutMs). The bulk phase then sends both lanes at
// sendsPerTick per lane, whose round-trip times include queueing behind
// the 64 KB reliable messages sharing the SCTP association.
//
// Pass criteria: every reliable echo arrives, in order and intact; at least
// minUnreliablePercent of the unreliable echoes arrive intact (loopback
// loses none in practice, but the lane is allowed to); no echo is corrupt,
// duplicated beyond what the lane allows, or from the wrong lane; one
// oversize send per lane is refused locally (stats().sendErrors).
//
// Headless and single-threaded: tick() does one poll + drain + send step and
// is driven by a plain loop natively and by the browser's main loop.
#pragma once

#include "kfm/net/transport.hpp"
#include "kfm/net/webrtc_transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace kfm::netprobe {

inline constexpr std::byte kClientTag{0xC1};
inline constexpr std::byte kEchoTag{0xEC};
inline constexpr size_t kHeaderBytes = 14;

// The echo the server sends for `message` (empty if it is not a probe message).
[[nodiscard]] std::vector<std::byte> makeEcho(std::span<const std::byte> message);

inline constexpr std::byte kPingLane{2};

struct ProbeConfig {
  std::string url;         // ws://host:port
  uint32_t messages{200};  // per lane
  uint32_t pings{20};
  uint32_t sendsPerTick{4};           // per lane
  uint32_t timeoutMs{30'000};         // whole probe, connect included
  uint32_t drainMs{1'000};            // wait for late unreliable echoes after the last send
  uint32_t minUnreliablePercent{90};  // of the unreliable echoes
};

struct LaneResult {
  uint32_t sent{0};
  uint32_t echoed{0};  // distinct sequences echoed intact
  uint32_t duplicates{0};
  uint32_t corrupt{0};     // wrong length, tag, lane, fill or unknown sequence
  uint32_t outOfOrder{0};  // reliable lane only
  uint64_t bytesSent{0};
  uint64_t bytesEchoed{0};
  size_t maxSize{0};  // largest message sent
  std::vector<uint64_t> rttNs;
};

enum class ProbeState : uint8_t { Connecting = 0, Pinging, Sending, Draining, Passed, Failed };

class ProbeClient {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<ProbeClient>, std::string> create(
      const ProbeConfig& config);

  ProbeClient(ProbeConfig config, std::unique_ptr<net::WebRtcTransport> transport);

  // One step at nowNs (a monotonic clock in ns). Returns true once finished.
  [[nodiscard]] bool tick(uint64_t nowNs);

  [[nodiscard]] ProbeState state() const { return state_; }
  [[nodiscard]] bool done() const {
    return state_ == ProbeState::Passed || state_ == ProbeState::Failed;
  }
  [[nodiscard]] bool passed() const { return state_ == ProbeState::Passed; }
  // "KFM_NET_PROBE PASS ..." / "KFM_NET_PROBE FAIL ...", once done().
  [[nodiscard]] const std::string& summary() const { return summary_; }

  // Closes the connection (the server sees a Disconnected).
  void close() { transport_.reset(); }

 private:
  void ping(uint64_t nowNs);
  void send(uint64_t nowNs);
  void receive();
  void check(const net::Packet& packet);
  void finish(uint64_t nowNs, const char* reason);

  ProbeConfig config_;
  std::unique_ptr<net::WebRtcTransport> transport_;
  ProbeState state_{ProbeState::Connecting};
  uint64_t startNs_{0};
  uint64_t connectedNs_{0};
  uint64_t lastSendNs_{0};
  std::array<LaneResult, 2> lanes_{};
  uint32_t pingsSent_{0};
  uint32_t pingsLost_{0};
  bool pingPending_{false};
  uint64_t pingSentNs_{0};
  std::vector<uint64_t> pingRttNs_;
  std::array<std::vector<bool>, 2> seen_{};
  int64_t lastReliableSeq_{-1};
  uint64_t expectedSendErrors_{0};
  std::string summary_;
};

}  // namespace kfm::netprobe
