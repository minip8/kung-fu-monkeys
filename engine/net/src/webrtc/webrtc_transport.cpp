// WebRtcTransport over libdatachannel (native) and datachannel-wasm (browser),
// see kfm/net/webrtc_transport.hpp for the contract. Every entry point
// catches at the boundary: the rtc:: API throws, our callers are built
// without exceptions.
#include "kfm/net/webrtc_transport.hpp"

#include "kfm/net/transport.hpp"
#include "kfm/net/webrtc_signaling.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <rtc/rtc.hpp>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace kfm::net {
namespace {

constexpr const char* kUnreliableLabel = "u";
constexpr const char* kReliableLabel = "r";

// Readable messages held for recv() plus those queued by callbacks; beyond
// this they are dropped and counted (stats().recvDropped).
constexpr size_t kMaxQueuedPackets = 8192;
#ifndef __EMSCRIPTEN__
// Local SCTP maximum message size advertised in the SDP (a=max-message-size).
// The reliable lane carries kMaxReliableBytes; browsers advertise 256 KB
// themselves (Chromium, Firefox), so only libdatachannel needs telling.
constexpr size_t kSctpMaxMessageBytes = size_t{256} * 1024;
static_assert(kSctpMaxMessageBytes >= kMaxReliableBytes);
#endif
// The unreliable lane never blocks and never queues up stale inputs: once
// this much is waiting in the SCTP send buffer, further sends are dropped.
constexpr size_t kMaxUnreliableBuffered = size_t{64} * 1024;

constexpr uint64_t kNsPerMs = 1'000'000;

[[nodiscard]] uint64_t steadyNowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

[[nodiscard]] constexpr size_t laneIndex(Lane lane) {
  return static_cast<size_t>(lane);
}

// What a callback thread hands to poll().
enum class CallbackKind : uint8_t {
  NewClient = 0,   // server: a signaling WebSocket arrived (ws set)
  SignalOpen,      // the signaling WebSocket is open
  SignalClosed,    // ... closed or failed
  SignalText,      // a signaling message (text)
  ChannelArrived,  // server: the remote opened a data channel (dc set)
  ChannelOpen,     // a lane's channel is open
  ChannelClosed,   // ... closed or failed
  PeerState,       // the peer connection changed state (state)
  Message,         // a data channel message (bytes)
};

struct CallbackEvent {
  CallbackKind kind{CallbackKind::Message};
  PeerId peer{PeerId::invalid()};
  Lane lane{Lane::Unreliable};
  rtc::PeerConnection::State state{rtc::PeerConnection::State::New};
  uint64_t steadyNs{0};
  std::string text;
  std::vector<std::byte> bytes;
  std::shared_ptr<rtc::WebSocket> ws;
  std::shared_ptr<rtc::DataChannel> dc;
};

// The only state callbacks touch. Captured weakly, so a callback that runs
// after the transport is gone does nothing.
struct CallbackQueue {
  std::mutex mutex;
  std::vector<CallbackEvent> events;
  size_t queuedMessages{0};
  uint64_t droppedMessages{0};
  uint16_t nextPeerId{1};  // server: PeerIds handed out by onClient

