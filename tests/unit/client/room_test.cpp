// The client's room join flow (M6a, architecture §13.5, docs/lobby-protocol.md
// §3): the --lobby / --room / --create / --region flags, the Play online
// screen (v0.43; the lobby panel's Online section until then), the invite text, the status wording
// of every lobby refusal, and the NetSession's lobby pattern (the room's token reaches the Hello)
// over the loopback emulator with a fake lobby-mode transport.
#include "core/client_options.hpp"
#include "core/lobby_menu.hpp"
#include "core/lobby_settings.hpp"
#include "core/net_session.hpp"
#include "core/online_menu.hpp"
#include "kfm/net/loopback.hpp"
#include "kfm/net/net_server.hpp"
#include "kfm/platform/input_event.hpp"
#include "kfm/platform/input_sampler.hpp"
#include "kfm/sim/content.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace kfm;
using client::ClientOptions;
using client::LobbyCommand;
using client::LobbyTarget;
using client::NetPhase;
using client::NetSession;
using client::NetSessionConfig;
using client::OnlineMenu;
using client::OnlineRow;
using client::RegionChoice;

namespace {

[[nodiscard]] bool parse(std::vector<const char*> argv, ClientOptions& o, std::string& error) {
  argv.insert(argv.begin(), "kfm_client");
  return client::parseClientOptions(std::span<const char* const>(argv.data(), argv.size()), o,
                                    error);
}

[[nodiscard]] std::vector<std::byte> bytes(std::string_view s) {
  std::vector<std::byte> b;
  for (const char c : s)
    b.push_back(static_cast<std::byte>(c));
  return b;
}

}  // namespace

// --- flags -----------------------------------------------------------------------

TEST_CASE("room flags: defaults, --room, --create, --region, --lobby", "[client][room][options]") {
  ClientOptions o;
  CHECK(o.lobby == client::defaultLobbyUrl());
  CHECK(o.room.empty());
  CHECK_FALSE(o.create);
  CHECK(o.region == "eu");
  CHECK_FALSE(o.lobbyMode());
  CHECK_FALSE(o.networkMode());
  CHECK_FALSE(client::lobbyTarget(o).has_value());

  std::string error;
  SECTION("--room joins, normalised to upper case; network mode") {
    REQUIRE(parse({"--lobby", "ws://127.0.0.1:8080", "--room", "abcde"}, o, error));
    CHECK(o.lobby == "ws://127.0.0.1:8080");
    CHECK(o.room == "ABCDE");
    CHECK(o.lobbyMode());
    CHECK(o.networkMode());
    const std::optional<LobbyTarget> t = client::lobbyTarget(o);
    CHECK(t == LobbyTarget{.create = false, .region = "eu", .code = "ABCDE"});
    const NetSessionConfig c = client::netSessionConfig(o, true);
    CHECK(c.serverLabel == "ws://127.0.0.1:8080");
    CHECK(c.lobby == t);
    CHECK(c.requestedSlots == 1);
  }
  SECTION("--create with a region; the browser's ?create&region=us") {
    REQUIRE(parse({"--lobby", "wss://lobby.example", "--create", "--region", "us"}, o, error));
    CHECK(o.create);
    CHECK(o.networkMode());
    CHECK(client::lobbyTarget(o) == LobbyTarget{.create = true, .region = "us", .code = ""});
  }
  SECTION("--token, --spectate, --players and --stun still apply in lobby mode") {
    REQUIRE(parse({"--lobby", "ws://h:1", "--room", "QRSTU", "--spectate", "--players", "2",
                   "--stun", "stun:s:3478", "--token", "t"},
                  o, error));
    const NetSessionConfig c = client::netSessionConfig(o, true);
    CHECK(c.spectate);
    CHECK(c.requestedSlots == 2);
    CHECK(o.stun == "stun:s:3478");
  }
  SECTION("--connect alone stays the direct path") {
    REQUIRE(parse({"--connect", "ws://127.0.0.1:9000"}, o, error));
    CHECK(o.networkMode());
    CHECK_FALSE(o.lobbyMode());
    const NetSessionConfig c = client::netSessionConfig(o, true);
    CHECK(c.serverLabel == "ws://127.0.0.1:9000");
    CHECK_FALSE(c.lobby.has_value());
  }
  SECTION("a lobby URL alone is not network mode (the panel offers rooms)") {
    REQUIRE(parse({"--lobby", "ws://127.0.0.1:8080"}, o, error));
    CHECK_FALSE(o.networkMode());
  }
}

