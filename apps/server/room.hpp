// Room: the online room's state machine (M6b, architecture §6.6, §11 "M6b
// design"), kept beside ServerHost as the lobby adapter is, never in
// WorldState. Headless and policy only: ServerHost feeds it the human slots
// and the peers' requests and carries out what it decides (bots, the match
// restart, the RoomState broadcast).
//
//   Waiting  no match runs (the host does not tick the NetServer); peers
//            occupy slots, see the roster, the settings and the host; the
//            host's RoomSettings are validated and applied; the host's
//            Start → Playing.
//   Playing  the match runs as before M6b; MatchEnded → Over.
//   Over     the match-over screen (no ticks); the host's Start ("play
//            again") → Waiting with the same settings.
// Every state returns to Waiting when the last human slot is gone, keeping
// the settings; reset() returns to Waiting with given settings (a new lobby
// room).
//
// The host is a slot: the first human slot to appear, then, when its slots
// go, the longest-present human slot that remains; none while the room has
// no human. A slot reserved for a reconnecting peer stays a member, so a
// host that drops mid-match and reconnects keeps the role; outside Playing
// the host frees reservations at once (no grace: no tick expires them).
#pragma once

#include "kfm/core/types.hpp"
#include "kfm/net/messages.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/input.hpp"

#include <array>
#include <cstdint>

namespace kfm::server {

// Whether the room may play these settings on this content: arena, mode
// and loadout defined, ContentDB::compatible(arena, mode), and at most
// maxPlayers − 1 bots (one slot stays a human's). The power-up mask is any.
[[nodiscard]] bool validRoomSettings(const sim::ContentDB& content, const net::RoomSettings& s,
                                     uint8_t maxPlayers);

// The room's settings of a match config and a bot count (the server's
// defaults: --arena/--mode/--loadout/--powerups and --bots).
[[nodiscard]] net::RoomSettings roomSettingsOf(const sim::MatchConfig& config, uint8_t bots);

// The slots `count` room bots take at Start: the lowest `count` slots of
// `free` (NetServer::freeSlots(): below the player cap, nobody's), all of
// them when fewer are free. ServerHost places the bots with it and sends
// it as the Waiting RoomState's botPlacement, so the waiting room's
// preview and the placement cannot disagree (issue #68).
[[nodiscard]] sim::PlayerMask roomBotSlots(sim::PlayerMask free, uint8_t count);

class Room {
 public:
  enum class StartResult : uint8_t {
    Ignored = 0,    // not the host, or nothing to start in this state
    Start,          // Waiting → Playing: place the bots and restart the match
    BackToWaiting,  // Over → Waiting: remove the bots
  };

  Room(const sim::ContentDB& content, uint8_t maxPlayers, const net::RoomSettings& settings);

  [[nodiscard]] net::RoomPhase phase() const { return phase_; }
  [[nodiscard]] bool playing() const { return phase_ == net::RoomPhase::Playing; }
  [[nodiscard]] const net::RoomSettings& settings() const { return settings_; }
  // The host slot, invalid when the room has no human.
  [[nodiscard]] PlayerId host() const { return count_ > 0 ? order_[0] : PlayerId::invalid(); }
  // Whether a peer holding `slots` is the host.
  [[nodiscard]] bool isHost(sim::PlayerMask slots) const;
  // Over: the match's winner (invalid on a draw or in any other state).
  [[nodiscard]] PlayerId winner() const { return winner_; }
  // The human slots in the order they joined, the host first.
  [[nodiscard]] uint8_t memberCount() const { return count_; }
  [[nodiscard]] PlayerId member(uint8_t i) const {
    return i < count_ ? order_[i] : PlayerId::invalid();
  }

  // The human slots now (NetServer::humanSlots()): new slots join the order
  // (ascending among themselves), gone ones leave it. With none left the room
  // returns to Waiting. True when anything a RoomState shows changed.
  bool updateMembers(sim::PlayerMask humans);
  // A peer's RoomSettings: applied when it is the host, the room is Waiting
  // and the settings are valid. True when applied (the caller re-sends the
  // RoomState either way).
  bool requestSettings(sim::PlayerMask peerSlots, const net::RoomSettings& settings);
  // A peer's Start (see StartResult); the phase changes before it returns.
  StartResult requestStart(sim::PlayerMask peerSlots);
  // The match ended (Playing → Over). False in any other state.
  bool matchEnded(PlayerId winner);
  // Waiting with `settings` (a new group arrives): the roster stays.
  void reset(const net::RoomSettings& settings);

 private:
  const sim::ContentDB* content_;
  uint8_t maxPlayers_;
  net::RoomPhase phase_{net::RoomPhase::Waiting};
  net::RoomSettings settings_{};
  PlayerId winner_{PlayerId::invalid()};
  std::array<PlayerId, sim::kMaxPlayers> order_{};
  uint8_t count_{0};
};

}  // namespace kfm::server