  void push(CallbackEvent&& e) {
    const std::scoped_lock lock(mutex);
    if (e.kind == CallbackKind::Message) {
      if (queuedMessages >= kMaxQueuedPackets) {
        ++droppedMessages;
        return;
      }
      ++queuedMessages;
    }
    events.push_back(std::move(e));
  }
};

using WeakQueue = std::weak_ptr<CallbackQueue>;

void post(const WeakQueue& weak, CallbackEvent&& e) {
  if (const auto q = weak.lock()) q->push(std::move(e));
}

void postSimple(const WeakQueue& weak, CallbackKind kind, PeerId peer,
                Lane lane = Lane::Unreliable) {
  CallbackEvent e;
  e.kind = kind;
  e.peer = peer;
  e.lane = lane;
  post(weak, std::move(e));
}

void sendSignal(const std::weak_ptr<rtc::WebSocket>& weakWs, const signaling::Message& m) {
  try {
    if (const auto ws = weakWs.lock(); ws && ws->isOpen()) ws->send(signaling::encode(m));
  } catch (...) {  // NOLINT(bugprone-empty-catch): the socket closed under us; the peer times out
  }
}

void wireSignalingSocket(rtc::WebSocket& ws, const WeakQueue& weak, PeerId peer) {
  ws.onOpen([weak, peer] { postSimple(weak, CallbackKind::SignalOpen, peer); });
  ws.onClosed([weak, peer] { postSimple(weak, CallbackKind::SignalClosed, peer); });
  ws.onError(
      [weak, peer](const std::string&) { postSimple(weak, CallbackKind::SignalClosed, peer); });
  ws.onMessage([weak, peer](rtc::message_variant message) {
    if (auto* text = std::get_if<std::string>(&message)) {
      if (text->size() > signaling::kMaxMessageBytes) return;
      CallbackEvent e;
      e.kind = CallbackKind::SignalText;
      e.peer = peer;
      e.text = std::move(*text);
      post(weak, std::move(e));
    }
  });
}

void wireChannel(rtc::DataChannel& dc, const WeakQueue& weak, PeerId peer, Lane lane) {
  dc.onOpen([weak, peer, lane] { postSimple(weak, CallbackKind::ChannelOpen, peer, lane); });
  dc.onClosed([weak, peer, lane] { postSimple(weak, CallbackKind::ChannelClosed, peer, lane); });
  dc.onError([weak, peer, lane](const std::string&) {
    postSimple(weak, CallbackKind::ChannelClosed, peer, lane);
  });
  dc.onMessage([weak, peer, lane](rtc::message_variant message) {
    CallbackEvent e;
    e.kind = CallbackKind::Message;
    e.peer = peer;
    e.lane = lane;
    e.steadyNs = steadyNowNs();
    if (auto* bin = std::get_if<rtc::binary>(&message)) {
      e.bytes = std::move(*bin);
    } else if (auto* text = std::get_if<std::string>(&message)) {
      const auto* p = reinterpret_cast<const std::byte*>(text->data());
      e.bytes.assign(p, p + text->size());
    }
    post(weak, std::move(e));
  });
}

[[nodiscard]] std::optional<Lane> laneForLabel(const std::string& label) {
  if (label == kUnreliableLabel) return Lane::Unreliable;
  if (label == kReliableLabel) return Lane::Reliable;
  return std::nullopt;
}

struct Received {
  PeerId peer{PeerId::invalid()};
  Lane lane{Lane::Unreliable};
  uint64_t receivedNs{0};
  std::vector<std::byte> bytes;
};

struct Peer {
  PeerId id{PeerId::invalid()};
  std::shared_ptr<rtc::WebSocket> ws;
  std::shared_ptr<rtc::PeerConnection> pc;
  std::array<std::shared_ptr<rtc::DataChannel>, 2> lanes{};
  std::array<bool, 2> laneOpen{};
  bool connected{false};
  std::optional<uint64_t> deadlineNs;  // caller clock; set on the first poll that sees the peer
  std::vector<Received> early;         // arrived before Connected, released with it
  // Remote candidates that arrived before the remote description: the
  // other side sends its description and its candidates from different
  // libdatachannel threads, so a candidate can overtake the SDP.
  std::vector<signaling::Message> earlyCandidates;
  bool remoteDescriptionSet{false};
};

// Upper bound on buffered early candidates (a peer gathers a handful).
constexpr size_t kMaxEarlyCandidates = 64;

std::once_flag gLoggerOnce;

void initLoggerOnce(WebRtcLogLevel level) {
  std::call_once(gLoggerOnce, [level] {
#ifndef __EMSCRIPTEN__
    try {
      rtc::InitLogger(static_cast<rtc::LogLevel>(level));
    } catch (...) {  // NOLINT(bugprone-empty-catch): logging is best effort
    }
#else
    (void)level;
#endif
  });
}

}  // namespace