TEST_CASE("room flags: mutual exclusion and bad values", "[client][room][options]") {
  ClientOptions o;
  std::string error;
  CHECK_FALSE(parse({"--lobby", "ws://h:1", "--room", "ABCDE", "--create"}, o, error));
  CHECK(error == "--room and --create exclude each other");
  o = {};
  CHECK_FALSE(parse({"--lobby", "ws://h:1", "--room", "ABCDE", "--connect", "ws://h:2"}, o, error));
  CHECK(error.find("excludes --connect") != std::string::npos);
  o = {};
  CHECK_FALSE(parse({"--create", "--lobby", "ws://h:1", "--connect", "ws://h:2"}, o, error));
  CHECK(error.find("--create") != std::string::npos);
  o = {};
  o.lobby.clear();  // a build without KFM_LOBBY_URL
  CHECK_FALSE(parse({"--create"}, o, error));
  CHECK(error.find("needs a lobby") != std::string::npos);
  o = {};
  CHECK_FALSE(parse({"--lobby", "http://h:1"}, o, error));
  CHECK(error.find("ws://") != std::string::npos);
  CHECK_FALSE(parse({"--room", "ABCD"}, o, error));  // too short
  CHECK(error.find("5-character") != std::string::npos);
  CHECK_FALSE(parse({"--room", "ABCD0"}, o, error));  // 0 is not in the alphabet
  CHECK_FALSE(parse({"--room"}, o, error));
  CHECK(error == "--room needs a value");
  CHECK_FALSE(parse({"--region", "EU"}, o, error));
  CHECK(error.find("--region") != std::string::npos);
  CHECK_FALSE(parse({"--region", ""}, o, error));
}

TEST_CASE("room codes: the lobby's alphabet, any case in, upper case out", "[client][room]") {
  CHECK(client::normalizeRoomCode("abcde") == "ABCDE");
  CHECK(client::normalizeRoomCode("Z2345") == "Z2345");
  CHECK_FALSE(client::normalizeRoomCode("ABCDI").has_value());  // I
  CHECK_FALSE(client::normalizeRoomCode("ABCDO").has_value());  // O
  CHECK_FALSE(client::normalizeRoomCode("ABCD1").has_value());  // 1
  CHECK_FALSE(client::normalizeRoomCode("ABC-E").has_value());
  CHECK_FALSE(client::normalizeRoomCode("ABCDEF").has_value());
  CHECK_FALSE(client::normalizeRoomCode("").has_value());
  CHECK(client::isRegionLabel("eu"));
  CHECK(client::isRegionLabel("us-west"));
  CHECK_FALSE(client::isRegionLabel("Eu"));
}

TEST_CASE("the invite: the page link in the browser, the flags natively", "[client][room]") {
  const client::LobbyRoomInfo room{.code = "ABCDE", .region = "eu"};
  CHECK(client::roomShareText(room, "", "wss://lobby.example") ==
        "--lobby wss://lobby.example --room ABCDE");
  CHECK(client::roomShareText(room, "", "") == "--room ABCDE");
  CHECK(client::roomShareText(room, "https://kfm.example/", "wss://lobby.example") ==
        "https://kfm.example/?room=ABCDE");
  // The creator's own flags stay behind; a development ?lobby= goes along.
  CHECK(client::roomShareText(
            room, "http://127.0.0.1:8000/kfm_client.html?lobby=ws://127.0.0.1:8080&create&overlay",
            "ws://127.0.0.1:8080") ==
        "http://127.0.0.1:8000/kfm_client.html?room=ABCDE&lobby=ws://127.0.0.1:8080");
  CHECK(client::roomShareText(room, "https://kfm.example/?create&region=us#x", "") ==
        "https://kfm.example/?room=ABCDE");
  // v0.43, Copy link / Share: the link when the page is known, else the code.
  CHECK(client::roomInviteLink(room, "https://kfm.example/?create") ==
        "https://kfm.example/?room=ABCDE");
  CHECK(client::roomInviteLink(room, "") == "ABCDE");
}

