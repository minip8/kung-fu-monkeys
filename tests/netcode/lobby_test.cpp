// The lobby seam end to end (M6a, docs/lobby-protocol.md), label `network`:
// a FakeLobby on 127.0.0.1, kfm_server's pieces (an external-signaling
// WebRtcTransport, net::LobbyLink, ServerHost, LobbyAdapter) and clients on
// WebRtcTransport's lobby mode with NetClients, one process, steady clock.
#include "fake_lobby.hpp"
#include "kfm/net/lobby_link.hpp"
#include "kfm/net/net_client.hpp"
#include "kfm/net/webrtc_signaling.hpp"
#include "kfm/net/webrtc_transport.hpp"
#include "kfm/sim/content.hpp"
#include "server_host.hpp"
#include "server_lobby.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace kfm;
using namespace kfm::net;

namespace {

constexpr const char* kSecret = "test-secret";

[[nodiscard]] uint64_t nowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

// kfm_server --lobby, in-process.
struct LobbyServer {
  std::unique_ptr<WebRtcTransport> transport;
  std::unique_ptr<LobbyLink> link;
  std::unique_ptr<server::ServerHost> host;
  std::unique_ptr<server::LobbyAdapter> adapter;
  std::vector<std::string> lines;

  LobbyServer(const std::string& lobbyUrl, const std::string& secret, uint8_t bots = 1) {
    WebRtcServerConfig sc;
    sc.externalSignaling = true;
    auto t = WebRtcTransport::createServer(sc);
    REQUIRE(t.has_value());
    transport = std::move(*t);
    CHECK(transport->listenPort() == 0);  // no TCP port in lobby mode

    const uint64_t start = nowNs();
    server::HostConfig hc;
    hc.server.match.seed = 0x10BB7;
    hc.server.maxPlayers = 4;
    hc.bots = server::referenceBots(bots);
    hc.log = [this](std::string_view l) { lines.emplace_back(l); };

    LobbyLinkConfig lc;
    lc.url = lobbyUrl;  // the base URL: LobbyLink appends /server
    lc.secret = secret;
    lc.region = "eu";
    lc.name = "test:1";
    lc.buildId = hc.server.buildId;
    lc.contentHash = sim::contentHash(sim::defaultContent());
    lc.capacity = server::lobbyCapacity(hc.server.maxPlayers, bots);
    lc.reconnectMinMs = 100;
    lc.reconnectMaxMs = 400;
    auto l = LobbyLink::create(*transport, lc, start);
    REQUIRE(l.has_value());
    link = std::move(*l);
    hc.server.acceptToken = server::lobbyTokenCheck(*link);
    hc.statsSuffix = server::lobbyStatsSuffix(*link);

    auto h = server::ServerHost::create(*transport, sim::defaultContent(), std::move(hc), start);
    if (!h) FAIL(h.error());
    host = std::move(*h);
    adapter = std::make_unique<server::LobbyAdapter>(*host, *link, uint8_t{4});
  }

  void frame(uint64_t now) const {
    host->frame(now);
    adapter->frame(now);
  }
  [[nodiscard]] std::string log() const {
    std::string all;
    for (const std::string& l : lines)
      all += l + "\n";
    return all;
  }
};

// A browser/native client in lobby mode.
struct LobbyClient {
  std::unique_ptr<WebRtcTransport> transport;
  std::unique_ptr<NetClient> client;
  bool tokenSet{false};
  uint32_t disconnects{0};

  LobbyClient(const std::string& lobbyUrl, LobbyRequest request) {
    WebRtcClientConfig cc;
    cc.signalingUrl = lobbyUrl;  // the base URL: the transport appends /client
    request.contentHash = sim::contentHash(sim::defaultContent());
    cc.lobby = std::move(request);
    auto t = WebRtcTransport::createClient(cc);
    REQUIRE(t.has_value());
    transport = std::move(*t);
    client = std::make_unique<NetClient>(sim::defaultContent(), ClientConfig{});
    client->connect(*transport, nowNs());
  }

