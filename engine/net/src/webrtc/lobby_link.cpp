// LobbyLink over libdatachannel's rtc::WebSocket, see kfm/net/lobby_link.hpp
// for the contract and docs/lobby-protocol.md §5 for the messages. Every
// entry point catches at the boundary: the rtc:: API throws, kfm_server is
// built without exceptions.
#include "kfm/net/lobby_link.hpp"

#include "kfm/net/transport.hpp"
#include "kfm/net/webrtc_signaling.hpp"
#include "kfm/net/webrtc_transport.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <rtc/rtc.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kfm::net {
namespace {

constexpr uint64_t kNsPerMs = 1'000'000;
// What the lobby socket accepts before libdatachannel truncates a message:
// well above the protocol's limit, so an oversized frame arrives whole and is
// ignored as such (docs/lobby-protocol.md §7) instead of reaching the decoder
// cut short. Sends are held to signaling::kMaxMessageBytes by send().
constexpr size_t kSocketMessageLimit = size_t{1024} * 1024;
// The protocol version this link speaks (`register`'s `version`, spec v1.3).
constexpr std::string_view kVersion = "1.3";
// The most tokens a room claim lists (the lobby keeps at most this many).
constexpr size_t kMaxClaimTokens = 128;
// The reconnect backoff's ceiling while the server still holds a room or
// players, so the lobby re-adopts it within seconds of coming back.
constexpr uint64_t kRoomReconnectMaxMs = 5'000;

// Whether a `version` value ("1.3", "1.10", "2.0") is 1.3 or later.
[[nodiscard]] bool atLeastV13(std::string_view v) {
  const auto number = [](std::string_view s) -> std::optional<uint32_t> {
    if (s.empty() || s.size() > 6) return std::nullopt;
    uint32_t n = 0;
    for (const char c : s) {
      if (c < '0' || c > '9') return std::nullopt;
      n = (n * 10) + static_cast<uint32_t>(c - '0');
    }
    return n;
  };
  const size_t dot = v.find('.');
  const auto major = number(v.substr(0, dot));
  if (!major) return false;
  if (dot == std::string_view::npos) return *major >= 2;
  const auto minor = number(v.substr(dot + 1));
  return minor && (*major > 1 || (*major == 1 && *minor >= 3));
}

enum class SocketEventKind : uint8_t { Open = 0, Closed, Text };

struct SocketEvent {
  uint32_t generation{0};  // which socket; a stale one's events are ignored
  SocketEventKind kind{SocketEventKind::Open};
  std::string text;
};

// The only state the socket's callbacks touch, captured weakly.
struct SocketQueue {
  std::mutex mutex;
  std::vector<SocketEvent> events;

  void push(SocketEvent&& e) {
    const std::scoped_lock lock(mutex);
    events.push_back(std::move(e));
  }
};

void postEvent(const std::weak_ptr<SocketQueue>& weak, uint32_t generation, SocketEventKind kind,
               std::string text = {}) {
  if (const auto q = weak.lock()) q->push(SocketEvent{generation, kind, std::move(text)});
}

}  // namespace

struct LobbyLink::Impl {
  WebRtcTransport* transport{nullptr};
  LobbyLinkConfig config;
  std::string url;

  std::shared_ptr<SocketQueue> queue = std::make_shared<SocketQueue>();
  std::shared_ptr<rtc::WebSocket> ws;
  uint32_t generation{0};
  LobbyLinkState state{LobbyLinkState::Waiting};
  uint64_t deadlineNs{0};  // Connecting/Registering: give up at; Waiting: retry at
  uint64_t backoffMs{0};
  uint32_t attempts{0};
  uint16_t capacity{0};
  std::optional<uint16_t> sentCapacity;
  // v1.3: the lobby's `registered` named a version that takes `players`.
  bool lobbyV13{false};
  std::optional<uint32_t> sentPlayers;  // the last `players` count, per room