// --- the Play online screen (v0.43) -------------------------------------------------

TEST_CASE("online menu: the region cycles, Create room and Join make commands",
          "[client][room][online]") {
  OnlineMenu m;
  using Activation = OnlineMenu::Activation;
  const auto row = [](OnlineRow r) { return static_cast<std::size_t>(r); };
  // It opens on Create room, with the first region.
  CHECK(m.focusedRow() == OnlineRow::CreateRoom);
  CHECK(m.regionText() == "eu");
  CHECK_FALSE(m.takeCommand().has_value());

  // Up to the region row; Left/Right cycle (wrapping), also from Create room.
  m.apply(client::MenuAction::Up);
  REQUIRE(m.focusedRow() == OnlineRow::Region);
  CHECK(m.apply(client::MenuAction::Right));
  CHECK(m.regionText() == "us");
  CHECK(m.apply(client::MenuAction::Right));
  CHECK(m.regionText() == "asia");
  CHECK(m.apply(client::MenuAction::Right));  // wraps
  CHECK(m.regionText() == "eu");
  CHECK(m.apply(client::MenuAction::Left));  // wraps back
  CHECK(m.region() == "asia");
  m.apply(client::MenuAction::Down);
  REQUIRE(m.focusedRow() == OnlineRow::CreateRoom);
  CHECK(m.apply(client::MenuAction::Right));
  CHECK(m.region() == "eu");
  CHECK(m.apply(client::MenuAction::Left));
  CHECK(m.region() == "asia");
  // Left/Right elsewhere change nothing.
  m.setFocus(row(OnlineRow::Back));
  CHECK_FALSE(m.apply(client::MenuAction::Right));
  CHECK(m.region() == "asia");

  // Create room: a CreateRoom command, once.
  CHECK(m.activate(row(OnlineRow::CreateRoom)) == Activation::Command);
  CHECK(m.takeCommand() ==
        LobbyCommand{.kind = LobbyCommand::Kind::CreateRoom, .region = "asia", .code = ""});
  CHECK_FALSE(m.takeCommand().has_value());
  // The lobby's own list replaces the default one; the current region is
  // kept when it is listed, else the first; an empty list changes nothing.
  const std::vector<RegionChoice> au = {{.name = "au", .free = true},
                                        {.name = "asia", .free = true}};
  m.setRegions(au);
  CHECK(m.regionText() == "asia");
  const std::vector<RegionChoice> onlyAu = {{.name = "au", .free = true}};
  m.setRegions(onlyAu);
  CHECK(m.regionText() == "au");
  m.setRegions(std::vector<RegionChoice>{});
  CHECK(m.regionText() == "au");
  CHECK_FALSE(m.change(row(OnlineRow::Region), +1));  // a one-entry list: nothing changes
  // A region outside the list (--region xx, a saved one) steps to the first.
  m.setRegion("eu-2");
  CHECK(m.regionText() == "eu-2");
  CHECK(m.change(row(OnlineRow::Region), +1));
  CHECK(m.regionText() == "au");

  // The code field keeps the room alphabet only, upper case, five at most;
  // Join with a valid code makes a JoinRoom, else asks for the field.
  CHECK(m.activate(row(OnlineRow::Code)) == Activation::EditCode);
  m.setCode("hj-k");
  CHECK(m.code() == "HJK");
  CHECK_FALSE(m.codeValid());
  CHECK(m.activate(row(OnlineRow::Join)) == Activation::EditCode);
  CHECK_FALSE(m.takeCommand().has_value());
  m.setCode("hjkmn0OI1pq");  // 0, O, I and 1 are not in the alphabet
  CHECK(m.code() == "HJKMN");
  CHECK(m.codeValid());
  CHECK(m.activate(row(OnlineRow::Join)) == Activation::Command);
  CHECK(m.takeCommand() ==
        LobbyCommand{.kind = LobbyCommand::Kind::JoinRoom, .region = "", .code = "HJKMN"});
  CHECK(m.activate(row(OnlineRow::Back)) == Activation::Back);
  CHECK(m.activate(row(OnlineRow::Region)) == Activation::None);
  CHECK_FALSE(m.takeCommand().has_value());
  // Focus wraps both ways.
  m.setFocus(0);
  m.apply(client::MenuAction::Up);
  CHECK(m.focusedRow() == OnlineRow::Back);
  m.apply(client::MenuAction::Down);
  CHECK(m.focusedRow() == OnlineRow::Region);
}

