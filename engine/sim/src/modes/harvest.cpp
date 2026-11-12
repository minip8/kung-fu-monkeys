// Harvest (architecture §5.3, §11 "Contest design: bananas as the economy",
// v0.81): bananas are the resource as well as the ammunition. One round on a
// clock with deathmatch's respawns and roster (a leaver keeps its score: it
// is the team's). Team t's home is zone t (homeZoneOf, content/arena.hpp).
//
// Every Playing tick, after the clock advanced (ticksInPhase >= 1):
//   - banking, when ticksInPhase is a multiple of bankTicks: each living
//     monkey (zoneOccupants: not Eliminated, not Falling) whose centre is in
//     its own team's home zone and holds a banana banks one, in slot order:
//     its ammo drops by one and its own score gains one (ScoreChanged,
//     ScoreReason::Objective). An enemy standing in the zone stops nothing;
//   - stealing, when stealTicks > 0 and ticksInPhase is a multiple of it
//     (after the banking of the same tick): each living monkey with room in
//     its hand (ammo < ammoMax), in slot order, standing in the home zone of
//     another team whose score (the sum of its slots', present or not) is
//     above zero and with none of its living monkeys in that zone, takes one
//     banana back out: the point comes off the team's highest-scoring slot
//     (the lowest slot on a tie; ScoreChanged with ScoreReason::Stolen, -1),
//     the raider's ammo gains one, then BananaStolen. A raider takes at most
//     one a beat, from the lowest-numbered home it qualifies in.
// A banked banana leaves the world (the third exception to "bananas never
// disappear"). The first team to scoreToWin wins, else the leading team when
// timeLimitTicks runs out (a tied lead is a draw). Eliminations score
// nothing; the respawn hand is ModeDef::respawnAmmo (mode_flow.cpp).
//
// modeData: word 0 holds bankTicks (low half) and stealTicks (high half),
// written at the round start so a bot can read the beats; nothing else.
#include "kfm/sim/modes/modes.hpp"
#include "kfm/sim/zones.hpp"
#include "modes/mode_flow.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace kfm::sim {
namespace {

[[nodiscard]] bool onBeat(uint32_t ticks, uint16_t beat) {
  return beat > 0 && ticks % beat == 0;
}

// The slot of `team` a theft takes its point from: the highest score, the
// lowest slot on a tie, among every slot of the team (a team's score is the
// sum over all of them).
[[nodiscard]] PlayerId richestSlotOf(const WorldState& s, const MatchContent& content,
                                     uint8_t team) {
  PlayerId best = PlayerId::invalid();
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    if (content.teams[i] != team) continue;
    if (!best.isValid() || s.rules.scores[i] > s.rules.scores[best.value]) best = PlayerId{i};
  }
  return best;
}

void bank(WorldState& s, const MatchContent& content, EventSink& sink) {
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const PlayerId id{i};
    Player* p = s.playerBySlot(id);
    if (p == nullptr || p->ammo == 0) continue;
    const uint8_t home = homeZoneOf(content.arena, content.teamOf(id));
    if (home == kNoZone || !maskHas(zoneOccupants(s, content.arena, home), id)) continue;
    --p->ammo;
    addScore(s, id, 1, ScoreReason::Objective, sink);
  }
}

void steal(WorldState& s, const MatchContent& content, EventSink& sink) {
  const ArenaDef& arena = content.arena;
  const uint8_t zones = std::min<uint8_t>(arena.zoneCount, kMaxArenaZones);
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const PlayerId raider{i};
    Player* p = s.playerBySlot(raider);
    if (p == nullptr || p->ammo >= p->stats.ammoMax) continue;
    const uint8_t own = content.teamOf(raider);
    for (uint8_t k = 0; k < zones; ++k) {
      // Zone k is team k's home; a team index above the mode's teams has
      // no slot, so its score is 0 and it is never robbed.
      if (k == own) continue;
      const PlayerMask inside = zoneOccupants(s, arena, k);
      if (!maskHas(inside, raider) || content.teamScore(s.rules.scores, k) <= 0) continue;
      bool defended = false;
      for (uint8_t j = 0; j < kMaxPlayers; ++j) {
        if (maskHas(inside, PlayerId{j}) && content.teamOf(PlayerId{j}) == k) defended = true;
      }
      if (defended) continue;
      const PlayerId robbed = richestSlotOf(s, content, k);
      if (!robbed.isValid()) continue;
      addScore(s, robbed, -1, ScoreReason::Stolen, sink);
      ++p->ammo;
      sink.push(SimEvent{EventKind::BananaStolen, s.tick, raider, s.players.idAt(raider.value),
                         robbed.value, k});
      break;  // one a beat
    }
  }
}

}  // namespace

void harvestOnRoundStart(WorldState& s, const MatchContent& content, EventSink& /*sink*/) {
  s.rules.modeData = {};
  s.rules.modeData[0] =
      uint32_t{content.mode.bankTicks} | (uint32_t{content.mode.stealTicks} << 16);
}

void harvestOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink) {
  const ModeDef& mode = content.mode;
  const MatchRules& r = s.rules;
  deathmatchRoster(s, content, /*forfeitScore=*/false);
  clockWarnings(s, mode.timeLimitTicks, sink);

  if (onBeat(r.ticksInPhase, mode.bankTicks)) bank(s, content, sink);
  if (onBeat(r.ticksInPhase, mode.stealTicks)) steal(s, content, sink);

  int16_t best = 0;
  const PlayerId leader = teamScoreLeader(s, content, best);
  if (std::cmp_greater_equal(best, mode.scoreToWin) || clockExpired(r, mode.timeLimitTicks))
    endMatch(s, leader, sink);
}

void harvestOnEliminated(WorldState& s, const MatchContent& content, PlayerId victim,
                         PlayerId /*killer*/, EventSink& /*sink*/) {
  countDeath(s, content, victim);
}

}  // namespace kfm::sim
