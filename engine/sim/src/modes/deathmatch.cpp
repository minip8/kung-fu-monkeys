// Deathmatch mode (architecture §5.3, v0.23): one round on a clock. Every
// elimination scores a point for the attacker (ScoreChanged) and a death for
// the victim, whose respawn LifecycleSystem runs respawnTicks later. The
// first to scoreToWin wins; when timeLimitTicks of play have passed the
// leader wins, and a tie for the lead is a draw.
//
// Roster (§5.4): a slot present without a monkey and without a pending
// respawn (it joined mid-match) is scheduled like a respawn; a slot that left
// keeps its monkey until it is eliminated, then forfeits its score and stays
// out.
#include "kfm/sim/modes/modes.hpp"
#include "modes/mode_flow.hpp"

#include <algorithm>
#include <cstdint>

namespace kfm::sim {
namespace {

[[nodiscard]] uint16_t respawnDelay(const ModeDef& mode) {
  return std::max<uint16_t>(mode.respawnTicks, 1);  // validation keeps it >= 1
}

void updateRoster(WorldState& s, const MatchContent& content) {
  MatchRules& r = s.rules;
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    if (s.players.isAliveIndex(i)) continue;
    if (maskHas(r.present, PlayerId{i})) {
      if (r.respawnTicks[i] == 0) r.respawnTicks[i] = respawnDelay(content.mode);
    } else {
      r.scores[i] = 0;
      r.deaths[i] = 0;
      r.respawnTicks[i] = 0;
      r.invulnTicks[i] = 0;
    }
  }
}

}  // namespace

void deathmatchOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink) {
  const ModeDef& mode = content.mode;
  updateRoster(s, content);
  clockWarnings(s, mode.timeLimitTicks, sink);
  uint8_t best = 0;
  const PlayerId leader = scoreLeader(s.rules, best);
  if (best >= mode.scoreToWin || clockExpired(s.rules, mode.timeLimitTicks)) {
    endMatch(s, leader, sink);
  }
}

void deathmatchOnEliminated(WorldState& s, const MatchContent& content, PlayerId victim,
                            PlayerId killer, EventSink& sink) {
  MatchRules& r = s.rules;
  if (victim.isValid() && victim.value < kMaxPlayers) {
    if (r.deaths[victim.value] < UINT8_MAX) ++r.deaths[victim.value];
    r.respawnTicks[victim.value] = respawnDelay(content.mode);
  }
  if (!killer.isValid() || killer.value >= kMaxPlayers || killer == victim) return;
  uint8_t& score = r.scores[killer.value];
  if (score < UINT8_MAX) ++score;
  sink.push(SimEvent{EventKind::ScoreChanged, s.tick, killer, EntityId{}, score, 0});
}

}  // namespace kfm::sim
