// Survival mode (architecture §5.3, v0.23): rounds on a clock. A round ends
// like a rounds-mode round (at most one monkey left with two or more slots
// present); when timeLimitTicks of play pass first, the survivor with the
// most hp wins it, then the one with the fewest deaths this match, and a tie
// on both is a draw. Round wins count toward roundsToWin as in rounds mode;
// nobody respawns within a round.
#include "kfm/sim/modes/modes.hpp"
#include "modes/mode_flow.hpp"

#include <bit>
#include <cstdint>

namespace kfm::sim {
namespace {

// The survivor ranked first by (hp desc, deaths asc), or invalid on a tie.
[[nodiscard]] PlayerId timeoutWinner(const WorldState& s) {
  PlayerId best = PlayerId::invalid();
  uint8_t bestHp = 0;
  uint8_t bestDeaths = 0;
  bool tied = false;
  s.players.forEachAlive([&](EntityId /*id*/, const Player& p) {
    if (p.tags.has(Tag::Eliminated) || p.id.value >= kMaxPlayers) return;
    const uint8_t deaths = s.rules.deaths[p.id.value];
    if (!best.isValid() || p.hp > bestHp || (p.hp == bestHp && deaths < bestDeaths)) {
      best = p.id;
      bestHp = p.hp;
      bestDeaths = deaths;
      tied = false;
    } else if (p.hp == bestHp && deaths == bestDeaths) {
      tied = true;
    }
  });
  return tied ? PlayerId::invalid() : best;
}

}  // namespace

void survivalOnPlaying(WorldState& s, const MatchContent& content, EventSink& sink) {
  const uint32_t limit = content.mode.timeLimitTicks;
  clockWarnings(s, limit, sink);
  if (std::popcount(static_cast<uint32_t>(s.rules.present)) >= 2 && s.players.size() <= 1) {
    endRound(s, content, soleSurvivor(s), sink);
  } else if (clockExpired(s.rules, limit)) {
    endRound(s, content, timeoutWinner(s), sink);
  }
}

void survivalOnEliminated(WorldState& s, const MatchContent& /*content*/, PlayerId victim,
                          PlayerId /*killer*/, EventSink& /*sink*/) {
  if (!victim.isValid() || victim.value >= kMaxPlayers) return;
  uint8_t& deaths = s.rules.deaths[victim.value];
  if (deaths < UINT8_MAX) ++deaths;
}

}  // namespace kfm::sim