TEST_CASE("online menu: every region with its availability (lobby v1.1)",
          "[client][room][online]") {
  OnlineMenu m;
  using Activation = OnlineMenu::Activation;
  // Before the lobby answers: kRegions, availability unknown (shown free).
  REQUIRE(m.regions().size() == client::kRegions.size());
  for (const RegionChoice& r : m.regions())
    CHECK(r.free);
  CHECK(m.regionText() == "eu");

  // The lobby lists eu busy and us free: the current region (eu) is listed,
  // so it stays, marked; the list keeps the lobby's order and availability.
  const std::vector<RegionChoice> mixed = {
      {.name = "asia", .free = false}, {.name = "eu", .free = false}, {.name = "us", .free = true}};
  m.setRegions(mixed);
  CHECK(std::ranges::equal(m.regions(), mixed));
  CHECK(m.regionText() == "eu (no server)");
  CHECK(m.region() == "eu");
  // Cycling reaches the busy regions too, and Create still asks for them
  // (the lobby refuses no_server if the region is still busy).
  const auto region = static_cast<std::size_t>(OnlineRow::Region);
  CHECK(m.change(region, +1));
  CHECK(m.regionText() == "us");
  CHECK(m.change(region, +1));
  CHECK(m.regionText() == "asia (no server)");
  CHECK(m.activate(static_cast<std::size_t>(OnlineRow::CreateRoom)) == Activation::Command);
  CHECK(m.takeCommand() ==
        LobbyCommand{.kind = LobbyCommand::Kind::CreateRoom, .region = "asia", .code = ""});

  // A current region the lobby does not list becomes its first free one...
  m.setRegion("au");
  CHECK(m.regionText() == "au");  // unknown to the list: shown bare
  m.setRegions(mixed);
  CHECK(m.region() == "us");
  // ...or its first when none is free.
  m.setRegion("au");
  const std::vector<RegionChoice> busy = {{.name = "eu", .free = false},
                                          {.name = "us", .free = false}};
  m.setRegions(busy);
  CHECK(m.region() == "eu");
  CHECK(m.regionText() == "eu (no server)");
  // A later answer updates the availability in place.
  const std::vector<RegionChoice> euFree = {{.name = "eu", .free = true},
                                            {.name = "us", .free = false}};
  m.setRegions(euFree);
  CHECK(m.regionText() == "eu");
}

TEST_CASE("activation presses: Enter and a bound pad's Start, never other keys",
          "[client][room][lobby]") {
  platform::InputEvent e;
  e.kind = platform::InputEventKind::KeyDown;
  e.key = platform::Key::Enter;
  CHECK(client::isActivateEvent(e, true));
  e.key = platform::Key::KpEnter;
  CHECK(client::isActivateEvent(e, true));
  e.key = platform::Key::J;
  CHECK_FALSE(client::isActivateEvent(e, true));
  e.kind = platform::InputEventKind::PadButtonDown;
  e.button = platform::PadButton::Start;
  CHECK(client::isActivateEvent(e, true));
  CHECK_FALSE(client::isActivateEvent(e, false));  // an unbound pad's press is its join
}

