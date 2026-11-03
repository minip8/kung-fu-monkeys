// WebRtcTransport paths that need no network, in both builds. Under the wasm
// preset this is what proves the browser transport (datachannel-wasm) links
// with its --js-library files and wasm exceptions; connecting needs a
// browser's RTCPeerConnection, so that is checked in a browser, not here.
#include "kfm/net/transport.hpp"
#include "kfm/net/webrtc_transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace kfm::net;

TEST_CASE("WebRtcTransport: a non-ws URL is an error, not a throw", "[net][webrtc]") {
  WebRtcClientConfig config;
  config.signalingUrl = "http://127.0.0.1:1";
  const auto client = WebRtcTransport::createClient(config);
  REQUIRE_FALSE(client.has_value());
  CHECK(client.error().find("ws://") != std::string::npos);
}

#ifdef __EMSCRIPTEN__
TEST_CASE("WebRtcTransport: the server role is native only", "[net][webrtc]") {
  const auto server = WebRtcTransport::createServer(WebRtcServerConfig{});
  REQUIRE_FALSE(server.has_value());
  CHECK(server.error().find("native only") != std::string::npos);
}
#else
TEST_CASE("WebRtcTransport: a taken signaling port is an error, not a throw", "[net][webrtc]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  WebRtcServerConfig config;
  config.signalingBindAddress = "127.0.0.1";
  const auto first = WebRtcTransport::createServer(config);
  REQUIRE(first.has_value());
  config.signalingPort = (*first)->listenPort();
  const auto second = WebRtcTransport::createServer(config);
  CHECK_FALSE(second.has_value());
}
#endif
