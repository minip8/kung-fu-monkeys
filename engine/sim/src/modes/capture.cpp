// Capture (architecture §5.3, v0.33): capture the flag between teams. One
// round on a clock with deathmatch's respawns and roster (a leaver keeps its
// score). Team t's flag (a PickupKind::Flag pickup whose `spawner` is t)
// starts at the centre of zone t. An enemy of the flag's team takes it
// through rule row 52 (rules/flag.cpp), which calls onPickupTaken: the taker
// gets Tag::Carrying and becomes the flag's carrier (a carrier takes no
// second flag). A carrier whose centre is in its own team's zone while its
// own flag is home scores (ScoreChanged to the carrier's slot) and the flag
// goes home. An eliminated carrier drops the flag as a pickup where it
// stood; a dropped flag nobody picks up for flagReturnTicks goes home (its
// own team cannot take it, so only the timer returns it). The first team to
// scoreToWin wins, else the leading team when timeLimitTicks runs out (a
// tied lead is a draw). Eliminations score nothing; bananas treat a carrier
// like anyone, and carrying changes no stat.
//
// modeData word t (t below the flag count, min(teams, zones, 8)): bits 0-1
// the flag's state (Home, Carried, Dropped), bits 8-15 the carrier's slot,
// bits 16-31 the ticks since it was dropped. The round start clears them and
// places the flags (the kind plays one round).
#include "kfm/sim/modes/modes.hpp"
#include "modes/mode_flow.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace kfm::sim {
namespace {

enum class FlagState : uint8_t { Home = 0, Carried = 1, Dropped = 2 };

struct Flag {
  FlagState state{FlagState::Home};
  uint8_t carrier{0};
  uint16_t droppedTicks{0};
};

// Flag t lives in modeData word t for every t below flagCount(), which is at
// most kMaxArenaZones: a word per possible zone must fit.
static_assert(kMaxArenaZones <= kModeDataWords, "a modeData word per capture flag");

[[nodiscard]] Flag readFlag(const MatchRules& r, uint8_t t) {
  const uint32_t w = r.modeData[t];
  return Flag{static_cast<FlagState>(w & 0x3u), static_cast<uint8_t>((w >> 8) & 0xFFu),
              static_cast<uint16_t>(w >> 16)};
}

void writeFlag(MatchRules& r, uint8_t t, const Flag& f) {
  r.modeData[t] = static_cast<uint32_t>(f.state) | (uint32_t{f.carrier} << 8) |
                  (uint32_t{f.droppedTicks} << 16);
}

// The number of flags: one per team, as far as the arena has zones.
[[nodiscard]] uint8_t flagCount(const MatchContent& content) {
  const uint8_t zones = std::min<uint8_t>(content.arena.zoneCount, kMaxArenaZones);
  return std::min<uint8_t>(content.mode.teams, zones);
}

// Team t's flag pickup lying in the arena, or an invalid id.
[[nodiscard]] EntityId flagPickup(const WorldState& s, uint8_t t) {
  EntityId found{};
  s.pickups.forEachAlive([&](EntityId id, const Pickup& k) {
    if (k.kind == PickupKind::Flag && k.spawner == t) found = id;
  });
  return found;
}

// Lays team t's flag at `pos` (PickupSpawned). False when the pool is full.
bool layFlag(WorldState& s, uint8_t t, Vec2 pos, EventSink& sink) {
  const EntityId id = s.pickups.spawn();
  if (!id.isValid()) return false;
  Pickup& k = s.pickups.items[id.index];
  k.kind = PickupKind::Flag;
  k.pos = pos;
  k.powerUpIndex = 0;
  k.spawner = t;
  sink.push(SimEvent{EventKind::PickupSpawned, s.tick, PlayerId::invalid(), id,
                     static_cast<uint32_t>(PickupKind::Flag), t});
  return true;
}

// Team t's flag back at the centre of zone t, wherever it was.
void sendHome(WorldState& s, const MatchContent& content, uint8_t t, EventSink& sink) {
  const EntityId lying = flagPickup(s, t);
  if (lying.isValid()) s.pickups.despawn(lying);
  writeFlag(s.rules, t, Flag{});
  layFlag(s, t, content.arena.zones[t].centre(), sink);
}

}  // namespace