  struct LobbyPeer {
    PeerId peer{PeerId::invalid()};
    std::string token;
  };
  std::map<std::string, LobbyPeer> byLobby;     // lobby peer id -> ours
  std::map<uint16_t, std::string> byTransport;  // ours -> lobby peer id
  // v1.3: peers kept across a lost link (their data channels were open). They
  // have no lobby peer id any more: ours -> token.
  std::map<uint16_t, std::string> detached;
  std::optional<std::string> room;
  // Every token the lobby announced for `room`, oldest first (a room claim's
  // `tokens`), at most kMaxClaimTokens.
  std::vector<std::string> roomTokens;
  // A claim the lobby did not adopt: the kept players stay until they leave,
  // and the process offers capacity 0 meanwhile.
  bool holding{false};
  std::string lastError;
  uint32_t ignoredFrames{0};
  std::vector<LobbyEvent> out;

  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl() {
    closeSocket();
    const std::scoped_lock lock(queue->mutex);
    queue->events.clear();
  }

  void closeSocket() {
    if (!ws) return;
    try {
      ws->close();
    } catch (...) {  // NOLINT(bugprone-empty-catch): already closed
    }
    ws.reset();
  }

  void open(uint64_t nowNs) {
    closeSocket();
    ++generation;
    ++attempts;
    state = LobbyLinkState::Connecting;
    deadlineNs = nowNs + uint64_t{config.openTimeoutMs} * kNsPerMs;
    sentCapacity.reset();
    rtc::WebSocket::Configuration wsConfig;
    wsConfig.maxMessageSize = kSocketMessageLimit;
    ws = std::make_shared<rtc::WebSocket>(wsConfig);
    const std::weak_ptr<SocketQueue> weak = queue;
    const uint32_t gen = generation;
    ws->onOpen([weak, gen] { postEvent(weak, gen, SocketEventKind::Open); });
    ws->onClosed([weak, gen] { postEvent(weak, gen, SocketEventKind::Closed); });
    ws->onError([weak, gen](const std::string& error) {
      postEvent(weak, gen, SocketEventKind::Closed, error);
    });
    ws->onMessage([weak, gen](rtc::message_variant message) {
      if (auto* text = std::get_if<std::string>(&message)) {
        postEvent(weak, gen, SocketEventKind::Text, std::move(*text));
      } else {
        // Text frames only (spec §1): a binary frame is a protocol error.
        postEvent(weak, gen, SocketEventKind::Closed, "binary frame from the lobby");
      }
    });
    ws->open(url);
  }

  void send(std::span<const signaling::Field> fields) {
    if (!ws || !ws->isOpen()) return;
    std::string text = signaling::encodeObject(fields);
    // The lobby closes a socket that sends a frame over the limit (spec §1):
    // a message that large (none of ours is) is dropped here instead.
    if (text.size() > signaling::kMaxMessageBytes) {
      lastError = "dropped an outgoing lobby message over the size limit";
      ++ignoredFrames;
      return;
    }
    ws->send(std::move(text));
  }

  // Registered: a frame from the lobby we cannot use. The lobby is trusted and
  // the frame most likely one meant for a single peer (a `signal`), so it is
  // ignored rather than taken as a reason to drop the link and every peer with
  // it (spec §7); that peer's handshake times out on its own.
  void ignore(std::string_view what) {
    lastError = "ignored a lobby frame: " + std::string(what);
    ++ignoredFrames;
  }

  void sendPeerGone(const std::string& lobbyPeer) {
    const std::array<signaling::Field, 2> f = {signaling::Field{"type", "peer_gone"},
                                               signaling::Field{"peer", lobbyPeer}};
    send(f);
  }

  // What the lobby should offer: nothing while players of a room it did not
  // adopt are still here.
  [[nodiscard]] uint16_t effectiveCapacity() const { return holding ? uint16_t{0} : capacity; }

  void sendCapacityIfChanged() {
    const uint16_t c = effectiveCapacity();
    if (state != LobbyLinkState::Registered || sentCapacity == c) return;
    const std::string text = std::to_string(c);
    const std::array<signaling::Field, 2> f = {signaling::Field{"type", "capacity"},
                                               signaling::Field{"capacity", text}};
    send(f);
    sentCapacity = c;
  }

  void rememberToken(const std::string& token) {
    if (std::ranges::find(roomTokens, token) != roomTokens.end()) return;
    roomTokens.push_back(token);
    if (roomTokens.size() > kMaxClaimTokens) roomTokens.erase(roomTokens.begin());
  }