TEST_CASE("the region persists with the settings", "[client][room][settings]") {
  const sim::ContentDB& db = sim::defaultContent();
  ClientOptions o;
  std::vector<std::string> warnings;
  client::applySettingsText("region=asia\n", db, o, warnings);
  CHECK(warnings.empty());
  CHECK(o.region == "asia");
  client::LobbySettings ls;
  std::string error;
  REQUIRE(client::lobbySettingsFrom(o, db, ls, error));
  CHECK(ls.region == "asia");
  ls.region = "us";
  client::storeLobbySettings(ls, db, o);
  CHECK(o.region == "us");
  CHECK(client::settingsText(o).find("region=us\n") != std::string::npos);

  // Any label round-trips (the lobby decides which regions exist).
  o.region = "eu-2";
  REQUIRE(client::lobbySettingsFrom(o, db, ls, error));
  CHECK(ls.region == "eu-2");
  ls.volume = 10;
  client::storeLobbySettings(ls, db, o);
  CHECK(o.region == "eu-2");
  ls.region = "au";
  client::storeLobbySettings(ls, db, o);
  CHECK(o.region == "au");

  warnings.clear();
  client::applySettingsText("region=Mars!\n", db, o, warnings);
  CHECK(warnings.size() == 1);
  CHECK(o.region == "au");
}

// --- status wording -----------------------------------------------------------------

TEST_CASE("lobby refusals and closes in plain words", "[client][room][status]") {
  const LobbyTarget join{.create = false, .region = "", .code = "ABCDE"};
  const LobbyTarget create{.create = true, .region = "eu", .code = ""};
  using client::lobbyErrorText;
  CHECK(lobbyErrorText("lobby refused: unknown_code", join) ==
        "Room ABCDE not found (check the code)");
  CHECK(lobbyErrorText("lobby refused: full", join) == "Room ABCDE is full");
  CHECK(lobbyErrorText("lobby refused: no_server", create) == "No server available in eu");
  CHECK(lobbyErrorText("lobby refused: no_server", join) == "No server available for this room");
  CHECK(lobbyErrorText("lobby refused: mismatch", join) ==
        "Version mismatch: reload the page or update the client");
  CHECK(lobbyErrorText("lobby refused: bad_request", create) ==
        "The lobby did not understand the request");
  CHECK(lobbyErrorText("lobby refused: server_gone", join) == "Room ABCDE's server went away");
  CHECK(lobbyErrorText("lobby refused: something_new", join) == "The lobby refused: something_new");
  CHECK(lobbyErrorText("lobby closed: server_gone", join) == "Room ABCDE's server went away");
  CHECK(lobbyErrorText("lobby closed: kicked", join) == "Removed from the room by its server");
  CHECK(lobbyErrorText("lobby closed: room_closed", join) == "Room ABCDE closed");
  // v1.2 (v0.44): a rejoin's refusal, and the socket a rejoin replaced.
  CHECK(lobbyErrorText("lobby refused: bad_token", join) ==
        "Room ABCDE does not know this player any more");
  CHECK(lobbyErrorText("lobby closed: replaced", join) == "Joined Room ABCDE again from elsewhere");
  // v1.3: an address over the lobby's limits.
  CHECK(lobbyErrorText("lobby refused: limited", create) ==
        "Too many attempts from this network: wait a minute");
  CHECK(lobbyErrorText("lobby connection closed", join) == "Lost the connection to the lobby");
  CHECK(lobbyErrorText("", join).empty());
  CHECK(lobbyErrorText("ICE failed", join).empty());
}

// --- the NetSession's lobby pattern over the loopback -------------------------------

namespace {

constexpr uint64_t kMs = 1'000'000;
constexpr uint64_t kSec = 1'000 * kMs;

// A lobby-mode transport stand-in over a loopback endpoint: invisible
// (nothing connected, nothing received) until the test says the lobby
// answered `joined` (then Connected, as WebRtcTransport reports it once the
// peer connection opens after `joined`) or `refused` (Disconnected, with the
// transport's error string).
class FakeLobbyTransport final : public net::ITransport, public client::ILobbyJoin {
 public:
  explicit FakeLobbyTransport(net::LoopbackTransport& inner) : inner_(inner) {}