struct WebRtcTransport::Impl {
  bool server{false};
  std::string stunServer;
  uint16_t icePortBegin{0};
  uint16_t icePortEnd{0};
  bool iceUdpMux{false};
  uint16_t maxPeers{kMaxPeers};
  uint64_t connectTimeoutNs{kDefaultWebRtcConnectTimeoutMs * kNsPerMs};
  std::vector<std::byte> token;
  uint16_t listenPort{0};

  std::shared_ptr<CallbackQueue> queue = std::make_shared<CallbackQueue>();
#ifndef __EMSCRIPTEN__
  std::shared_ptr<rtc::WebSocketServer> wsServer;
#endif
  std::map<uint16_t, Peer> peers;  // ordered: deterministic iteration
  std::vector<CallbackEvent> drained;
  std::deque<Received> inbox;
  std::vector<std::byte> current;  // backs the span recv() returned last
  std::vector<ConnectionEvent> localEvents;
  TransportStats stats;
  uint64_t lastPollNs{0};
  std::string lastError;

  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  ~Impl() {
    for (auto& [id, peer] : peers)
      closePeer(peer);
    peers.clear();
#ifndef __EMSCRIPTEN__
    try {
      if (wsServer) wsServer->stop();
    } catch (...) {  // NOLINT(bugprone-empty-catch): shutting down
    }
    wsServer.reset();
#endif
    // Release channels and sockets queued by callbacks here, on the
    // caller's thread, never on a libdatachannel thread.
    const std::scoped_lock lock(queue->mutex);
    queue->events.clear();
  }

  void fail(const char* where, const std::exception* e) {
    lastError = where;
    if (e != nullptr) {
      lastError += ": ";
      lastError += e->what();
    }
  }

  [[nodiscard]] rtc::Configuration peerConfig() const {
    rtc::Configuration config;
    if (!stunServer.empty()) config.iceServers.emplace_back(stunServer);
#ifndef __EMSCRIPTEN__
    config.maxMessageSize = kSctpMaxMessageBytes;
    if (icePortBegin != 0 || icePortEnd != 0) {
      config.portRangeBegin = icePortBegin;
      config.portRangeEnd = icePortEnd != 0 ? icePortEnd : icePortBegin;
    }
    config.enableIceUdpMux = iceUdpMux;
#endif
    return config;
  }

  static void closePeer(Peer& peer) {
    for (const auto& dc : peer.lanes) {
      try {
        if (dc) dc->close();
      } catch (...) {  // NOLINT(bugprone-empty-catch): already closed
      }
    }
    try {
      if (peer.pc) peer.pc->close();
    } catch (...) {  // NOLINT(bugprone-empty-catch): already closed
    }
    try {
      if (peer.ws) peer.ws->close();
    } catch (...) {  // NOLINT(bugprone-empty-catch): already closed
    }
    peer.lanes = {};
    peer.pc.reset();
    peer.ws.reset();
  }

  // Removes the peer; reports Disconnected if it was ever announced (or, on
  // a client, always: the caller is waiting for its one peer either way).
  void dropPeer(uint16_t id, uint64_t nowNs, ConnectionEvents* events) {
    const auto it = peers.find(id);
    if (it == peers.end()) return;
    const bool announce = it->second.connected || !server;
    closePeer(it->second);
    peers.erase(it);
    if (!announce) return;
    const ConnectionEvent e{ConnectionEventKind::Disconnected, PeerId{id}, nowNs};
    if (events != nullptr) {
      events->push(e);
    } else {
      localEvents.push_back(e);
    }
  }