  // The room's players on their game connection: lobby peers whose data
  // channels are both open.
  [[nodiscard]] uint32_t connectedPlayers() const {
    const auto lobbyPeers = std::ranges::count_if(
        byLobby, [&](const auto& entry) { return transport->isConnected(entry.second.peer); });
    const auto kept = std::ranges::count_if(
        detached, [&](const auto& entry) { return transport->isConnected(PeerId{entry.first}); });
    return static_cast<uint32_t>(lobbyPeers + kept);
  }

  // v1.3 `players`, to a lobby that takes it, when the count changes (an
  // older lobby would take the unknown type as a protocol error).
  void sendPlayersIfChanged() {
    if (state != LobbyLinkState::Registered || !lobbyV13 || !room) return;
    const uint32_t n = connectedPlayers();
    if (sentPlayers == n) return;
    const std::string c = std::to_string(n);
    const std::array<signaling::Field, 2> f = {signaling::Field{"type", "players"},
                                               signaling::Field{"count", c}};
    send(f);
    sentPlayers = n;
  }

  void sendRegister() {
    const std::string build = std::format("{:08x}", config.buildId);
    const std::string content = std::format("{:08x}", config.contentHash);
    const std::string cap = std::to_string(capacity);
    std::vector<signaling::Field> f = {
        signaling::Field{"type", "register"},      signaling::Field{"secret", config.secret},
        signaling::Field{"region", config.region}, signaling::Field{"name", config.name},
        signaling::Field{"build", build},          signaling::Field{"content", content},
        signaling::Field{"capacity", cap},         signaling::Field{"version", kVersion}};
    // v1.3: the room this process still hosts, claimed back (spec §5).
    std::string tokens;
    std::string players;
    if (room) {
      for (const std::string& t : roomTokens) {
        if (!tokens.empty()) tokens += ',';
        tokens += t;
      }
      players = std::to_string(connectedPlayers());
      f.emplace_back("room", *room);
      f.emplace_back("tokens", tokens);
      f.emplace_back("players", players);
    }
    send(f);
    sentCapacity = capacity;
    lobbyV13 = false;
    state = LobbyLinkState::Registering;
  }

  void forgetPeer(const std::string& lobbyPeer) {
    const auto it = byLobby.find(lobbyPeer);
    if (it == byLobby.end()) return;
    byTransport.erase(it->second.peer.value);
    byLobby.erase(it);
  }

  // The room is over: every peer goes, the lobby's and the kept ones.
  void closeAllPeers() {
    for (const auto& [lobbyPeer, p] : byLobby) {
      transport->closeExternalPeer(p.peer);
      out.push_back(LobbyEvent{LobbyEventKind::PeerGone, p.peer, lobbyPeer, {}, false, 0});
    }
    for (const auto& [id, token] : detached) {
      transport->closeExternalPeer(PeerId{id});
      out.push_back(LobbyEvent{LobbyEventKind::PeerGone, PeerId{id}, {}, {}, false, 0});
    }
    byLobby.clear();
    byTransport.clear();
    detached.clear();
    holding = false;
  }

  // Peers the transport dropped on its own (disconnect(), failure, timeout):
  // a lobby peer is reported to the lobby when the link is up; a kept one has
  // nobody to tell. Runs in every state, so the kept set stays true.
  void drainDroppedPeers() {
    const bool registered = state == LobbyLinkState::Registered;
    for (const PeerId id : transport->takeDroppedExternalPeers()) {
      if (const auto it = byTransport.find(id.value); it != byTransport.end()) {
        const std::string lobbyPeer = it->second;
        if (registered) sendPeerGone(lobbyPeer);
        forgetPeer(lobbyPeer);
        out.push_back(LobbyEvent{LobbyEventKind::PeerGone, id, lobbyPeer, {}, true, 0});
      } else if (detached.erase(id.value) > 0) {
        out.push_back(LobbyEvent{LobbyEventKind::PeerGone, id, {}, {}, true, 0});
      }
    }
    if (holding && detached.empty()) holding = false;  // free again: real capacity
  }

