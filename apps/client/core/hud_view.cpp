#include "hud_view.hpp"

#include "kfm/content/app_content.hpp"
#include "net_session.hpp"
#include "replay_session.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace kfm::client {

namespace {

constexpr std::array kLocalMatchOverActions{MatchOverAction::PlayAgain,
                                            MatchOverAction::BackToLobby};
constexpr std::array kLostConnectionActions{LostConnectionAction::Reconnect,
                                            LostConnectionAction::Leave};

}  // namespace

HudView hudView(const MatchSession& session) {
  HudView v;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    v.owners[i] = session.owner(PlayerId{i});
    if (v.owners[i] == SlotOwner::Bot) v.bots = static_cast<sim::PlayerMask>(v.bots | (1u << i));
  }
  v.dropped = static_cast<sim::PlayerMask>(session.droppedMask() & session.localMask());
  v.paused = session.paused();
  return v;
}

HudView hudView(const NetSession& session) {
  HudView v;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i)
    v.owners[i] = session.owner(PlayerId{i});
  v.dropped = session.droppedMask();
  v.network = true;
  v.statusLine = session.statusText();
  if (session.room())
    v.roomLine = "Room " + session.room()->code + " (" + session.room()->region + ")";
  // v0.42: the room's nicknames, v0.43 its bots by name too; a room shows
  // its own match over (no server-chosen next match).
  const std::optional<net::RoomStateMsg>& room = session.roomState();
  if (room) {
    v.bots = room->bots;
    for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
      const PlayerId slot{i};
      if (sim::maskHas(room->humans, slot) || sim::maskHas(room->bots, slot)) {
        v.names[i] = session.slotName(slot);
      }
    }
  }
  if (session.configKnown() && !room) {
    const sim::MatchConfig& next = session.client().matchConfig();
    const sim::ContentDB& db = session.content();
    v.nextMatch = std::string("Next match: ") + db.arenaAt(next.arena).name.cStr() + ", " +
                  db.modeAt(next.mode).name.cStr() + ", loadout " +
                  db.loadoutAt(next.loadout).name.cStr() +
                  ", power-ups: " + content::powerUpListText(db, next.powerUps);
  }
  return v;
}

HudView hudView(const ReplaySession& session) {
  HudView v;
  const sim::PlayerMask present = session.matchConfig().players;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    v.owners[i] = (present & (1u << i)) != 0 ? SlotOwner::Remote : SlotOwner::None;
  }
  v.replay = true;
  return v;
}

std::array<std::string, sim::kMaxPlayers> slotLabels(const HudView& view) {
  std::array<std::string, sim::kMaxPlayers> l;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i)
    l[i] = slotLabel(view.names[i], view.owners[i], i);
  return l;
}

sim::PlayerMask onlineLocalMask(const HudView& view) {
  if (!view.network) return 0;
  sim::PlayerMask m = 0;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if (view.owners[i] == SlotOwner::Local || view.owners[i] == SlotOwner::Dropped) {
      m = static_cast<sim::PlayerMask>(m | (1u << i));
    }
  }
  return m;
}

WinnerTitle winnerTitle(const HudView& view, const presentation::RenderHud& hud, uint8_t winner) {
  if (hud.teamCount > 0 && winner < sim::kMaxPlayers) {
    const uint8_t team = hud.teams[winner];
    return WinnerTitle{.text = "Team " + std::to_string(unsigned{team} + 1U), .colorIndex = team};
  }
  return WinnerTitle{.text = slotLabel(view.names[winner], view.owners[winner], winner),
                     .colorIndex = winner};
}

bool clockUrgent(const presentation::RenderHud& hud) {
  return hud.phase == presentation::HudPhase::Playing && hud.clockSeconds <= kClockUrgentSeconds;
}

