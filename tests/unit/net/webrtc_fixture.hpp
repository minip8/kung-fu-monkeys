// A server + N clients of WebRtcTransport on 127.0.0.1 with a real
// monotonic clock, shaped so the transport contract suite can instantiate
// it like the loopback fixture: endpoint(i) is an ITransport (0 = server),
// pump() polls every endpoint once and collects its events and packets,
// waitUntil() pumps until a predicate holds or a deadline passes.
#pragma once

#include "kfm/net/transport.hpp"
#include "kfm/net/webrtc_transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kfm::net::test {

struct ReceivedPacket {
  PeerId peer{PeerId::invalid()};
  Lane lane{Lane::Unreliable};
  uint64_t receivedNs{0};
  std::vector<std::byte> data;
};

// Everything one endpoint saw since the fixture started (or clear()).
struct Inbox {
  std::vector<ConnectionEvent> events;
  std::vector<ReceivedPacket> packets;
  size_t eventsDropped{0};

  void clear() {
    events.clear();
    packets.clear();
    eventsDropped = 0;
  }
  [[nodiscard]] size_t count(ConnectionEventKind kind) const {
    size_t n = 0;
    for (const auto& e : events)
      if (e.kind == kind) ++n;
    return n;
  }
  [[nodiscard]] PeerId lastPeer(ConnectionEventKind kind) const {
    for (const auto& e : std::views::reverse(events))
      if (e.kind == kind) return e.peer;
    return PeerId::invalid();
  }
};

class WebRtcFixture {
 public:
  // Generous: ICE on loopback completes in well under a second, but CI
  // machines under load (sanitizers, parallel ctest) can be slow.
  static constexpr std::chrono::milliseconds kDefaultTimeout{20'000};

  WebRtcFixture() : WebRtcFixture(WebRtcServerConfig{}) {}

  // The server with `config`, its signaling socket on an ephemeral port.
  explicit WebRtcFixture(WebRtcServerConfig config) {
    // Expected teardown noise (TCP lost, SCTP reset) is logged at Warning.
    WebRtcTransport::initLogging(WebRtcLogLevel::Error);
    config.signalingPort = 0;  // ephemeral, no clashes between parallel runs
    config.signalingBindAddress = "127.0.0.1";
    auto server = WebRtcTransport::createServer(config);
    if (server) {
      server_ = std::move(*server);
      endpoints_.push_back(server_.get());
      inboxes_.emplace_back();
    } else {
      error_ = server.error();
    }
  }

  [[nodiscard]] bool ok() const { return server_ != nullptr; }
  [[nodiscard]] const std::string& error() const { return error_; }
  [[nodiscard]] std::string url() const {
    return "ws://127.0.0.1:" + std::to_string(server_->listenPort());
  }

  [[nodiscard]] static uint64_t nowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
  }

  // Adds a client endpoint (index returned); it starts connecting at once.
  [[nodiscard]] size_t addClient(std::vector<std::byte> token = {}) {
    WebRtcClientConfig config;
    config.signalingUrl = url();
    config.connectionToken = std::move(token);
    auto client = WebRtcTransport::createClient(config);
    if (!client) {
      error_ = client.error();
      return 0;
    }
    clients_.push_back(std::move(*client));
    endpoints_.push_back(clients_.back().get());
    inboxes_.emplace_back();
    return endpoints_.size() - 1;
  }

  // Drops a client endpoint (its destructor closes the connection).
  void removeClient(size_t index) {
    const auto* t = endpoints_.at(index);
    for (auto& c : clients_)
      if (c.get() == t) c.reset();
    endpoints_[index] = nullptr;
  }

  [[nodiscard]] ITransport& endpoint(size_t index) { return *endpoints_.at(index); }
  [[nodiscard]] WebRtcTransport& server() { return *server_; }
  [[nodiscard]] WebRtcTransport& client(size_t index) { return *endpoints_.at(index); }
  [[nodiscard]] Inbox& inbox(size_t index) { return inboxes_.at(index); }
  [[nodiscard]] size_t endpointCount() const { return endpoints_.size(); }

  // One poll() + drain of every live endpoint.
  void pump() {
    const uint64_t now = nowNs();
    for (size_t i = 0; i < endpoints_.size(); ++i) {
      if (endpoints_[i] == nullptr) continue;
      ConnectionEvents events;
      endpoints_[i]->poll(now, events);
      for (const auto& e : events.span())
        inboxes_[i].events.push_back(e);
      inboxes_[i].eventsDropped += events.dropped;
      Packet p;
      while (endpoints_[i]->recv(p))
        inboxes_[i].packets.push_back(
            {p.peer, p.lane, p.receivedNs, std::vector<std::byte>(p.data.begin(), p.data.end())});
    }
  }

  template <typename Pred>
  [[nodiscard]] bool waitUntil(Pred&& pred, std::chrono::milliseconds timeout = kDefaultTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      pump();
      if (pred()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    pump();
    return pred();
  }

  // Pumps for a fixed time (to check that something does NOT happen).
  void pumpFor(std::chrono::milliseconds duration) {
    (void)waitUntil([] { return false; }, duration);
  }

 private:
  std::unique_ptr<WebRtcTransport> server_;
  std::vector<std::unique_ptr<WebRtcTransport>> clients_;
  std::vector<WebRtcTransport*> endpoints_;  // 0 = server, then clients in order
  std::vector<Inbox> inboxes_;
  std::string error_;
};

[[nodiscard]] inline std::vector<std::byte> bytesOf(std::string_view s) {
  std::vector<std::byte> out;
  out.reserve(s.size());
  for (const char c : s)
    out.push_back(static_cast<std::byte>(c));
  return out;
}

}  // namespace kfm::net::test