  // v1.3 (spec §0, §5): the socket is gone, the match is not. Peers whose data
  // channels are open are kept (detached) and the room with its tokens is
  // claimed back at the next registration; peers still setting up cannot
  // finish without the lobby and are closed.
  void lose(uint64_t nowNs, std::string why) {
    closeSocket();
    drainDroppedPeers();
    size_t kept = 0;
    for (const auto& [lobbyPeer, p] : byLobby) {
      if (transport->isConnected(p.peer)) {
        detached[p.peer.value] = p.token;
        ++kept;
      } else {
        transport->closeExternalPeer(p.peer);
        out.push_back(LobbyEvent{LobbyEventKind::PeerGone, p.peer, lobbyPeer, {}, false, 0});
      }
    }
    byLobby.clear();
    byTransport.clear();
    // Whatever the transport still holds for the closed peers is moot now.
    (void)transport->takeOutgoingSignaling();
    const uint64_t ceiling = room || !detached.empty()
                                 ? std::min<uint64_t>(config.reconnectMaxMs, kRoomReconnectMaxMs)
                                 : uint64_t{config.reconnectMaxMs};
    backoffMs = backoffMs == 0 ? std::min<uint64_t>(config.reconnectMinMs, ceiling)
                               : std::min<uint64_t>(backoffMs * 2, ceiling);
    state = LobbyLinkState::Waiting;
    deadlineNs = nowNs + backoffMs * kNsPerMs;
    if (!why.empty()) lastError = why;
    if (kept > 0) why += std::format("; {} connected peer(s) kept", kept);
    out.push_back(LobbyEvent{
        LobbyEventKind::LinkLost, {}, {}, std::move(why), false, static_cast<uint32_t>(backoffMs)});
  }

  // The lobby's answer to a room claim (spec §5): `registered` named the room
  // (adopted: the room goes on, players and tokens as claimed), or did not (a
  // v1.2 lobby, or the code is taken there): the room is closed as far as the
  // lobby goes, and the kept players stay until they leave with capacity 0.
  void settleClaim(std::optional<std::string_view> adopted) {
    const std::string code = room.value_or("");
    if (lobbyV13 && adopted == std::optional<std::string_view>(code)) {
      sentPlayers = connectedPlayers();  // the claim carried it
      out.push_back(LobbyEvent{LobbyEventKind::RoomAdopted, {}, {}, code, false, 0});
      return;
    }
    out.push_back(LobbyEvent{LobbyEventKind::RoomClosed, {}, {}, code, false, 0});
    room.reset();
    roomTokens.clear();
    holding = !detached.empty();
    // A v1.3 lobby registers a refused claim at capacity 0 itself.
    if (lobbyV13) sentCapacity = 0;
  }

  // A protocol error closes the socket (spec §1) and reconnects.
  void protocolError(uint64_t nowNs, std::string_view what) {
    lose(nowNs, "lobby protocol error: " + std::string(what));
  }

