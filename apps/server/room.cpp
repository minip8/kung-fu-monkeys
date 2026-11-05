#include "room.hpp"

#include "kfm/net/messages.hpp"
#include "kfm/sim/content.hpp"

#include <cstdint>

namespace kfm::server {

bool validRoomSettings(const sim::ContentDB& content, const net::RoomSettings& s,
                       uint8_t maxPlayers) {
  return s.arena < content.arenaCount && s.mode < content.modeCount &&
         s.loadout < content.loadoutCount && content.compatible(s.arena, s.mode) &&
         s.bots < maxPlayers;
}

net::RoomSettings roomSettingsOf(const sim::MatchConfig& config, uint8_t bots) {
  return net::RoomSettings{.arena = config.arena,
                           .mode = config.mode,
                           .loadout = config.loadout,
                           .powerUps = config.powerUps,
                           .bots = bots};
}

Room::Room(const sim::ContentDB& content, uint8_t maxPlayers, const net::RoomSettings& settings)
    : content_(&content), maxPlayers_(maxPlayers), settings_(settings) {}

bool Room::isHost(sim::PlayerMask slots) const {
  const PlayerId h = host();
  return h.isValid() && sim::maskHas(slots, h);
}

bool Room::updateMembers(sim::PlayerMask humans) {
  bool changed = false;
  uint8_t kept = 0;
  for (uint8_t i = 0; i < count_; ++i) {
    if (sim::maskHas(humans, order_[i])) {
      order_[kept++] = order_[i];
    } else {
      changed = true;
    }
  }
  count_ = kept;
  for (uint8_t s = 0; s < sim::kMaxPlayers; ++s) {
    const PlayerId slot{s};
    if (!sim::maskHas(humans, slot)) continue;
    bool known = false;
    for (uint8_t i = 0; i < count_ && !known; ++i)
      known = order_[i] == slot;
    if (known) continue;
    order_[count_++] = slot;
    changed = true;
  }
  if (count_ == 0 && phase_ != net::RoomPhase::Waiting) {
    phase_ = net::RoomPhase::Waiting;
    winner_ = PlayerId::invalid();
    changed = true;
  }
  return changed;
}

bool Room::requestSettings(sim::PlayerMask peerSlots, const net::RoomSettings& settings) {
  if (phase_ != net::RoomPhase::Waiting || !isHost(peerSlots) ||
      !validRoomSettings(*content_, settings, maxPlayers_)) {
    return false;
  }
  settings_ = settings;
  return true;
}

Room::StartResult Room::requestStart(sim::PlayerMask peerSlots) {
  if (!isHost(peerSlots)) return StartResult::Ignored;
  switch (phase_) {
    case net::RoomPhase::Waiting:
      phase_ = net::RoomPhase::Playing;
      winner_ = PlayerId::invalid();
      return StartResult::Start;
    case net::RoomPhase::Over:
      phase_ = net::RoomPhase::Waiting;
      winner_ = PlayerId::invalid();
      return StartResult::BackToWaiting;
    case net::RoomPhase::Playing:
      break;
  }
  return StartResult::Ignored;
}

bool Room::matchEnded(PlayerId winner) {
  if (phase_ != net::RoomPhase::Playing) return false;
  phase_ = net::RoomPhase::Over;
  winner_ = winner;
  return true;
}

void Room::reset(const net::RoomSettings& settings) {
  phase_ = net::RoomPhase::Waiting;
  winner_ = PlayerId::invalid();
  settings_ = settings;
}

}  // namespace kfm::server
