// FakeLobby: an in-process stand-in for services/lobby (docs/lobby-protocol.md)
// on 127.0.0.1, enough of both sides for the engine's lobby tests: one
// server socket (/server: register with a secret, room, peer, signal relay,
// peer_gone both ways, capacity) and client sockets (/client: regions (v1.1),
// create/join, joined/refused, signaling relay, closed). One room at a time, code
// "ABCDE", or "FGHJK" for the second, and so on. Test code only: it runs on
// libdatachannel's callback threads under one recursive mutex and uses
// exceptions freely.
#pragma once

#include "kfm/net/webrtc_signaling.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <rtc/rtc.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kfm::net::test {

class FakeLobby {
  // Runs f under the lock (declared first: the accessors deduce from it).
  template <class F>
  auto locked(F&& f) const {
    const std::scoped_lock lock(mutex_);
    return f();
  }

 public:
  explicit FakeLobby(std::string secret) : secret_(std::move(secret)) {
    rtc::WebSocketServer::Configuration c;
    c.port = 0;
    c.bindAddress = "127.0.0.1";
    server_ = std::make_unique<rtc::WebSocketServer>(c);
    server_->onClient([this](const std::shared_ptr<rtc::WebSocket>& ws) { onSocket(ws); });
  }
  FakeLobby(const FakeLobby&) = delete;
  FakeLobby& operator=(const FakeLobby&) = delete;
  FakeLobby(FakeLobby&&) = delete;
  FakeLobby& operator=(FakeLobby&&) = delete;
  ~FakeLobby() {
    std::vector<std::shared_ptr<rtc::WebSocket>> all;
    {
      const std::scoped_lock lock(mutex_);
      stopped_ = true;
      all = std::move(sockets_);  // a move, not a copy: GCC 15 -O3 flags the copy's refcount path
      sockets_.clear();
    }
    for (const auto& ws : all) {
      try {
        ws->close();
      } catch (...) {  // NOLINT(bugprone-empty-catch): test teardown
      }
    }
    server_->stop();
  }

  [[nodiscard]] std::string url() const {
    return "ws://127.0.0.1:" + std::to_string(server_->port());
  }

  // --- what the tests look at ---
  [[nodiscard]] int registrations() const {
    return locked([&] { return registrations_; });
  }
  [[nodiscard]] bool serverRegistered() const {
    return locked([&] { return serverWs_ != nullptr && registered_; });
  }
  [[nodiscard]] std::string serverCapacity() const {
    return locked([&] { return capacity_; });
  }
  [[nodiscard]] std::string serverBuild() const {
    return locked([&] { return build_; });
  }
  [[nodiscard]] std::string serverContent() const {
    return locked([&] { return content_; });
  }
  [[nodiscard]] int peerGoneFromServer() const {
    return locked([&] { return peerGoneFromServer_; });
  }
  [[nodiscard]] int signalsFromServer() const {
    return locked([&] { return signalsFromServer_; });
  }
  [[nodiscard]] std::optional<std::string> room() const {
    return locked([&] { return room_; });
  }
  [[nodiscard]] size_t clientsInRoom() const {
    return locked([&] { return clients_.size(); });
  }

  // --- what the tests do ---
  // The room expires: room_closed to the server (its clients, if any, get closed).
  void closeRoom() {
    const std::scoped_lock lock(mutex_);
    if (!room_) return;
    for (auto& [peer, c] : clients_)
      closeClient(c, "room_closed");
    clients_.clear();
    const std::string code = room_.value_or("");
    sendServer({{"type", "room_closed"}, {"code", code}});
    room_.reset();
  }
  // The next regions replies carry these keys instead of the state's ("eu"
  // listed once a server registered, free while it has no room); no `free`
  // answers as a v1 lobby does.
  void answerRegions(std::string list, std::optional<std::string> free) {
    const std::scoped_lock lock(mutex_);
    regionsReply_ = std::pair{std::move(list), std::move(free)};
  }
  // The lobby drops the server's socket (as on a lobby restart).
  void dropServer() {
    const std::scoped_lock lock(mutex_);
    if (serverWs_) serverWs_->close();
    serverGone();
  }

