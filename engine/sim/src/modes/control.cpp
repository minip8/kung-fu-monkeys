// Control (architecture §5.3, v0.79): capture the arena's zones and keep
// them. One round on a clock with deathmatch's respawns and roster (a leaver
// keeps its score: it is the team's), over every zone of the arena.
//
// A zone has an owner (a team, or nobody at the start). Every Playing tick,
// per zone, the monkeys whose centre is in it (zoneOccupants) decide:
//   - the team with strictly the most monkeys there, when it is not the
//     owner, is the zone's challenger and gains a tick of capture progress
//     (a new challenger starts from zero); at captureTicks it owns the zone
//     (ZoneCaptured) and the progress clears;
//   - the owner with strictly the most, or an empty zone, clears the
//     progress and the challenger;
//   - a tie for the most holds the progress where it is.
// So a zone is taken by out-numbering whoever stands on it, and kept without
// standing on it. An owned zone then pays its team one tick of the team's
// clock per Playing tick, occupied or not; every hillTicks of clock score a
// point (ScoreChanged to the team's lowest present slot, the team_rounds
// convention) and the remainder carries over. The first team to scoreToWin
// wins, else the leading team when timeLimitTicks runs out (a tied lead is a
// draw). Eliminations score nothing.
//
// modeData (up to kMaxControlTeams teams, kMaxArenaZones zones):
//   words 0-1  the clocks of teams 0..3 (u16, team 2w in the low half of
//              word w, 2w+1 in the high half: hill's layout)
//   word 2     the owners: 4 bits per zone, zone k in bits 4k..4k+3, the
//              team + 1 (0 = nobody)
//   word 3     the challengers, likewise (0 = no capture under way)
//   words 4-7  the capture progress: u16 per zone, zone k in word 4 + k/2,
//              the low half for an even k
// The round start clears them (the kind plays one round).
#include "kfm/sim/modes/modes.hpp"
#include "kfm/sim/zones.hpp"
#include "modes/mode_flow.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace kfm::sim {
namespace {

constexpr uint8_t kOwnerWord = 2;
constexpr uint8_t kChallengerWord = 3;
constexpr uint8_t kProgressWord = 4;
constexpr uint8_t kNobody = 0xFF;
static_assert((kMaxControlTeams + 1) / 2 <= kOwnerWord, "a clock per team before the owners");
static_assert(kMaxArenaZones * 4 <= 32, "a nibble per zone in the owner and challenger words");
static_assert(kMaxControlTeams < 15, "a team + 1 fits a nibble");
static_assert(kProgressWord + ((kMaxArenaZones + 1) / 2) <= kModeDataWords,
              "a u16 of progress per zone");

[[nodiscard]] uint32_t clockOf(const MatchRules& r, uint8_t team) {
  return (r.modeData[team / 2] >> ((team % 2) * 16)) & 0xFFFFu;
}

void setClock(MatchRules& r, uint8_t team, uint32_t ticks) {
  const uint32_t shift = (team % 2) * 16;
  uint32_t& word = r.modeData[team / 2];
  word = (word & ~(0xFFFFu << shift)) | ((ticks & 0xFFFFu) << shift);
}

// The team in zone k's nibble of `word`, or kNobody.
[[nodiscard]] uint8_t teamIn(const MatchRules& r, uint8_t word, uint8_t k) {
  const uint32_t v = (r.modeData[word] >> (4u * k)) & 0xFu;
  return v == 0 ? kNobody : static_cast<uint8_t>(v - 1);
}

void setTeamIn(MatchRules& r, uint8_t word, uint8_t k, uint8_t team) {
  const uint32_t shift = 4u * k;
  const uint32_t v = team == kNobody ? 0u : uint32_t{team} + 1u;
  r.modeData[word] = (r.modeData[word] & ~(0xFu << shift)) | (v << shift);
}

[[nodiscard]] uint32_t progressOf(const MatchRules& r, uint8_t k) {
  return (r.modeData[kProgressWord + (k / 2)] >> ((k % 2) * 16)) & 0xFFFFu;
}

void setProgress(MatchRules& r, uint8_t k, uint32_t ticks) {
  const uint32_t shift = (k % 2) * 16;
  uint32_t& word = r.modeData[kProgressWord + (k / 2)];
  word = (word & ~(0xFFFFu << shift)) | ((ticks & 0xFFFFu) << shift);
}

// What the monkeys in a zone decide this tick.
struct Majority {
  uint8_t team{kNobody};  // the team with strictly the most, or kNobody
  bool empty{true};       // nobody of a control team is in the zone
};

[[nodiscard]] Majority majorityOf(const MatchContent& content, PlayerMask occupants) {
  std::array<uint8_t, kMaxControlTeams> count{};
  Majority m;
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const PlayerId id{i};
    if (!maskHas(occupants, id)) continue;
    const uint8_t t = content.teamOf(id);
    if (t >= kMaxControlTeams) continue;  // the loader keeps a control mode within them
    ++count[t];
    m.empty = false;
  }
  uint8_t most = 0;
  bool tie = false;
  for (uint8_t t = 0; t < kMaxControlTeams; ++t) {
    if (count[t] > most) {
      most = count[t];
      m.team = t;
      tie = false;
    } else if (count[t] == most && most > 0) {
      tie = true;
    }
  }
  if (tie) m.team = kNobody;
  return m;
}