  void createPeerConnection(Peer& peer) const {
    peer.pc = std::make_shared<rtc::PeerConnection>(peerConfig());
    const WeakQueue weak = queue;
    const PeerId id = peer.id;
    const std::weak_ptr<rtc::WebSocket> weakWs = peer.ws;
    peer.pc->onLocalDescription([weakWs](const rtc::Description& d) {
      signaling::Message m;
      m.type = d.typeString() == "answer" ? signaling::Type::Answer : signaling::Type::Offer;
      m.sdp = std::string(d);
      sendSignal(weakWs, m);
    });
    peer.pc->onLocalCandidate([weakWs](const rtc::Candidate& c) {
      signaling::Message m;
      m.type = signaling::Type::Candidate;
      m.candidate = c.candidate();
      m.mid = c.mid();
      sendSignal(weakWs, m);
    });
    peer.pc->onStateChange([weak, id](rtc::PeerConnection::State state) {
      CallbackEvent e;
      e.kind = CallbackKind::PeerState;
      e.peer = id;
      e.state = state;
      post(weak, std::move(e));
    });
    if (server) {
      peer.pc->onDataChannel([weak, id](const std::shared_ptr<rtc::DataChannel>& dc) {
        const auto lane = laneForLabel(dc->label());
        if (!lane) {
          dc->close();
          return;
        }
        // Wire it here, before any message can arrive; poll() adopts it.
        wireChannel(*dc, weak, id, *lane);
        CallbackEvent e;
        e.kind = CallbackKind::ChannelArrived;
        e.peer = id;
        e.lane = *lane;
        e.dc = dc;
        post(weak, std::move(e));
      });
    }
  }

  // Client: the signaling socket is open, so offer. Creating the first
  // channel starts negotiation on both platforms (libdatachannel's
  // auto-negotiation, the browser's negotiationneeded); the offer goes out
  // from onLocalDescription.
  void startClientOffer(Peer& peer) const {
    createPeerConnection(peer);
    const WeakQueue weak = queue;
    const rtc::DataChannelInit reliable;  // ordered, reliable by default
    rtc::DataChannelInit unreliable;
    unreliable.reliability.unordered = true;
    unreliable.reliability.maxRetransmits = 0U;
    auto r = peer.pc->createDataChannel(kReliableLabel, reliable);
    wireChannel(*r, weak, peer.id, Lane::Reliable);
    auto u = peer.pc->createDataChannel(kUnreliableLabel, unreliable);
    wireChannel(*u, weak, peer.id, Lane::Unreliable);
    peer.lanes[laneIndex(Lane::Reliable)] = std::move(r);
    peer.lanes[laneIndex(Lane::Unreliable)] = std::move(u);
  }

  void addCandidate(Peer& peer, const signaling::Message& m) {
    try {
      peer.pc->addRemoteCandidate(rtc::Candidate(m.candidate, m.mid));
    } catch (const std::exception& e) {
      fail("remote candidate", &e);  // one bad candidate is not fatal; ICE uses the others
    }
  }

  void setRemoteDescription(Peer& peer, const std::string& sdp, const char* type) {
    peer.pc->setRemoteDescription(rtc::Description(sdp, std::string(type)));
    peer.remoteDescriptionSet = true;
    for (const auto& c : peer.earlyCandidates)
      addCandidate(peer, c);
    peer.earlyCandidates.clear();
  }

  void handleSignal(Peer& peer, const std::string& text) {
    const auto m = signaling::decode(text);
    if (!m) {
      lastError = "malformed signaling message";
      return;
    }
    switch (m->type) {
      case signaling::Type::Offer:
        if (!server || peer.pc) return;  // one offer per peer, no renegotiation
        createPeerConnection(peer);
        setRemoteDescription(peer, m->sdp, "offer");
        break;
      case signaling::Type::Answer:
        if (server || !peer.pc || peer.remoteDescriptionSet) return;
        setRemoteDescription(peer, m->sdp, "answer");
        break;
      case signaling::Type::Candidate:
        if (peer.pc && peer.remoteDescriptionSet) {
          addCandidate(peer, *m);
        } else if (peer.earlyCandidates.size() < kMaxEarlyCandidates) {
          peer.earlyCandidates.push_back(*m);
        }
        break;
    }
  }

  void accept(Received&& r) {
    if (inbox.size() >= kMaxQueuedPackets) {
      ++stats.recvDropped;
      return;
    }
    ++stats.packetsReceived;
    stats.bytesReceived += r.bytes.size();
    inbox.push_back(std::move(r));
  }

