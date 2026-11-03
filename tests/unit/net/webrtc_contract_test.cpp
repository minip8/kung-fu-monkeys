// The transport contract suite (architecture §6.4) over WebRtcTransport on
// 127.0.0.1: real libdatachannel, a real monotonic clock, host candidates
// only. Every case builds a fresh server on an ephemeral signaling port and
// one client that has started connecting. Label `network` (CMake).
#include "kfm/net/transport.hpp"
#include "kfm/net/webrtc_transport.hpp"
#include "net/transport_contract.hpp"
#include "net/webrtc_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

using namespace kfm::net;

namespace {

class WebRtcHarness final : public contract::Harness {
 public:
  WebRtcHarness() {
    INFO("server: " << fixture_.error());
    REQUIRE(fixture_.ok());
    client_ = startClient();
  }

  ITransport& server() override { return fixture_.server(); }
  ITransport& client() override { return *client_; }
  ITransport* addClient() override { return startClient(); }
  uint64_t nowNs() override { return test::WebRtcFixture::nowNs(); }
  // The transport works on its own threads; the suite polls after this.
  void advance() override { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  [[nodiscard]] uint64_t timeoutNs() const override {
    return static_cast<uint64_t>(
        std::chrono::nanoseconds(test::WebRtcFixture::kDefaultTimeout).count());
  }

 private:
  ITransport* startClient() {
    const size_t index = fixture_.addClient();
    INFO("client: " << fixture_.error());
    REQUIRE(index != 0);
    return &fixture_.endpoint(index);
  }

  // Only creates the endpoints: the suite's Pump polls and drains them.
  test::WebRtcFixture fixture_;
  ITransport* client_{nullptr};
};

}  // namespace

KFM_TRANSPORT_CONTRACT_TESTS("WebRtc", "[net][webrtc][contract]",
                             [] { return std::make_unique<WebRtcHarness>(); })