  void join(client::LobbyRoomInfo room, std::vector<std::byte> token) {
    room_ = std::move(room);
    token_ = std::move(token);
  }
  void refuse(const std::string& reason) {
    error_ = "lobby refused: " + reason;
    refused_ = true;
  }
  // v1.3: the lobby socket went away under a connected transport, which
  // plays on (WebRtcTransport reports nothing, and may keep an error string).
  void loseLobby(std::string error) { error_ = std::move(error); }

  // ITransport
  void sendUnreliable(net::PeerId peer, std::span<const std::byte> data) override {
    if (open()) inner_.sendUnreliable(peer, data);
  }
  void sendReliable(net::PeerId peer, std::span<const std::byte> data) override {
    if (open()) inner_.sendReliable(peer, data);
  }
  [[nodiscard]] bool recv(net::Packet& out) override { return open() && inner_.recv(out); }
  void poll(uint64_t nowNs, net::ConnectionEvents& events) override {
    net::ConnectionEvents inner;
    inner_.poll(nowNs, inner);
    if (refused_ && !reported_) {
      reported_ = true;
      events.push({net::ConnectionEventKind::Disconnected, net::kServerPeer, nowNs});
      return;
    }
    if (!room_) return;
    if (!announced_ && inner_.isConnected(net::kServerPeer)) {
      announced_ = true;
      events.push({net::ConnectionEventKind::Connected, net::kServerPeer, nowNs});
    }
    for (const net::ConnectionEvent& e : inner.span()) {
      if (e.kind == net::ConnectionEventKind::Disconnected) events.push(e);
    }
  }
  void disconnect(net::PeerId peer) override { inner_.disconnect(peer); }
  [[nodiscard]] bool isConnected(net::PeerId peer) const override {
    return open() && inner_.isConnected(peer);
  }
  [[nodiscard]] net::TransportStats stats() const override { return inner_.stats(); }

  // ILobbyJoin
  [[nodiscard]] std::optional<client::LobbyRoomInfo> room() const override { return room_; }
  [[nodiscard]] std::vector<std::byte> token() const override { return token_; }
  [[nodiscard]] std::string error() const override { return error_; }

 private:
  [[nodiscard]] bool open() const { return room_.has_value() && !refused_ && announced_; }

  net::LoopbackTransport& inner_;
  std::optional<client::LobbyRoomInfo> room_;
  std::vector<std::byte> token_;
  std::string error_;
  bool refused_{false};
  bool reported_{false};
  bool announced_{false};
};

// The lobby view the session owns, forwarding to the test's transport.
class LobbyView final : public client::ILobbyJoin {
 public:
  explicit LobbyView(const FakeLobbyTransport& t) : t_(t) {}
  [[nodiscard]] std::optional<client::LobbyRoomInfo> room() const override { return t_.room(); }
  [[nodiscard]] std::vector<std::byte> token() const override { return t_.token(); }
  [[nodiscard]] std::string error() const override { return t_.error(); }

 private:
  const FakeLobbyTransport& t_;
};

struct LobbyRig {
  net::LoopbackNetwork net{5};
  std::unique_ptr<net::NetServer> server;
  std::unique_ptr<FakeLobbyTransport> transport;
  std::unique_ptr<NetSession> session;
  uint64_t now{0};
  uint64_t serverTicks{0};
  std::vector<std::vector<std::byte>> tokensSeen;

