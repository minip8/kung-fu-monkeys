// Hill (architecture §5.3, v0.33): king of the hill between teams on zone 0
// of the arena. One round on a clock with deathmatch's respawns and roster
// (a leaver keeps its score: it is the team's). Every Playing tick, the
// teams of the monkeys whose centre is in the zone (zoneOccupants) decide:
// exactly one team there advances that team's hold clock, and at hillTicks
// the team scores a point (ScoreChanged to its lowest slot in the zone, the
// team_rounds convention) and only that clock restarts; a contested or
// empty zone holds every clock where it is (it never resets). The first
// team to scoreToWin wins, else the leading team when timeLimitTicks runs
// out (a tied lead is a draw). Eliminations score nothing.
//
// modeData: words 0..4 hold the clocks of teams 0..9 (u16, team 2w in the
// low half of word w, 2w+1 in the high half); word 5 is the team holding
// the zone alone + 1 (0 = nobody), for HillHolderChanged. The round start
// clears them (the kind plays one round, so that is the match start).
#include "kfm/sim/modes/modes.hpp"
#include "kfm/sim/zones.hpp"
#include "modes/mode_flow.hpp"

#include <cstdint>
#include <utility>

namespace kfm::sim {
namespace {

constexpr uint8_t kHolderWord = 5;
constexpr uint32_t kNoHolder = 0xFF;
static_assert((kMaxPlayers + 1) / 2 <= kHolderWord && kHolderWord < kModeDataWords);

[[nodiscard]] uint32_t clockOf(const MatchRules& r, uint8_t team) {
  return (r.modeData[team / 2] >> ((team % 2) * 16)) & 0xFFFFu;
}

void setClock(MatchRules& r, uint8_t team, uint32_t ticks) {
  const uint32_t shift = (team % 2) * 16;
  uint32_t& word = r.modeData[team / 2];
  word = (word & ~(0xFFFFu << shift)) | ((ticks & 0xFFFFu) << shift);
}

// The one team among the zone's occupants, or kNoHolder (none or several);
// `lowest` receives that team's lowest slot in the zone.
[[nodiscard]] uint32_t soleTeam(const MatchContent& content, PlayerMask occupants,
                                PlayerId& lowest) {
  uint32_t team = kNoHolder;
  lowest = PlayerId::invalid();
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const PlayerId id{i};
    if (!maskHas(occupants, id)) continue;
    const uint8_t t = content.teamOf(id);
    if (team == kNoHolder) {
      team = t;
      lowest = id;
    } else if (team != t) {
      lowest = PlayerId::invalid();
      return kNoHolder;
    }
  }
  return team;
}

}  // namespace

void hillOnRoundStart(WorldState& s, const MatchContent& /*content*/, EventSink& /*sink*/) {
  s.rules.modeData = {};
}

void hillOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink) {
  const ModeDef& mode = content.mode;
  MatchRules& r = s.rules;
  deathmatchRoster(s, content, /*forfeitScore=*/false);
  clockWarnings(s, mode.timeLimitTicks, sink);

  PlayerId credit = PlayerId::invalid();
  const uint32_t holder = soleTeam(content, zoneOccupants(s, content.arena, 0), credit);
  const uint32_t stored = holder == kNoHolder ? 0 : holder + 1;
  if (stored != r.modeData[kHolderWord]) {
    r.modeData[kHolderWord] = stored;
    sink.push(
        SimEvent{EventKind::HillHolderChanged, s.tick, PlayerId::invalid(), EntityId{}, holder, 0});
  }
  if (holder != kNoHolder && credit.isValid()) {
    const auto team = static_cast<uint8_t>(holder);
    const uint32_t held = clockOf(r, team) + 1;
    if (held >= (mode.hillTicks > 0 ? mode.hillTicks : 1u)) {
      setClock(r, team, 0);
      addScore(s, credit, 1, ScoreReason::Objective, sink);
    } else {
      setClock(r, team, held);
    }
  }

  int16_t best = 0;
  const PlayerId leader = teamScoreLeader(s, content, best);
  if (std::cmp_greater_equal(best, mode.scoreToWin) || clockExpired(r, mode.timeLimitTicks))
    endMatch(s, leader, sink);
}

void hillOnEliminated(WorldState& s, const MatchContent& content, PlayerId victim,
                      PlayerId /*killer*/, EventSink& /*sink*/) {
  countDeath(s, content, victim);
}

}  // namespace kfm::sim