  void handleText(uint64_t nowNs, const std::string& text) {
    if (state == LobbyLinkState::Registered && text.size() > signaling::kMaxMessageBytes) {
      ignore("over the size limit");
      return;
    }
    const auto o = signaling::decodeObject(text);
    if (!o) {
      if (state == LobbyLinkState::Registered) {
        ignore("not a flat object of strings");
      } else {
        protocolError(nowNs, "not a flat object of strings");
      }
      return;
    }
    const std::string_view type = o->get("type").value_or("");
    if (state == LobbyLinkState::Registering) {
      if (type == "registered") {
        state = LobbyLinkState::Registered;
        backoffMs = 0;
        lobbyV13 = atLeastV13(o->get("version").value_or(""));
        out.push_back(LobbyEvent{LobbyEventKind::Registered, {}, {}, {}, false, 0});
        if (room) settleClaim(o->get("room"));
        sendCapacityIfChanged();
      } else if (type == "refused") {
        const std::string reason(o->get("reason").value_or("unknown"));
        out.push_back(LobbyEvent{LobbyEventKind::Refused, {}, {}, reason, false, 0});
        lose(nowNs, "lobby refused: " + reason);
      } else {
        protocolError(nowNs, "expected registered or refused");
      }
      return;
    }
    if (state != LobbyLinkState::Registered) return;
    const auto need = [&](std::string_view key) -> std::optional<std::string> {
      if (const auto v = o->get(key)) return std::string(*v);
      return std::nullopt;
    };
    if (type == "room") {
      const auto code = need("code");
      if (!code) {
        ignore("room without code");
        return;
      }
      room = *code;
      roomTokens.clear();
      sentPlayers = 0;  // a new room starts with nobody connected
      out.push_back(LobbyEvent{LobbyEventKind::RoomAssigned, {}, {}, *code, false, 0});
    } else if (type == "peer") {
      const auto peer = need("peer");
      const auto token = need("token");
      if (!peer || !token) {
        ignore("peer without peer or token");
        return;
      }
      if (byLobby.contains(*peer)) return;  // announced twice: keep the first
      const std::optional<PeerId> id = transport->acceptExternalPeer();
      if (!id) {
        lastError = "transport full: peer " + *peer + " refused";
        sendPeerGone(*peer);
        return;
      }
      byLobby[*peer] = LobbyPeer{*id, *token};
      byTransport[id->value] = *peer;
      rememberToken(*token);
      out.push_back(LobbyEvent{LobbyEventKind::PeerJoined, *id, *peer, {}, false, 0});
    } else if (type == "signal") {
      const auto peer = need("peer");
      const auto msg = need("msg");
      if (!peer || !msg) {
        ignore("signal without peer or msg");
        return;
      }
      const auto it = byLobby.find(*peer);
      if (it == byLobby.end()) return;  // a peer we dropped a moment ago
      if (!transport->deliverSignaling(it->second.peer, *msg))
        lastError = "peer " + *peer + ": " + transport->lastError();
    } else if (type == "peer_gone") {
      const auto peer = need("peer");
      if (!peer) {
        ignore("peer_gone without peer");
        return;
      }
      const auto it = byLobby.find(*peer);
      if (it == byLobby.end()) return;
      const PeerId id = it->second.peer;
      transport->closeExternalPeer(id);
      forgetPeer(*peer);
      out.push_back(LobbyEvent{LobbyEventKind::PeerGone, id, *peer, {}, false, 0});
    } else if (type == "room_closed") {
      const std::string code(o->get("code").value_or(room.value_or("")));
      // Every peer has left by then; any that has not, goes with the room.
      closeAllPeers();
      room.reset();
      roomTokens.clear();
      out.push_back(LobbyEvent{LobbyEventKind::RoomClosed, {}, {}, code, false, 0});
    }
    // Unknown types are ignored (forward compatibility).
  }

  // Registered: the transport's side of the relay (drainDroppedPeers() has
  // run this poll).
  void relayFromTransport() {
    for (const OutgoingSignal& s : transport->takeOutgoingSignaling()) {
      const auto it = byTransport.find(s.peer.value);
      if (it == byTransport.end()) continue;
      const std::array<signaling::Field, 3> f = {signaling::Field{"type", "signal"},
                                                 signaling::Field{"peer", it->second},
                                                 signaling::Field{"msg", s.text}};
      send(f);
    }
    sendCapacityIfChanged();
    sendPlayersIfChanged();
  }

  void poll(uint64_t nowNs) {
    std::vector<SocketEvent> events;
    {
      const std::scoped_lock lock(queue->mutex);
      events.swap(queue->events);
    }
    for (const SocketEvent& e : events) {
      if (e.generation != generation || state == LobbyLinkState::Waiting) continue;
      switch (e.kind) {
        case SocketEventKind::Open:
          if (state == LobbyLinkState::Connecting) sendRegister();
          break;
        case SocketEventKind::Closed:
          lose(nowNs, e.text.empty() ? std::string("lobby connection closed")
                                     : "lobby connection failed: " + e.text);
          break;
        case SocketEventKind::Text:
          handleText(nowNs, e.text);
          break;
      }
    }
    drainDroppedPeers();
    if (state != LobbyLinkState::Registered) (void)transport->takeOutgoingSignaling();
    switch (state) {
      case LobbyLinkState::Connecting:
      case LobbyLinkState::Registering:
        if (nowNs >= deadlineNs) lose(nowNs, "lobby did not register us in time");
        break;
      case LobbyLinkState::Registered:
        relayFromTransport();
        break;
      case LobbyLinkState::Waiting:
        if (nowNs >= deadlineNs) open(nowNs);
        break;
    }
  }
};