  explicit LobbyRig(const LobbyTarget& target) {
    net::ServerConfig sc;
    sc.acceptToken = [this](std::span<const std::byte> t) {
      tokensSeen.emplace_back(t.begin(), t.end());
      return std::vector<std::byte>(t.begin(), t.end()) == bytes("room-token");
    };
    server = std::make_unique<net::NetServer>(net.server(), sim::defaultContent(), sc);
    net::LoopbackTransport& inner = net.connectClient(now, net::LinkConditions::uniform(kMs),
                                                      net::LinkConditions::uniform(kMs));
    transport = std::make_unique<FakeLobbyTransport>(inner);
    NetSessionConfig config;
    config.serverLabel = "ws://lobby";
    config.lobby = target;
    platform::InputSampler sampler;
    REQUIRE(sampler.bind(PlayerId{0}, platform::DeviceBinding::keyboard(0)));
    session = std::make_unique<NetSession>(config, sampler, transport.get(), now);
    session->attachLobby(std::make_unique<LobbyView>(*transport));
  }

  void step() {
    now += kMs;
    server->pump(now);
    while (now >= serverTicks * kSec / sim::kTickHz) {
      server->tick(now);
      ++serverTicks;
    }
    server->clearEvents();
    if (now % (16 * kMs) == 0) session->advance(now);
  }
  template <class Pred>
  bool runUntil(Pred pred, uint64_t maxNs) {
    const uint64_t end = now + maxNs;
    while (now < end) {
      if (pred()) return true;
      step();
    }
    return pred();
  }
};

}  // namespace

TEST_CASE("a lobby session hands the room's token to its Hello and names the room",
          "[client][room][net]") {
  LobbyRig rig(LobbyTarget{.create = true, .region = "eu", .code = ""});
  NetSession& s = *rig.session;
  // Before the lobby answers: creating, nothing reaches the server.
  for (int i = 0; i < 200; ++i)
    rig.step();
  CHECK(s.phase() == NetPhase::Connecting);
  CHECK(s.statusText() == "Creating a room in eu ...");
  CHECK_FALSE(s.room().has_value());
  CHECK(rig.tokensSeen.empty());

  rig.transport->join({.code = "ABCDE", .region = "eu"}, bytes("room-token"));
  REQUIRE(rig.runUntil([&] { return s.running(); }, 3 * kSec));
  REQUIRE(rig.tokensSeen.size() == 1);
  CHECK(rig.tokensSeen[0] == bytes("room-token"));  // the lobby's, in the first Hello
  REQUIRE(s.room().has_value());
  CHECK(s.room() == client::LobbyRoomInfo{.code = "ABCDE", .region = "eu"});
  CHECK(s.statusText().find("Online on room ABCDE (eu)") != std::string::npos);
  CHECK(s.lobbyErrorText().empty());
}

TEST_CASE("a refused lobby join shows the reason and never says hello", "[client][room][net]") {
  struct Case {
    const char* reason{nullptr};
    LobbyTarget target{};
    const char* text{nullptr};
  };
  const LobbyTarget join{.create = false, .region = "", .code = "ABCDE"};
  const Case cases[] = {
      {"unknown_code", join, "Room ABCDE not found (check the code)"},
      {"full", join, "Room ABCDE is full"},
      {"no_server", LobbyTarget{.create = true, .region = "asia", .code = ""},
       "No server available in asia"},
      {"mismatch", join, "Version mismatch: reload the page or update the client"},
  };
  for (const Case& c : cases) {
    INFO(c.reason);
    LobbyRig rig(c.target);
    NetSession& s = *rig.session;
    if (!c.target.create) CHECK(s.statusText() == "Joining room ABCDE ...");
    rig.transport->refuse(c.reason);
    REQUIRE(rig.runUntil([&] { return s.phase() == NetPhase::Disconnected; }, kSec));
    CHECK(s.statusText() == c.text);
    CHECK(s.lobbyErrorText() == c.text);
    CHECK(rig.tokensSeen.empty());
    CHECK_FALSE(s.everRan());
  }
}