ZoneStrip zoneStrip(const presentation::RenderState& state) {
  ZoneStrip strip;
  if (state.hud.mode != sim::ModeKind::Control) return strip;
  const auto team = [](int8_t t) -> std::optional<uint8_t> {
    if (t < 0 || t >= static_cast<int8_t>(sim::kMaxPlayers)) return std::nullopt;
    return static_cast<uint8_t>(t);
  };
  for (uint8_t k = 0; k < state.zoneCount && k < sim::kMaxArenaZones; ++k) {
    const presentation::RenderZone& z = state.zones[k];
    ZoneChip& chip = strip.chips[strip.count++];
    chip.letter = static_cast<char>('a' + k);
    chip.owner = team(z.team);
    chip.challenger = team(z.challenger);
    chip.capture = chip.challenger ? z.capture : 0.0f;
    if (chip.owner) ++strip.owned[*chip.owner];
  }
  return strip;
}

const char* matchOverLabel(MatchOverAction action) {
  switch (action) {
    case MatchOverAction::PlayAgain:
      return "Play again";
    case MatchOverAction::BackToLobby:
      return "Back to lobby";
    case MatchOverAction::Leave:
      return "Leave";
  }
  return "";
}

std::span<const MatchOverAction> localMatchOverActions() {
  return kLocalMatchOverActions;
}

std::optional<MatchOverAction> matchOverActionAt(std::span<const MatchOverAction> actions,
                                                 std::size_t index) {
  if (index >= actions.size()) return std::nullopt;
  return actions[index];
}

const char* lostConnectionLabel(LostConnectionAction action) {
  switch (action) {
    case LostConnectionAction::Reconnect:
      return "Reconnect";
    case LostConnectionAction::Leave:
      return "Leave";
  }
  return "";
}

std::span<const LostConnectionAction> lostConnectionActions(bool canReconnect) {
  if (!canReconnect) return {};
  return kLostConnectionActions;
}

std::optional<LostConnectionAction> lostConnectionActionAt(
    std::span<const LostConnectionAction> actions, std::size_t index) {
  if (index >= actions.size()) return std::nullopt;
  return actions[index];
}

MatchOverView matchOverView(const HudView& view, const presentation::RenderHud& hud) {
  MatchOverView v;
  v.board = buildScoreboard(hud, slotLabels(view), onlineLocalMask(view), view.bots);
  v.title = "Draw!";
  if (hud.matchWinner >= 0 && std::cmp_less(hud.matchWinner, sim::kMaxPlayers)) {
    WinnerTitle w = winnerTitle(view, hud, static_cast<uint8_t>(hud.matchWinner));
    v.title = std::move(w.text) + " wins the match!";
    v.colorIndex = w.colorIndex;
  }
  return v;
}

MatchOverView localMatchOverView(const HudView& view, const presentation::RenderHud& hud) {
  MatchOverView v = matchOverView(view, hud);
  const std::span<const MatchOverAction> actions = localMatchOverActions();
  v.actions.assign(actions.begin(), actions.end());
  return v;
}

MatchOverView roomMatchOverView(const HudView& view, const presentation::RenderHud& hud, bool host,
                                std::string_view hostName) {
  MatchOverView v = matchOverView(view, hud);
  if (host) {
    v.actions = {MatchOverAction::PlayAgain, MatchOverAction::Leave};
  } else {
    v.actions = {MatchOverAction::Leave};
    v.waiting = "Waiting for " +
                (hostName.empty() ? std::string("the host") : std::string(hostName)) +
                " to play again";
  }
  return v;
}

std::array<RoomSlot, sim::kMaxPlayers> roomSlots(const net::RoomStateMsg& room) {
  std::array<RoomSlot, sim::kMaxPlayers> out{};
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    const PlayerId slot{i};
    if (i >= room.maxPlayers) {
      out[i] = RoomSlot::Closed;  // the decoder refuses a holder up here
    } else if (sim::maskHas(room.humans, slot)) {
      out[i] = RoomSlot::Human;
    } else if (sim::maskHas(room.bots, slot)) {
      out[i] = RoomSlot::Bot;
    } else if (sim::maskHas(room.botPlacement, slot)) {
      out[i] = RoomSlot::BotAtStart;
    }
  }
  return out;
}

}  // namespace kfm::client