std::expected<std::unique_ptr<LobbyLink>, std::string> LobbyLink::create(WebRtcTransport& transport,
                                                                         LobbyLinkConfig config,
                                                                         uint64_t nowNs) {
  if (!config.url.starts_with("ws://") && !config.url.starts_with("wss://"))
    return std::unexpected("LobbyLink: the lobby URL must be ws[s]://host:port, got '" +
                           config.url + "'");
  if (config.secret.empty()) return std::unexpected(std::string("LobbyLink: empty secret"));
  if (config.region.empty()) return std::unexpected(std::string("LobbyLink: empty region"));
  if (!transport.isServer() || transport.listenPort() != 0)
    return std::unexpected(
        std::string("LobbyLink: the transport must be a server with externalSignaling"));
  try {
    WebRtcTransport::initLogging(WebRtcLogLevel::Warning);
    auto impl = std::make_unique<Impl>();
    impl->transport = &transport;
    impl->config = std::move(config);
    impl->url = signaling::withDefaultPath(impl->config.url, "/server");
    impl->capacity = impl->config.capacity;
    impl->config.reconnectMinMs = std::max<uint32_t>(impl->config.reconnectMinMs, 1);
    impl->config.reconnectMaxMs =
        std::max(impl->config.reconnectMaxMs, impl->config.reconnectMinMs);
    impl->open(nowNs);
    return std::make_unique<LobbyLink>(std::move(impl));
  } catch (const std::exception& e) {
    return std::unexpected(std::string("LobbyLink: ") + e.what());
  } catch (...) {
    return std::unexpected(std::string("LobbyLink: unknown error"));
  }
}

LobbyLink::LobbyLink(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

LobbyLink::~LobbyLink() {
  try {
    impl_.reset();
  } catch (...) {  // NOLINT(bugprone-empty-catch): never let a destructor throw
  }
}

std::vector<LobbyEvent> LobbyLink::poll(uint64_t nowNs) {
  try {
    impl_->poll(nowNs);
  } catch (const std::exception& e) {
    // A send or open that threw: the socket is unusable, start over.
    try {
      impl_->lose(nowNs, std::string("lobby socket error: ") + e.what());
    } catch (...) {  // NOLINT(bugprone-empty-catch): the next poll retries
    }
  } catch (...) {
    try {
      impl_->lose(nowNs, "lobby socket error");
    } catch (...) {  // NOLINT(bugprone-empty-catch): the next poll retries
    }
  }
  std::vector<LobbyEvent> out;
  out.swap(impl_->out);
  return out;
}

void LobbyLink::setCapacity(uint16_t capacity) {
  impl_->capacity = capacity;
}

bool LobbyLink::lobbySpeaksV13() const {
  return impl_->lobbyV13;
}

bool LobbyLink::acceptsToken(std::span<const std::byte> token) const {
  const std::string_view t(reinterpret_cast<const char*>(token.data()), token.size());
  return std::ranges::any_of(impl_->byLobby,
                             [&](const auto& entry) { return entry.second.token == t; });
}

LobbyLinkState LobbyLink::state() const {
  return impl_->state;
}

std::optional<std::string> LobbyLink::room() const {
  return impl_->room;
}

size_t LobbyLink::peerCount() const {
  return impl_->byLobby.size() + impl_->detached.size();
}

size_t LobbyLink::keptPeerCount() const {
  return impl_->detached.size();
}

bool LobbyLink::holding() const {
  return impl_->holding;
}

uint16_t LobbyLink::capacity() const {
  return impl_->capacity;
}

uint32_t LobbyLink::reconnects() const {
  return impl_->attempts > 0 ? impl_->attempts - 1 : 0;
}

std::string LobbyLink::statusText(uint64_t nowNs) const {
  const Impl& i = *impl_;
  switch (i.state) {
    case LobbyLinkState::Connecting:
      return "connecting";
    case LobbyLinkState::Registering:
      return "registering";
    case LobbyLinkState::Registered:
      return std::format("registered room={} peers={} capacity={}", i.room.value_or("-"),
                         i.byLobby.size(), i.capacity);
    case LobbyLinkState::Waiting: {
      const uint64_t ms = i.deadlineNs > nowNs ? (i.deadlineNs - nowNs) / kNsPerMs : 0;
      return std::format("waiting retry_in={}ms room={} kept={}", ms, i.room.value_or("-"),
                         i.detached.size());
    }
  }
  return "unknown";
}

std::string LobbyLink::lastError() const {
  return impl_->lastError;
}

uint32_t LobbyLink::ignoredFrames() const {
  return impl_->ignoredFrames;
}

std::string LobbyLink::url() const {
  return impl_->url;
}

}  // namespace kfm::net