TEST_CASE("reconnect from the UI in a room: a lobby rejoin with the room's token (v0.44)",
          "[client][room][net][reconnect]") {
  LobbyRig rig(LobbyTarget{.create = true, .region = "eu", .code = ""});
  NetSession& s = *rig.session;
  rig.transport->join({.code = "ABCDE", .region = "eu"}, bytes("room-token"));
  REQUIRE(rig.runUntil([&] { return s.running(); }, 3 * kSec));
  const sim::PlayerMask slots = s.localSlots();
  CHECK(slots == 0b1);
  CHECK_FALSE(s.canReconnect());

  // The link dies; both ends time out and the server keeps the slot.
  rig.net.cut(net::PeerId{1});  // the rig's only client
  REQUIRE(rig.runUntil([&] { return s.phase() == NetPhase::Disconnected; }, 7 * kSec));
  REQUIRE(rig.runUntil([&] { return rig.server->peers().empty(); }, kSec));
  CHECK(s.canReconnect());
  const NetSessionConfig again = s.reconnectConfig();
  REQUIRE(again.lobby.has_value());
  CHECK(again.lobby == LobbyTarget{.create = false, .region = "eu", .code = "ABCDE"});
  CHECK(again.rejoinToken == "room-token");
  CHECK(again.token == bytes("room-token"));

  // The button: a new lobby transport that rejoins and gets the same token.
  net::LoopbackTransport& inner = rig.net.connectClient(rig.now, net::LinkConditions::uniform(kMs),
                                                        net::LinkConditions::uniform(kMs));
  auto fresh = std::make_unique<FakeLobbyTransport>(inner);
  FakeLobbyTransport* raw = fresh.get();
  s.reconnect(raw, rig.now, std::move(fresh));
  s.attachLobby(std::make_unique<LobbyView>(*raw));
  CHECK(s.statusText() == "Joining room ABCDE ...");
  raw->join({.code = "ABCDE", .region = "eu"}, bytes("room-token"));
  REQUIRE(rig.runUntil([&] { return s.running(); }, 3 * kSec));
  CHECK(s.localSlots() == slots);
  CHECK(rig.server->stats().reconnects == 1);
  CHECK(s.room() == client::LobbyRoomInfo{.code = "ABCDE", .region = "eu"});
}

TEST_CASE(
    "a lobby that goes away under a running match ends nothing; Reconnect still rejoins (v1.3)",
    "[client][room][net][reconnect]") {
  LobbyRig rig(LobbyTarget{.create = true, .region = "eu", .code = ""});
  NetSession& s = *rig.session;
  rig.transport->join({.code = "ABCDE", .region = "eu"}, bytes("room-token"));
  REQUIRE(rig.runUntil([&] { return s.running(); }, 3 * kSec));
  const sim::PlayerMask slots = s.localSlots();

  // The lobby restarts: the session goes on over its data channels, and the
  // status keeps naming the room.
  rig.transport->loseLobby("lobby connection closed");
  for (int i = 0; i < 2000; ++i)
    rig.step();
  CHECK(s.running());
  CHECK(s.statusText().find("Online on room ABCDE (eu)") != std::string::npos);
  CHECK_FALSE(s.canReconnect());

  // The link dies later: Reconnect is the same rejoin with the room's token,
  // which the server re-announced when it claimed the room back.
  rig.net.cut(net::PeerId{1});
  REQUIRE(rig.runUntil([&] { return s.phase() == NetPhase::Disconnected; }, 7 * kSec));
  REQUIRE(rig.runUntil([&] { return rig.server->peers().empty(); }, kSec));
  CHECK(s.canReconnect());
  const NetSessionConfig again = s.reconnectConfig();
  CHECK(again.lobby == LobbyTarget{.create = false, .region = "eu", .code = "ABCDE"});
  CHECK(again.rejoinToken == "room-token");
  net::LoopbackTransport& inner = rig.net.connectClient(rig.now, net::LinkConditions::uniform(kMs),
                                                        net::LinkConditions::uniform(kMs));
  auto fresh = std::make_unique<FakeLobbyTransport>(inner);
  FakeLobbyTransport* raw = fresh.get();
  s.reconnect(raw, rig.now, std::move(fresh));
  s.attachLobby(std::make_unique<LobbyView>(*raw));
  raw->join({.code = "ABCDE", .region = "eu"}, bytes("room-token"));
  REQUIRE(rig.runUntil([&] { return s.running(); }, 3 * kSec));
  CHECK(s.localSlots() == slots);
}