// The lowest present slot of `team`, or invalid: where its points go.
[[nodiscard]] PlayerId lowestPresentOf(const WorldState& s, const MatchContent& content,
                                       uint8_t team) {
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const PlayerId id{i};
    if (maskHas(s.rules.present, id) && content.teamOf(id) == team) return id;
  }
  return PlayerId::invalid();
}

}  // namespace

void controlOnRoundStart(WorldState& s, const MatchContent& /*content*/, EventSink& /*sink*/) {
  s.rules.modeData = {};
}

void controlOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink) {
  const ModeDef& mode = content.mode;
  MatchRules& r = s.rules;
  deathmatchRoster(s, content, /*forfeitScore=*/false);
  clockWarnings(s, mode.timeLimitTicks, sink);

  const uint8_t zones = std::min<uint8_t>(content.arena.zoneCount, kMaxArenaZones);
  const uint32_t captureTicks = mode.captureTicks > 0 ? mode.captureTicks : 1u;
  std::array<uint8_t, kMaxControlTeams> owned{};
  for (uint8_t k = 0; k < zones; ++k) {
    const Majority m = majorityOf(content, zoneOccupants(s, content.arena, k));
    uint8_t owner = teamIn(r, kOwnerWord, k);
    if (m.team != kNobody && m.team != owner) {
      uint32_t progress = teamIn(r, kChallengerWord, k) == m.team ? progressOf(r, k) : 0u;
      ++progress;
      if (progress >= captureTicks) {
        owner = m.team;
        setTeamIn(r, kOwnerWord, k, owner);
        setTeamIn(r, kChallengerWord, k, kNobody);
        setProgress(r, k, 0);
        sink.push(
            SimEvent{EventKind::ZoneCaptured, s.tick, PlayerId::invalid(), EntityId{}, owner, k});
      } else {
        setTeamIn(r, kChallengerWord, k, m.team);
        setProgress(r, k, progress);
      }
    } else if (m.team != kNobody || m.empty) {
      // The owner out-numbers everyone, or nobody is left: the capture is
      // off. What remains is a tie for the most, which holds the progress.
      setTeamIn(r, kChallengerWord, k, kNobody);
      setProgress(r, k, 0);
    }
    if (owner != kNobody && owner < kMaxControlTeams) ++owned[owner];
  }

  const uint32_t pointTicks = mode.hillTicks > 0 ? mode.hillTicks : 1u;
  for (uint8_t t = 0; t < kMaxControlTeams; ++t) {
    if (owned[t] == 0) continue;
    uint32_t clock = clockOf(r, t) + owned[t];
    const PlayerId credit = lowestPresentOf(s, content, t);
    while (clock >= pointTicks) {
      clock -= pointTicks;
      if (credit.isValid()) addScore(s, credit, 1, ScoreReason::Objective, sink);
    }
    setClock(r, t, clock);
  }

  int16_t best = 0;
  const PlayerId leader = teamScoreLeader(s, content, best);
  if (std::cmp_greater_equal(best, mode.scoreToWin) || clockExpired(r, mode.timeLimitTicks))
    endMatch(s, leader, sink);
}

void controlOnEliminated(WorldState& s, const MatchContent& content, PlayerId victim,
                         PlayerId /*killer*/, EventSink& /*sink*/) {
  countDeath(s, content, victim);
}

}  // namespace kfm::sim