 private:
  struct Client {
    std::shared_ptr<rtc::WebSocket> ws;
    std::string peer;
  };

  static void send(const std::shared_ptr<rtc::WebSocket>& ws,
                   std::initializer_list<signaling::Field> fields) {
    try {
      if (ws && ws->isOpen())
        ws->send(signaling::encodeObject(
            std::span<const signaling::Field>(fields.begin(), fields.size())));
    } catch (...) {  // NOLINT(bugprone-empty-catch): the socket went away
    }
  }
  void sendServer(std::initializer_list<signaling::Field> fields) { send(serverWs_, fields); }

  static void closeClient(Client& c, std::string_view reason) {
    send(c.ws, {{"type", "closed"}, {"reason", reason}});
    c.ws->close();
  }

  void serverGone() {
    serverWs_.reset();
    registered_ = false;
    for (auto& [peer, c] : clients_)
      closeClient(c, "server_gone");
    clients_.clear();
    room_.reset();
  }

  [[nodiscard]] static bool hex8(std::string_view s) {
    return s.size() == 8 && std::ranges::all_of(s, [](char c) {
             return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
  }

  void onSocket(const std::shared_ptr<rtc::WebSocket>& ws) {
    {
      const std::scoped_lock lock(mutex_);
      if (stopped_) return;
      sockets_.push_back(ws);
    }
    const std::weak_ptr<rtc::WebSocket> weak = ws;
    // The path is known once the handshake is done (onOpen).
    ws->onOpen([] {});
    ws->onMessage([this, weak](rtc::message_variant m) {
      const auto self = weak.lock();
      const auto* text = std::get_if<std::string>(&m);
      if (!self || text == nullptr) return;
      const std::scoped_lock lock(mutex_);
      if (stopped_) return;
      const std::string path = self->path().value_or("");
      if (path == "/server") {
        onServerText(self, *text);
      } else if (path == "/client") {
        onClientText(self, *text);
      } else {
        self->close();
      }
    });
    ws->onClosed([this, weak] {
      const std::scoped_lock lock(mutex_);
      if (stopped_) return;
      const auto self = weak.lock();
      if (!self) return;
      if (self == serverWs_) {
        serverGone();
        return;
      }
      for (auto it = clients_.begin(); it != clients_.end(); ++it) {
        if (it->second.ws == self) {
          sendServer({{"type", "peer_gone"}, {"peer", it->first}});
          clients_.erase(it);
          return;
        }
      }
    });
  }

  void onServerText(const std::shared_ptr<rtc::WebSocket>& ws, const std::string& text) {
    const auto o = signaling::decodeObject(text);
    if (!o) {
      ws->close();
      return;
    }
    const std::string_view type = o->get("type").value_or("");
    if (type == "register") {
      if (o->get("secret") != std::optional<std::string_view>(secret_)) {
        send(ws, {{"type", "refused"}, {"reason", "bad_secret"}});
        ws->close();
        return;
      }
      if (serverWs_ && serverWs_ != ws) serverGone();
      serverWs_ = ws;
      registered_ = true;
      ++registrations_;
      build_ = std::string(o->get("build").value_or(""));
      content_ = std::string(o->get("content").value_or(""));
      capacity_ = std::string(o->get("capacity").value_or(""));
      send(ws, {{"type", "registered"}});
      return;
    }
    if (ws != serverWs_) return;
    if (type == "signal") {
      ++signalsFromServer_;
      const auto it = clients_.find(std::string(o->get("peer").value_or("")));
      if (it != clients_.end()) {
        try {
          it->second.ws->send(std::string(o->get("msg").value_or("")));
        } catch (...) {  // NOLINT(bugprone-empty-catch): the client went away
        }
      }
    } else if (type == "peer_gone") {
      ++peerGoneFromServer_;
      const auto it = clients_.find(std::string(o->get("peer").value_or("")));
      if (it != clients_.end()) {
        Client c = it->second;
        clients_.erase(it);
        closeClient(c, "kicked");
      }
    } else if (type == "capacity") {
      capacity_ = std::string(o->get("capacity").value_or(""));
    }
  }

  void onClientText(const std::shared_ptr<rtc::WebSocket>& ws, const std::string& text) {
    for (auto& [peer, c] : clients_) {
      if (c.ws == ws) {  // after joined: signaling to the server
        sendServer({{"type", "signal"}, {"peer", peer}, {"msg", text}});
        return;
      }
    }
    {
      const signaling::FlatObject o =
          signaling::decodeObject(text).value_or(signaling::FlatObject{});
      if (o.get("type") == std::optional<std::string_view>("regions")) {
        const bool listed = serverWs_ && registered_;
        const bool free = listed && !room_;
        if (regionsReply_ && !regionsReply_->second) {
          send(ws, {{"type", "regions"}, {"list", regionsReply_->first}});
        } else if (regionsReply_) {
          send(ws, {{"type", "regions"},
                    {"list", regionsReply_->first},
                    {"free", regionsReply_->second.value_or("")}});
        } else {
          send(ws, {{"type", "regions"}, {"list", listed ? "eu" : ""}, {"free", free ? "eu" : ""}});
        }
        ws->close();
        return;
      }
    }
    if (const std::optional<std::string_view> reason = admit(text)) {
      send(ws, {{"type", "refused"}, {"reason", *reason}});
      ws->close();
      return;
    }
    const std::string code = room_.value_or("");
    const std::string peer = std::to_string(nextPeer_++);
    // 32 lower-case hex characters, distinct per join.
    std::string token = "0123456789abcdef0123456789abcd";
    token += peer.size() == 1 ? "0" + peer : peer.substr(peer.size() - 2);
    clients_[peer] = Client{ws, peer};
    sendServer({{"type", "peer"}, {"peer", peer}, {"token", token}});
    send(ws, {{"type", "joined"}, {"code", code}, {"token", token}, {"region", "eu"}});
  }

  // A client's request: nullopt when admitted (a create opens the room),
  // else the refusal reason.
  std::optional<std::string_view> admit(const std::string& text) {
    const signaling::FlatObject o = signaling::decodeObject(text).value_or(signaling::FlatObject{});
    const std::string_view type = o.get("type").value_or("");
    const std::string build(o.get("build").value_or(""));
    const std::string content(o.get("content").value_or(""));
    const bool match = hex8(build) && hex8(content) && build == build_ && content == content_;
    if (type == "create") {
      if (!serverWs_ || !registered_ || room_ ||
          o.get("region") != std::optional<std::string_view>("eu"))
        return "no_server";
      if (!match) return "mismatch";
      static constexpr std::array<const char*, 4> kCodes = {"ABCDE", "FGHJK", "LMNPQ", "RSTUV"};
      room_ = kCodes[roomsCreated_++ % kCodes.size()];
      sendServer({{"type", "room"}, {"code", *room_}});
      return std::nullopt;
    }
    if (type == "join") {
      if (!room_ || o.get("code") != std::optional<std::string_view>(*room_)) return "unknown_code";
      if (!match) return "mismatch";
      return std::nullopt;
    }
    return "bad_request";
  }

  mutable std::recursive_mutex mutex_;
  std::string secret_;
  std::unique_ptr<rtc::WebSocketServer> server_;
  std::vector<std::shared_ptr<rtc::WebSocket>> sockets_;
  bool stopped_{false};
  std::shared_ptr<rtc::WebSocket> serverWs_;
  bool registered_{false};
  int registrations_{0};
  std::string build_;
  std::string content_;
  std::string capacity_;
  int peerGoneFromServer_{0};
  int signalsFromServer_{0};
  std::optional<std::string> room_;
  std::optional<std::pair<std::string, std::optional<std::string>>> regionsReply_;
  uint32_t roomsCreated_{0};
  uint32_t nextPeer_{1};
  std::map<std::string, Client> clients_;
};

}  // namespace kfm::net::test