  void markOpen(Peer& peer, Lane lane, uint64_t nowNs, ConnectionEvents& events) {
    peer.laneOpen[laneIndex(lane)] = true;
    if (peer.connected || !peer.laneOpen[0] || !peer.laneOpen[1]) return;
    peer.connected = true;
    events.push(ConnectionEvent{ConnectionEventKind::Connected, peer.id, nowNs});
    for (auto& r : peer.early)
      accept(std::move(r));
    peer.early.clear();
  }

  void handle(CallbackEvent& e, uint64_t nowNs, uint64_t steadyNs, ConnectionEvents& events) {
    if (e.kind == CallbackKind::NewClient) {
      if (peers.size() >= maxPeers) {
        Peer rejected;
        rejected.ws = std::move(e.ws);
        closePeer(rejected);
        return;
      }
      Peer& peer = peers[e.peer.value];
      peer.id = e.peer;
      peer.ws = std::move(e.ws);
      return;
    }
    const auto it = peers.find(e.peer.value);
    if (it == peers.end()) return;  // dropped or disconnected already
    Peer& peer = it->second;
    switch (e.kind) {
      case CallbackKind::NewClient:
        break;
      case CallbackKind::SignalOpen:
        if (!server && !peer.pc) startClientOffer(peer);
        break;
      case CallbackKind::SignalClosed:
        // After Connected the socket has done its job; before it, the
        // handshake cannot finish.
        if (!peer.connected) dropPeer(e.peer.value, nowNs, &events);
        break;
      case CallbackKind::SignalText:
        handleSignal(peer, e.text);
        break;
      case CallbackKind::ChannelArrived: {
        auto& slot = peer.lanes[laneIndex(e.lane)];
        if (slot) {  // a second channel with the same label
          e.dc->close();
          break;
        }
        slot = std::move(e.dc);
        if (slot->isOpen()) markOpen(peer, e.lane, nowNs, events);
        break;
      }
      case CallbackKind::ChannelOpen:
        markOpen(peer, e.lane, nowNs, events);
        break;
      case CallbackKind::ChannelClosed:
        dropPeer(e.peer.value, nowNs, &events);
        break;
      case CallbackKind::PeerState:
        if (e.state == rtc::PeerConnection::State::Failed ||
            e.state == rtc::PeerConnection::State::Closed)
          dropPeer(e.peer.value, nowNs, &events);
        break;
      case CallbackKind::Message: {
        const size_t limit = e.lane == Lane::Reliable ? kMaxReliableBytes : kMaxUnreliableBytes;
        if (e.bytes.size() > limit) {
          ++stats.recvDropped;
          break;
        }
        // Stamp on the caller's clock: its now minus the message's age,
        // never before the previous poll.
        const uint64_t age = steadyNs > e.steadyNs ? steadyNs - e.steadyNs : 0;
        uint64_t stamp = nowNs > age ? nowNs - age : 0;
        stamp = std::clamp(stamp, std::min(lastPollNs, nowNs), nowNs);
        Received r{e.peer, e.lane, stamp, std::move(e.bytes)};
        if (peer.connected) {
          accept(std::move(r));
        } else if (peer.early.size() < kMaxQueuedPackets) {
          peer.early.push_back(std::move(r));
        } else {
          ++stats.recvDropped;
        }
        break;
      }
    }
  }

  void poll(uint64_t nowNs, ConnectionEvents& events) {
    for (const auto& e : localEvents)
      events.push(e);
    localEvents.clear();

    drained.clear();
    {
      const std::scoped_lock lock(queue->mutex);
      drained.swap(queue->events);
      queue->queuedMessages = 0;
      stats.recvDropped += queue->droppedMessages;
      queue->droppedMessages = 0;
    }
    const uint64_t steadyNs = steadyNowNs();
    for (auto& e : drained) {
      try {
        handle(e, nowNs, steadyNs, events);
      } catch (const std::exception& ex) {
        fail("poll", &ex);
        dropPeer(e.peer.value, nowNs, &events);
      } catch (...) {
        fail("poll", nullptr);
        dropPeer(e.peer.value, nowNs, &events);
      }
    }
    drained.clear();  // releases sockets/channels of rejected or stale events here

    std::vector<uint16_t> expired;
    for (auto& [id, peer] : peers) {
      if (peer.connected) continue;
      if (!peer.deadlineNs) peer.deadlineNs = nowNs + connectTimeoutNs;
      if (nowNs >= *peer.deadlineNs) expired.push_back(id);
    }
    for (const uint16_t id : expired)
      dropPeer(id, nowNs, &events);
    lastPollNs = nowNs;
  }