void captureOnRoundStart(WorldState& s, const MatchContent& content, EventSink& sink) {
  s.rules.modeData = {};
  const uint8_t flags = flagCount(content);
  for (uint8_t t = 0; t < flags; ++t)
    layFlag(s, t, content.arena.zones[t].centre(), sink);
}

void captureOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink) {
  const ModeDef& mode = content.mode;
  MatchRules& r = s.rules;
  deathmatchRoster(s, content, /*forfeitScore=*/false);
  clockWarnings(s, mode.timeLimitTicks, sink);

  const uint8_t flags = flagCount(content);
  for (uint8_t t = 0; t < flags; ++t) {
    Flag f = readFlag(r, t);
    switch (f.state) {
      case FlagState::Carried: {
        Player* carrier = s.playerBySlot(PlayerId{f.carrier});
        if (carrier == nullptr || carrier->tags.has(Tag::Eliminated) ||
            !carrier->tags.has(Tag::Carrying)) {
          sendHome(s, content, t, sink);  // the carrier is gone without a drop
          break;
        }
        const uint8_t own = content.teamOf(carrier->id);
        if (own >= flags || readFlag(r, own).state != FlagState::Home) break;
        const Zone& base = content.arena.zones[own];
        if (!boxContains(base.min, base.max, carrier->transform.pos)) break;
        carrier->tags.clear(Tag::Carrying);
        addScore(s, carrier->id, 1, ScoreReason::Objective, sink);
        sendHome(s, content, t, sink);
        break;
      }
      case FlagState::Dropped:
        // Counted from the drop tick's own post-tick: a flag dropped on
        // tick T goes home on tick T + flagReturnTicks.
        if (f.droppedTicks >= std::max<uint16_t>(mode.flagReturnTicks, 1) ||
            !flagPickup(s, t).isValid()) {
          sendHome(s, content, t, sink);
        } else {
          ++f.droppedTicks;
          writeFlag(r, t, f);
        }
        break;
      case FlagState::Home:
        // A full pickup pool kept it from being laid: try again.
        if (!flagPickup(s, t).isValid()) layFlag(s, t, content.arena.zones[t].centre(), sink);
        break;
    }
  }

  int16_t best = 0;
  const PlayerId leader = teamScoreLeader(s, content, best);
  if (std::cmp_greater_equal(best, mode.scoreToWin) || clockExpired(r, mode.timeLimitTicks))
    endMatch(s, leader, sink);
}

void captureOnEliminated(WorldState& s, const MatchContent& content, PlayerId victim,
                         PlayerId /*killer*/, EventSink& sink) {
  countDeath(s, content, victim);
  Player* p = s.playerBySlot(victim);
  if (p == nullptr || !p->tags.has(Tag::Carrying)) return;
  p->tags.clear(Tag::Carrying);
  const uint8_t flags = flagCount(content);
  for (uint8_t t = 0; t < flags; ++t) {
    const Flag f = readFlag(s.rules, t);
    if (f.state != FlagState::Carried || f.carrier != victim.value) continue;
    if (layFlag(s, t, p->transform.pos, sink)) {
      writeFlag(s.rules, t, Flag{FlagState::Dropped, 0, 0});
    } else {
      sendHome(s, content, t, sink);
    }
  }
}

void captureOnPickupTaken(WorldState& s, const MatchContent& content, PlayerId player,
                          const Pickup& pickup, EventSink& /*sink*/) {
  if (pickup.kind != PickupKind::Flag || pickup.spawner >= flagCount(content)) return;
  Player* p = s.playerBySlot(player);
  if (p == nullptr) return;
  p->tags.set(Tag::Carrying);
  writeFlag(s.rules, pickup.spawner, Flag{FlagState::Carried, player.value, 0});
}

}  // namespace kfm::sim
