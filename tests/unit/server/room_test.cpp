// The Room state machine on its own (apps/server/room.hpp, architecture
// §6.6, v0.42): members and the host role, settings validation, Start in
// each state, the match end and the return to Waiting.
#include "room.hpp"

#include "kfm/net/messages.hpp"
#include "kfm/sim/content.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace kfm;
using kfm::net::RoomPhase;
using kfm::net::RoomSettings;
using kfm::server::Room;

namespace {

// Two arenas and a team mode only the second suits.
sim::ContentDB teamContent() {
  sim::ContentDB db = sim::defaultContent();
  db.arenas[1] = db.arenas[0];
  db.arenas[1].name = sim::ContentName::of("two_sides");
  db.arenas[1].spawnGroups = 2;
  db.arenas[0].spawnGroups = 0;
  db.arenaCount = 2;
  db.modes[1] = db.modes[0];
  db.modes[1].name = sim::ContentName::of("team_rounds");
  db.modes[1].teams = 2;
  db.modeCount = 2;
  return db;
}

}  // namespace

TEST_CASE("Room: the first human is host, then the longest present", "[server][room]") {
  const sim::ContentDB db = sim::defaultContent();
  Room room(db, sim::kMaxPlayers, RoomSettings{});
  CHECK(room.phase() == RoomPhase::Waiting);
  CHECK(!room.host().isValid());
  CHECK_FALSE(room.updateMembers(0));

  CHECK(room.updateMembers(0b0100));
  CHECK(room.host() == PlayerId{2});
  CHECK(room.updateMembers(0b0111));  // two more, ascending after the host
  CHECK(room.host() == PlayerId{2});
  CHECK(room.member(1) == PlayerId{0});
  CHECK(room.member(2) == PlayerId{1});
  CHECK_FALSE(room.updateMembers(0b0111));

  CHECK(room.isHost(0b0100));
  CHECK(room.isHost(0b1100));  // a peer with several slots, one of them the host's
  CHECK_FALSE(room.isHost(0b0001));

  CHECK(room.updateMembers(0b0011));  // the host left: slot 0 joined next
  CHECK(room.host() == PlayerId{0});
  CHECK(room.updateMembers(0b1011));  // a newcomer does not jump the queue
  CHECK(room.host() == PlayerId{0});
  CHECK(room.updateMembers(0b1000));
  CHECK(room.host() == PlayerId{3});
  CHECK(room.updateMembers(0));
  CHECK(!room.host().isValid());
  CHECK(room.updateMembers(0b1));  // the next human becomes host
  CHECK(room.host() == PlayerId{0});
}

TEST_CASE("Room: only the host's valid settings apply, and only while waiting", "[server][room]") {
  const sim::ContentDB db = teamContent();
  Room room(db, 4, RoomSettings{});
  room.updateMembers(0b11);
  const RoomSettings teams{.arena = 1, .mode = 1, .loadout = 0, .powerUps = 0, .bots = 2};
  CHECK_FALSE(room.requestSettings(0b10, teams));  // not the host
  CHECK(room.settings() == RoomSettings{});
  CHECK(room.requestSettings(0b01, teams));
  CHECK(room.settings() == teams);

  // Out of range, incompatible, too many bots: refused, settings unchanged.
  RoomSettings bad = teams;
  bad.arena = 2;
  CHECK_FALSE(room.requestSettings(0b01, bad));
  bad = teams;
  bad.arena = 0;  // no spawn groups: the team mode does not suit it
  CHECK_FALSE(room.requestSettings(0b01, bad));
  bad = teams;
  bad.mode = 2;
  CHECK_FALSE(room.requestSettings(0b01, bad));
  bad = teams;
  bad.loadout = static_cast<uint8_t>(db.loadoutCount);
  CHECK_FALSE(room.requestSettings(0b01, bad));
  bad = teams;
  bad.bots = 4;  // maxPlayers 4: one slot stays a human's
  CHECK_FALSE(room.requestSettings(0b01, bad));
  bad.bots = 3;
  CHECK(room.requestSettings(0b01, bad));
  CHECK(room.settings().bots == 3);

  CHECK(room.requestStart(0b01) == Room::StartResult::Start);
  CHECK_FALSE(room.requestSettings(0b01, teams));  // Playing: settings are fixed
}

TEST_CASE("Room: Start, match over, play again", "[server][room]") {
  const sim::ContentDB db = sim::defaultContent();
  Room room(db, sim::kMaxPlayers, RoomSettings{.bots = 2});
  room.updateMembers(0b11);
  CHECK(room.requestStart(0b10) == Room::StartResult::Ignored);  // not the host
  CHECK(room.phase() == RoomPhase::Waiting);
  CHECK(room.requestStart(0b01) == Room::StartResult::Start);
  CHECK(room.playing());
  CHECK(room.requestStart(0b01) == Room::StartResult::Ignored);  // already playing
  CHECK(room.matchEnded(PlayerId{1}));
  CHECK(room.phase() == RoomPhase::Over);
  CHECK(room.winner() == PlayerId{1});
  CHECK_FALSE(room.matchEnded(PlayerId{0}));  // not playing
  CHECK(room.requestStart(0b10) == Room::StartResult::Ignored);
  CHECK(room.requestStart(0b01) == Room::StartResult::BackToWaiting);
  CHECK(room.phase() == RoomPhase::Waiting);
  CHECK(!room.winner().isValid());
  CHECK(room.settings() == RoomSettings{.bots = 2});  // the same settings
}

TEST_CASE("Room: the last human leaving returns it to Waiting; reset takes new settings",
          "[server][room]") {
  const sim::ContentDB db = sim::defaultContent();
  Room room(db, sim::kMaxPlayers, RoomSettings{.powerUps = 1, .bots = 1});
  room.updateMembers(0b1);
  CHECK(room.requestStart(0b1) == Room::StartResult::Start);
  CHECK(room.updateMembers(0));
  CHECK(room.phase() == RoomPhase::Waiting);
  CHECK(room.settings().bots == 1);

  room.updateMembers(0b1);
  CHECK(room.requestStart(0b1) == Room::StartResult::Start);
  CHECK(room.matchEnded(PlayerId::invalid()));  // a draw
  CHECK(!room.winner().isValid());
  room.reset(RoomSettings{});
  CHECK(room.phase() == RoomPhase::Waiting);
  CHECK(room.settings() == RoomSettings{});
  CHECK(room.host() == PlayerId{0});  // the roster stays
}

TEST_CASE("validRoomSettings and roomSettingsOf", "[server][room]") {
  const sim::ContentDB db = sim::defaultContent();
  CHECK(server::validRoomSettings(db, RoomSettings{}, sim::kMaxPlayers));
  CHECK(server::validRoomSettings(db, RoomSettings{.bots = 9}, sim::kMaxPlayers));
  CHECK_FALSE(server::validRoomSettings(db, RoomSettings{.bots = 10}, sim::kMaxPlayers));
  CHECK_FALSE(server::validRoomSettings(db, RoomSettings{.bots = 1}, 1));
  const sim::MatchConfig c{.seed = 9, .arena = 0, .mode = 0, .powerUps = 0x0F, .loadout = 0};
  CHECK(server::roomSettingsOf(c, 3) ==
        RoomSettings{.arena = 0, .mode = 0, .loadout = 0, .powerUps = 0x0F, .bots = 3});
}