  void send(PeerId peer, Lane lane, std::span<const std::byte> data) {
    const size_t limit = lane == Lane::Reliable ? kMaxReliableBytes : kMaxUnreliableBytes;
    const auto it = peers.find(peer.value);
    if (data.size() > limit || it == peers.end() || !it->second.connected) {
      ++stats.sendErrors;
      return;
    }
    const auto& dc = it->second.lanes[laneIndex(lane)];
    try {
      if (!dc || !dc->isOpen() ||
          (lane == Lane::Unreliable && dc->bufferedAmount() > kMaxUnreliableBuffered)) {
        ++stats.sendErrors;
        return;
      }
      const bool ok = dc->send(data.data(), data.size());
#ifdef __EMSCRIPTEN__
      // datachannel-wasm: false means the browser refused the message.
      if (!ok) {
        ++stats.sendErrors;
        return;
      }
#else
      (void)ok;  // libdatachannel: false only means "buffered", it is still sent
#endif
      ++stats.packetsSent;
      stats.bytesSent += data.size();
    } catch (const std::exception& ex) {
      fail("send", &ex);
      ++stats.sendErrors;
    } catch (...) {
      fail("send", nullptr);
      ++stats.sendErrors;
    }
  }
};

std::expected<std::unique_ptr<WebRtcTransport>, std::string> WebRtcTransport::createServer(
    const WebRtcServerConfig& config) {
#ifdef __EMSCRIPTEN__
  (void)config;
  return std::unexpected(std::string("WebRtcTransport: the server role is native only"));
#else
  try {
    initLoggerOnce(WebRtcLogLevel::Warning);
    auto impl = std::make_unique<Impl>();
    impl->server = true;
    impl->stunServer = config.stunServer;
    impl->icePortBegin = config.icePortBegin;
    impl->icePortEnd = config.icePortEnd;
    impl->iceUdpMux = config.iceUdpMux;
    impl->maxPeers = std::min(config.maxPeers, kMaxPeers);
    impl->connectTimeoutNs = uint64_t{config.connectTimeoutMs} * kNsPerMs;

    rtc::WebSocketServer::Configuration wsConfig;
    wsConfig.port = config.signalingPort;
    if (!config.signalingBindAddress.empty()) wsConfig.bindAddress = config.signalingBindAddress;
    wsConfig.maxMessageSize = signaling::kMaxMessageBytes;
    impl->wsServer = std::make_shared<rtc::WebSocketServer>(wsConfig);
    impl->listenPort = impl->wsServer->port();

    const WeakQueue weak = impl->queue;
    impl->wsServer->onClient([weak](const std::shared_ptr<rtc::WebSocket>& ws) {
      const auto q = weak.lock();
      if (!q) return;
      PeerId id = PeerId::invalid();
      {
        const std::scoped_lock lock(q->mutex);
        if (q->nextPeerId != PeerId::kInvalidValue) id = PeerId{q->nextPeerId++};
      }
      if (!id.isValid()) {  // 65534 clients in one process: refuse the rest
        ws->close();
        return;
      }
      wireSignalingSocket(*ws, weak, id);
      CallbackEvent e;
      e.kind = CallbackKind::NewClient;
      e.peer = id;
      e.ws = ws;
      q->push(std::move(e));
    });
    return std::make_unique<WebRtcTransport>(std::move(impl));
  } catch (const std::exception& e) {
    return std::unexpected(std::string("WebRtcTransport server: ") + e.what());
  } catch (...) {
    return std::unexpected(std::string("WebRtcTransport server: unknown error"));
  }
#endif
}