  void frame(uint64_t now) {
    // The lobby's token reaches the NetClient before its Hello goes out.
    if (!tokenSet && transport->lobbyRoom()) {
      client->setToken(transport->connectionToken());
      tokenSet = true;
    }
    NetClient& c = *client;
    c.beginFrame();
    c.pump(now);
    c.reconcile();
    for (uint32_t n = c.ticksDue(now); n > 0; --n)
      c.tick({});
    static_cast<void>(c.drainEvents());
  }
};

struct World {
  test::FakeLobby lobby{kSecret};
  std::unique_ptr<LobbyServer> server;
  std::vector<std::unique_ptr<LobbyClient>> clients;

  void frame() {
    const uint64_t now = nowNs();
    if (server) server->frame(now);
    for (auto& c : clients)
      if (c) c->frame(now);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  bool runUntil(const std::function<bool()>& done, uint64_t timeoutNs = 20'000'000'000ull) {
    const uint64_t end = nowNs() + timeoutNs;
    while (nowNs() < end) {
      frame();
      if (done()) return true;
    }
    return done();
  }
  void runFor(uint64_t ns) {
    const uint64_t end = nowNs() + ns;
    while (nowNs() < end)
      frame();
  }
  LobbyClient& create() {
    LobbyRequest r;
    r.kind = LobbyRequest::Kind::Create;
    r.region = "eu";
    clients.push_back(std::make_unique<LobbyClient>(lobby.url(), r));
    return *clients.back();
  }
  LobbyClient& join(std::string code) {
    LobbyRequest r;
    r.kind = LobbyRequest::Kind::Join;
    r.code = std::move(code);
    clients.push_back(std::make_unique<LobbyClient>(lobby.url(), r));
    return *clients.back();
  }
  // Server registered, one client created a room and is running.
  LobbyClient& startRoom() {
    server = std::make_unique<LobbyServer>(lobby.url(), kSecret);
    REQUIRE(runUntil([&] { return server->link->state() == LobbyLinkState::Registered; }));
    LobbyClient& c = create();
    const bool running = runUntil([&] { return c.client->running(); });
    INFO("client: " << c.transport->lastError() << "\nserver log:\n" << server->log());
    REQUIRE(running);
    return c;
  }
};

}  // namespace

TEST_CASE("lobby: the server registers and a created room plays", "[lobby][webrtc]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  World w;
  LobbyClient& c = w.startRoom();

  // Register carried the formats the lobby compares (8 lower-case hex digits).
  CHECK(w.lobby.registrations() == 1);
  CHECK(w.lobby.serverBuild() == "00000000");
  CHECK(w.lobby.serverContent() == std::format("{:08x}", sim::contentHash(sim::defaultContent())));
  CHECK(w.lobby.serverCapacity() == "3");  // 4 players minus 1 bot

  const std::optional<LobbyRoom> room = c.transport->lobbyRoom();
  REQUIRE(room.has_value());
  CHECK(room.value_or(LobbyRoom{}).code == "ABCDE");
  CHECK(room.value_or(LobbyRoom{}).region == "eu");
  CHECK(c.transport->connectionToken().size() == 32);
  CHECK(w.server->link->room() == std::optional<std::string>("ABCDE"));
  CHECK(w.server->link->peerCount() == 1);
  CHECK(w.server->adapter->roomsOpened() == 1);
  CHECK(w.server->adapter->restarts() == 1);  // nobody was connected
  CHECK(w.lobby.signalsFromServer() >= 1);    // at least the answer

  // NetServer accepted the Hello with the lobby's token, and nothing else.
  const ServerStats& ss = w.server->host->server().stats();
  CHECK(ss.joins == 1);
  CHECK(ss.rejects == 0);
  const std::string token = "0123456789abcdef0123456789abcd01";
  CHECK(w.server->link->acceptsToken(std::as_bytes(std::span(token.data(), token.size()))));
  const std::string other = "ffffffffffffffffffffffffffffffff";
  CHECK_FALSE(w.server->link->acceptsToken(std::as_bytes(std::span(other.data(), other.size()))));

  // A second client joins the room by code (any case).
  LobbyClient& d = w.join("ABCDE");
  REQUIRE(w.runUntil([&] { return d.client->running(); }));
  CHECK(w.server->host->server().stats().joins == 2);
  CHECK(w.server->link->peerCount() == 2);
  CHECK(w.server->adapter->restarts() == 1);

  // v1.3: the server named its version, and reports its connected players
  // to a lobby that takes them.
  CHECK(w.lobby.serverVersion() == "1.3");
  CHECK(w.server->link->lobbySpeaksV13());
  REQUIRE(w.runUntil([&] { return w.lobby.players() == "2"; }));

  // The stats line carries the lobby state.
  w.server->host->logStats(nowNs());
  CHECK(w.server->lines.back().find("| lobby registered room=ABCDE peers=2 capacity=3") !=
        std::string::npos);
}

TEST_CASE("lobby: players follow the game connections; a v1.2 lobby hears none (v1.3)",
          "[lobby][webrtc]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  World w;
  w.startRoom();
  REQUIRE(w.runUntil([&] { return w.lobby.players() == "1"; }));
  const int sent = w.lobby.playersMessages();
  w.runFor(200'000'000);
  CHECK(w.lobby.playersMessages() == sent);  // only on a change
  w.clients[0].reset();
  REQUIRE(w.runUntil([&] { return w.lobby.players() == "0"; }));

  // An older lobby gets no `players` (it would take the type as a protocol
  // error), and the link says so.
  World old;
  old.lobby.asLegacyLobby();
  old.startRoom();
  CHECK_FALSE(old.server->link->lobbySpeaksV13());
  old.runFor(300'000'000);
  CHECK(old.lobby.playersMessages() == 0);
  CHECK(old.server->link->state() == LobbyLinkState::Registered);
}

TEST_CASE("lobby: a refused join is Disconnected with the reason", "[lobby][webrtc]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  World w;
  w.server = std::make_unique<LobbyServer>(w.lobby.url(), kSecret);
  REQUIRE(w.runUntil([&] { return w.server->link->state() == LobbyLinkState::Registered; }));
  LobbyClient& c = w.join("ZZZZZ");
  REQUIRE(w.runUntil([&] { return c.client->state() == ClientState::Disconnected; }));
  CHECK(c.transport->lastError() == "lobby refused: unknown_code");
  CHECK_FALSE(c.transport->lobbyRoom().has_value());
  CHECK(c.transport->connectionToken().empty());
}

namespace {
[[nodiscard]] LobbyRequest regionsRequest() {
  LobbyRequest r;
  r.kind = LobbyRequest::Kind::Regions;
  return r;
}
}  // namespace

TEST_CASE("lobby: a regions query lists the lobby's regions", "[lobby][webrtc]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  World w;
  const auto query = [&] {
    w.clients.push_back(std::make_unique<LobbyClient>(w.lobby.url(), regionsRequest()));
    LobbyClient& c = *w.clients.back();
    REQUIRE(w.runUntil([&] { return c.client->state() == ClientState::Disconnected; }));
    CHECK(c.transport->lastError().empty());  // the close after the reply is no error
    CHECK_FALSE(c.transport->lobbyRoom().has_value());
    return c.transport->lobbyRegions();
  };
  using Regions = std::vector<LobbyRegion>;
  const Regions unset{LobbyRegion{.name = "unset"}};

  // No server yet: the list is empty.
  CHECK(query().value_or(unset).empty());

  // A registered server with no room: eu listed and free.
  w.server = std::make_unique<LobbyServer>(w.lobby.url(), kSecret);
  REQUIRE(w.runUntil([&] { return w.server->link->state() == LobbyLinkState::Registered; }));
  CHECK(query().value_or(unset) == Regions{{.name = "eu", .free = true}});

  // v1.1: every listed region, free only when named in `free`; an item of
  // `free` that is not listed is ignored, empty items are skipped.
  w.lobby.answerRegions("asia,eu,,us", "us,eu,mars");
  CHECK(query().value_or(unset) == Regions{{.name = "asia", .free = false},
                                           {.name = "eu", .free = true},
                                           {.name = "us", .free = true}});
  w.lobby.answerRegions("eu,us", "");
  CHECK(query().value_or(unset) ==
        Regions{{.name = "eu", .free = false}, {.name = "us", .free = false}});

  // A v1 lobby sends no `free`: availability unknown, every region assumed free.
  w.lobby.answerRegions("eu,us", std::nullopt);
  CHECK(query().value_or(unset) ==
        Regions{{.name = "eu", .free = true}, {.name = "us", .free = true}});
}

TEST_CASE("lobby: a busy region stays listed and refuses a create", "[lobby][webrtc]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  World w;
  w.startRoom();  // the fake lobby's only server (eu) now hosts a room
  w.clients.push_back(std::make_unique<LobbyClient>(w.lobby.url(), regionsRequest()));
  LobbyClient& q = *w.clients.back();
  REQUIRE(w.runUntil([&] { return q.client->state() == ClientState::Disconnected; }));
  CHECK(q.transport->lobbyRegions().value_or(std::vector<LobbyRegion>{}) ==
        std::vector<LobbyRegion>{{.name = "eu", .free = false}});

  // A client may still try the busy region: refused no_server, as in v1.
  LobbyClient& late = w.create();
  REQUIRE(w.runUntil([&] { return late.client->state() == ClientState::Disconnected; }));
  CHECK(late.transport->lastError() == "lobby refused: no_server");
}

TEST_CASE("lobby: peers leave both ways", "[lobby][webrtc]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  World w;
  w.startRoom();
  LobbyClient& d = w.join("ABCDE");
  REQUIRE(w.runUntil([&] { return d.client->running(); }));
  REQUIRE(w.server->host->server().peers().size() == 2);

  // A client closing its lobby socket becomes peer_gone; the server drops it.
  // Its data channels close at the same time, so the server may notice the
  // loss first and report it to the lobby itself (at most once; the lobby
  // ignores a peer_gone for a peer it has already forgotten).
  w.clients[0].reset();
  REQUIRE(w.runUntil([&] { return w.server->host->server().peers().size() == 1; }));
  CHECK(w.server->link->peerCount() == 1);
  CHECK(w.lobby.peerGoneFromServer() <= 1);
  const int reportedSoFar = w.lobby.peerGoneFromServer();

  // The server dropping a peer is peer_gone to the lobby, which closes the
  // client with `kicked`.
  const PeerId peer = w.server->host->server().peers().front().peer;
  w.server->transport->disconnect(peer);
  REQUIRE(w.runUntil([&] { return d.client->state() == ClientState::Disconnected; }));
  CHECK(w.lobby.peerGoneFromServer() == reportedSoFar + 1);
  CHECK(w.server->link->peerCount() == 0);
  // (The client may see its data channels close before the lobby's
  // `kicked` frame arrives, so its lastError() is not checked here.)
  CHECK(w.lobby.clientsInRoom() == 0);
}

TEST_CASE("lobby: a dropped client rejoins with its token and reclaims its slot (v1.2)",
          "[lobby][webrtc]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  World w;
  LobbyClient& c = w.startRoom();
  const sim::PlayerMask slots = c.client->localSlots();
  REQUIRE(slots != 0);
  const std::span<const std::byte> t = c.transport->connectionToken();
  const std::string token(reinterpret_cast<const char*>(t.data()), t.size());
  REQUIRE(token.size() == 32);

  // The client vanishes (its lobby socket and data channels close): the
  // server keeps its slots reserved for the reconnect grace.
  w.clients[0].reset();
  REQUIRE(w.runUntil([&] { return w.server->host->server().peers().empty(); }));
  CHECK(w.server->host->server().humanSlots() == slots);

  // Reconnect from the UI: a rejoin through the lobby with the same token.
  LobbyRequest r;
  r.kind = LobbyRequest::Kind::Join;
  r.code = "ABCDE";
  r.token = token;
  w.clients.push_back(std::make_unique<LobbyClient>(w.lobby.url(), r));
  LobbyClient& again = *w.clients.back();
  REQUIRE(w.runUntil([&] { return again.client->running(); }));
  const std::span<const std::byte> t2 = again.transport->connectionToken();
  CHECK(std::string(reinterpret_cast<const char*>(t2.data()), t2.size()) == token);
  CHECK(again.client->localSlots() == slots);
  CHECK(w.server->host->server().stats().reconnects == 1);
  CHECK(w.server->host->server().stats().joins == 1);
  CHECK(w.lobby.joins() == 2);

  // A token the room never issued is refused.
  LobbyRequest bad = r;
  bad.token = "ffffffffffffffffffffffffffffffff";
  w.clients.push_back(std::make_unique<LobbyClient>(w.lobby.url(), bad));
  LobbyClient& refused = *w.clients.back();
  REQUIRE(w.runUntil([&] { return refused.client->state() == ClientState::Disconnected; }));
  CHECK(refused.transport->lastError() == "lobby refused: bad_token");
}

TEST_CASE("lobby: a new room after room_closed restarts the match", "[lobby][webrtc]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  World w;
  w.startRoom();
  w.clients.clear();
  REQUIRE(w.runUntil([&] { return w.server->host->server().peers().empty(); }));
  const uint32_t matchBefore = w.server->host->server().matchIndex();

  w.lobby.closeRoom();
  REQUIRE(w.runUntil([&] { return !w.server->link->room().has_value(); }));
  CHECK(w.server->log().find("lobby room ABCDE closed") != std::string::npos);

  LobbyClient& c = w.create();
  REQUIRE(w.runUntil([&] { return c.client->running(); }));
  CHECK(c.transport->lobbyRoom().value_or(LobbyRoom{}).code == "FGHJK");
  CHECK(w.server->adapter->roomsOpened() == 2);
  CHECK(w.server->adapter->restarts() == 2);
  CHECK(w.server->host->server().matchIndex() == matchBefore + 1);
  CHECK(w.server->log().find("new match: room FGHJK") != std::string::npos);
}

namespace {
[[nodiscard]] std::string tokenOf(const LobbyClient& c) {
  const std::span<const std::byte> t = c.transport->connectionToken();
  return {reinterpret_cast<const char*>(t.data()), t.size()};
}
}  // namespace

TEST_CASE("lobby: the server's lost link ends no match; it claims its room back (v1.3)",
          "[lobby][webrtc]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  World w;
  LobbyClient& c = w.startRoom();
  w.lobby.dropServer();
  // The lobby closes the client's socket with server_gone; the client's data
  // channels are open, so it plays on, and so does the server, which keeps
  // the peer and registers again after the backoff, claiming its room.
  REQUIRE(w.runUntil([&] {
    return w.lobby.registrations() == 2 && w.server->link->state() == LobbyLinkState::Registered;
  }));
  // reconnects() counts connection attempts after the first: a reconnect
  // whose handshake fails on a busy machine is retried after the backoff,
  // so it may be more than one. registrations() == 2 above pins that the
  // lobby saw exactly one new registration.
  CHECK(w.server->link->reconnects() >= 1);
  CHECK(w.lobby.claims() == 1);
  CHECK(w.lobby.claimedRoom() == "ABCDE");
  CHECK(w.lobby.claimedTokens() == tokenOf(c));
  CHECK(w.lobby.claimedPlayers() == "1");
  CHECK(w.server->link->room() == std::optional<std::string>("ABCDE"));
  CHECK(w.server->link->peerCount() == 1);
  CHECK(w.server->link->keptPeerCount() == 1);
  CHECK(w.server->log().find("lobby link lost") != std::string::npos);
  CHECK(w.server->log().find("1 connected peer(s) kept") != std::string::npos);
  CHECK(w.server->log().find("lobby room ABCDE adopted again") != std::string::npos);
  w.runFor(300'000'000);
  CHECK(c.client->running());
  CHECK(c.transport->lastError().empty());  // server_gone is no error once connected
  CHECK(w.server->host->server().peers().size() == 1);

  // The kept player leaving is reported as players 0; then the room can
  // close and the server serves a new one.
  w.clients.clear();
  REQUIRE(w.runUntil([&] { return w.server->host->server().peers().empty(); }));
  REQUIRE(w.runUntil([&] { return w.lobby.players() == "0"; }));
  CHECK(w.server->link->keptPeerCount() == 0);
  w.lobby.closeRoom();
  REQUIRE(w.runUntil([&] { return !w.server->link->room().has_value(); }));
  LobbyClient& d = w.create();
  REQUIRE(w.runUntil([&] { return d.client->running(); }));
}

TEST_CASE("lobby: a lobby restart under a live room; the match plays on and rejoins work (v1.3)",
          "[lobby][webrtc]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  World w;
  LobbyClient& c = w.startRoom();
  LobbyClient& d = w.join("ABCDE");
  REQUIRE(w.runUntil([&] { return d.client->running(); }));
  const std::string tokenC = tokenOf(c);
  const std::string tokenD = tokenOf(d);
  const sim::PlayerMask slotsC = c.client->localSlots();

  // Every socket drops at once, no `closed` message: the process died.
  w.lobby.restart();
  REQUIRE(w.runUntil([&] {
    return w.lobby.registrations() == 2 && w.server->link->state() == LobbyLinkState::Registered;
  }));
  CHECK(w.lobby.claimedRoom() == "ABCDE");
  std::string bothTokens = tokenC;
  bothTokens += ',';
  bothTokens += tokenD;
  CHECK(w.lobby.claimedTokens() == bothTokens);  // oldest first
  CHECK(w.lobby.claimedPlayers() == "2");
  CHECK(w.lobby.room() == std::optional<std::string>("ABCDE"));
  CHECK(w.server->link->keptPeerCount() == 2);
  w.runFor(300'000'000);
  CHECK(c.client->running());
  CHECK(d.client->running());
  CHECK(w.server->host->server().peers().size() == 2);
  CHECK(c.transport->lastError().empty());

  // The room takes a new player by its code again...
  LobbyClient& e = w.join("ABCDE");
  REQUIRE(w.runUntil([&] { return e.client->running(); }));
  CHECK(w.server->host->server().peers().size() == 3);
  REQUIRE(w.runUntil([&] { return w.lobby.players() == "3"; }));

  // ... and Reconnect from the UI still works: a kept player whose link dies
  // later rejoins with its token, which the claim re-announced.
  w.clients[0].reset();
  REQUIRE(w.runUntil([&] { return w.server->host->server().peers().size() == 2; }));
  CHECK(w.server->link->keptPeerCount() == 1);
  LobbyRequest r;
  r.kind = LobbyRequest::Kind::Join;
  r.code = "ABCDE";
  r.token = tokenC;
  w.clients.push_back(std::make_unique<LobbyClient>(w.lobby.url(), r));
  LobbyClient& again = *w.clients.back();
  REQUIRE(w.runUntil([&] { return again.client->running(); }));
  CHECK(tokenOf(again) == tokenC);
  CHECK(again.client->localSlots() == slotsC);
  CHECK(w.server->host->server().stats().reconnects == 1);
}

TEST_CASE("lobby: a claim an old lobby does not adopt keeps the players until they leave (v1.3)",
          "[lobby][webrtc]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  for (const bool legacy : {true, false}) {
    INFO((legacy ? "a v1.2 lobby" : "a v1.3 lobby that refuses the claim"));
    World w;
    LobbyClient& c = w.startRoom();
    REQUIRE(w.runUntil([&] { return w.lobby.players() == "1"; }));
    const int playersBefore = w.lobby.playersMessages();
    if (legacy) {
      w.lobby.asLegacyLobby();
    } else {
      w.lobby.refuseClaims();
    }
    w.lobby.restart();
    REQUIRE(w.runUntil([&] {
      return w.lobby.registrations() == 2 && w.server->link->state() == LobbyLinkState::Registered;
    }));
    // Not adopted: the room is closed for the lobby, the match is not, and
    // the server offers no slot while its players are still here.
    REQUIRE(w.runUntil([&] { return w.lobby.serverCapacity() == "0"; }));
    CHECK_FALSE(w.server->link->room().has_value());
    CHECK(w.server->link->holding());
    CHECK(w.server->log().find("lobby room ABCDE closed (its connected players play on)") !=
          std::string::npos);
    w.runFor(300'000'000);
    CHECK(c.client->running());
    // A v1.2 lobby hears no `players` (it would be a protocol error there).
    if (legacy) CHECK(w.lobby.playersMessages() == playersBefore);

    // The last one leaves: the real capacity is back and the server is free.
    w.clients.clear();
    REQUIRE(w.runUntil([&] { return w.lobby.serverCapacity() == "3"; }));
    CHECK_FALSE(w.server->link->holding());
    CHECK(w.server->link->peerCount() == 0);
    LobbyClient& d = w.create();
    REQUIRE(w.runUntil([&] { return d.client->running(); }));
    CHECK(w.server->adapter->restarts() == 2);  // a fresh match for the new room
  }
}