std::expected<std::unique_ptr<WebRtcTransport>, std::string> WebRtcTransport::createClient(
    const WebRtcClientConfig& config) {
  if (!config.signalingUrl.starts_with("ws://") && !config.signalingUrl.starts_with("wss://"))
    return std::unexpected("WebRtcTransport client: signalingUrl must be ws://host:port, got '" +
                           config.signalingUrl + "'");
  try {
    initLoggerOnce(WebRtcLogLevel::Warning);
    auto impl = std::make_unique<Impl>();
    impl->server = false;
    impl->stunServer = config.stunServer;
    impl->token = config.connectionToken;
    impl->connectTimeoutNs = uint64_t{config.connectTimeoutMs} * kNsPerMs;

    Peer& peer = impl->peers[kServerPeer.value];
    peer.id = kServerPeer;
#ifdef __EMSCRIPTEN__
    peer.ws = std::make_shared<rtc::WebSocket>();
#else
    rtc::WebSocket::Configuration wsConfig;
    wsConfig.maxMessageSize = signaling::kMaxMessageBytes;
    peer.ws = std::make_shared<rtc::WebSocket>(wsConfig);
#endif
    wireSignalingSocket(*peer.ws, impl->queue, kServerPeer);
    peer.ws->open(config.signalingUrl);
    return std::make_unique<WebRtcTransport>(std::move(impl));
  } catch (const std::exception& e) {
    return std::unexpected(std::string("WebRtcTransport client: ") + e.what());
  } catch (...) {
    return std::unexpected(std::string("WebRtcTransport client: unknown error"));
  }
}

void WebRtcTransport::initLogging(WebRtcLogLevel level) {
  initLoggerOnce(level);
}

WebRtcTransport::WebRtcTransport(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

WebRtcTransport::~WebRtcTransport() {
  try {
    impl_.reset();
  } catch (...) {  // NOLINT(bugprone-empty-catch): never let a destructor throw
  }
}

void WebRtcTransport::sendUnreliable(PeerId peer, std::span<const std::byte> data) {
  impl_->send(peer, Lane::Unreliable, data);
}

void WebRtcTransport::sendReliable(PeerId peer, std::span<const std::byte> data) {
  impl_->send(peer, Lane::Reliable, data);
}

bool WebRtcTransport::recv(Packet& out) {
  if (impl_->inbox.empty()) return false;
  Received& front = impl_->inbox.front();
  impl_->current = std::move(front.bytes);
  out.peer = front.peer;
  out.lane = front.lane;
  out.receivedNs = front.receivedNs;
  out.data = std::span<const std::byte>(impl_->current);
  impl_->inbox.pop_front();
  return true;
}

void WebRtcTransport::poll(uint64_t nowNs, ConnectionEvents& events) {
  try {
    impl_->poll(nowNs, events);
  } catch (const std::exception& e) {
    impl_->fail("poll", &e);
  } catch (...) {
    impl_->fail("poll", nullptr);
  }
}

void WebRtcTransport::disconnect(PeerId peer) {
  try {
    impl_->dropPeer(peer.value, impl_->lastPollNs, nullptr);
  } catch (const std::exception& e) {
    impl_->fail("disconnect", &e);
  } catch (...) {
    impl_->fail("disconnect", nullptr);
  }
}

bool WebRtcTransport::isConnected(PeerId peer) const {
  const auto it = impl_->peers.find(peer.value);
  return it != impl_->peers.end() && it->second.connected;
}

TransportStats WebRtcTransport::stats() const {
  return impl_->stats;
}

bool WebRtcTransport::isServer() const {
  return impl_->server;
}

uint16_t WebRtcTransport::listenPort() const {
  return impl_->listenPort;
}

std::span<const std::byte> WebRtcTransport::connectionToken() const {
  return impl_->token;
}

std::string WebRtcTransport::lastError() const {
  return impl_->lastError;
}

}  // namespace kfm::net