TEST_CASE("lobby: a frame the server cannot use is ignored, the room plays on", "[lobby][webrtc]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  World w;
  LobbyClient& c = w.startRoom();
  LobbyClient& d = w.join("ABCDE");
  REQUIRE(w.runUntil([&] { return d.client->running(); }));

  // An oversized frame (a v1.2 lobby could relay one), one that is not a flat
  // object, and known messages missing their keys: each concerns one peer at
  // most, so none drops the link and every player with it.
  w.lobby.sendServerRaw(signaling::encodeObject(std::array<signaling::Field, 3>{
      signaling::Field{"type", "signal"}, signaling::Field{"peer", "1"},
      signaling::Field{"msg", std::string(size_t{100} * 1024, 'v')}}));
  w.lobby.sendServerRaw(R"({"type":"signal","peer":"1","msg":"{\"type\":\"offer\",)");
  w.lobby.sendServerRaw(R"({"type":"signal","peer":"1"})");
  w.lobby.sendServerRaw(R"({"type":"peer","peer":"9"})");
  REQUIRE(w.runUntil([&] { return w.server->link->ignoredFrames() == 4; }));
  w.runFor(300'000'000);
  CHECK(w.server->link->state() == LobbyLinkState::Registered);
  CHECK(w.server->link->reconnects() == 0);
  CHECK(w.lobby.registrations() == 1);
  CHECK(w.server->link->room() == std::optional<std::string>("ABCDE"));
  CHECK(w.server->link->peerCount() == 2);
  CHECK(c.client->running());
  CHECK(d.client->running());
  CHECK(w.server->host->server().peers().size() == 2);
  CHECK(w.server->link->lastError().starts_with("ignored a lobby frame"));
}

TEST_CASE("lobby: a wrong secret is refused and retried", "[lobby][webrtc]") {
  WebRtcTransport::initLogging(WebRtcLogLevel::Error);
  World w;
  w.server = std::make_unique<LobbyServer>(w.lobby.url(), "wrong");
  REQUIRE(w.runUntil([&] { return w.server->link->reconnects() >= 2; }));
  CHECK(w.server->link->state() != LobbyLinkState::Registered);
  CHECK(w.server->log().find("lobby refused the registration: bad_secret") != std::string::npos);
  CHECK(w.server->link->lastError() == "lobby refused: bad_secret");
}
