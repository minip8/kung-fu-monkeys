// RulesSystem (architecture §5.3 steps 1 and 10): the phase machine every
// mode shares, with the mode-specific decisions in the ModeBehavior table
// (modes/registry.hpp) selected by content.mode.kind.
//
// Lobby -> (first tick) MatchStarted, modeData cleared, round 1 ->
// RoundStarting (players spawned, the mode's onRoundStart, countdown of
// roundStartTicks) -> Playing -> (the mode ends the round)
// RoundEnded (roundEndTicks) -> next round ... -> MatchEnded (the mode ends
// the match). A phase lasts exactly its tick count: rulesPreTick transitions
// when ticksInPhase reaches it, and a zero-length countdown goes straight to
// Playing in the same tick.
#include "kfm/sim/modes/registry.hpp"
#include "kfm/sim/systems.hpp"
#include "modes/mode_flow.hpp"

#include <cstdint>

namespace kfm::sim {
namespace {

void startRound(WorldState& s, const MatchContent& content, EventSink& sink) {
  s.boomerangs.clear();
  s.pickups.clear();
  // A player who left the roster mid-round (§6.6) is removed here; the
  // pool slot is reused if the slot rejoins later.
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    if (!maskHas(s.rules.present, PlayerId{i}) && s.players.isAliveIndex(i)) {
      s.players.despawn(s.players.idAt(i));
    }
  }
  // Everyone starts the round fresh: no respawn pending, no spawn protection.
  s.rules.respawnTicks = {};
  s.rules.invulnTicks = {};

  const ArenaDef& arena = content.arena;
  const uint32_t points = arena.spawnPointCount;
  const uint32_t offset = points > 0 ? s.rng.below(points) : 0;
  uint32_t rank = 0;
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const PlayerId id{i};
    if (!maskHas(s.rules.present, id)) continue;
    const EntityId e = s.players.isAliveIndex(i) ? s.players.idAt(i) : s.players.spawnAt(i);
    const Vec2 pos = points > 0 ? arena.spawnPoints[(rank + offset) % points] : Vec2::zero();
    ++rank;
    resetPlayer(s.players.items[e.index], id, pos, content);
    sink.push(SimEvent{EventKind::PlayerSpawned, s.tick, id, e, 0, 0});
  }
  sink.push(
      SimEvent{EventKind::RoundStarted, s.tick, PlayerId::invalid(), EntityId{}, s.rules.round, 0});
  s.rules.phase = MatchPhase::RoundStarting;
  s.rules.ticksInPhase = 0;
  // v0.31: the mode's own round start, after the shared spawn and before
  // the countdown runs.
  modeBehaviorOf(content).onRoundStart(s, content, sink);
}

}  // namespace

void rulesPreTick(WorldState& s, const MatchContent& content, EventSink& sink) {
  MatchRules& r = s.rules;
  if (r.phase == MatchPhase::Lobby) {
    sink.push(SimEvent{EventKind::MatchStarted, s.tick, PlayerId::invalid(), EntityId{}, 0, 0});
    r.modeData = {};  // v0.31: the mode's scratch starts every match empty
    r.round = 1;
    startRound(s, content, sink);
  } else if (r.phase == MatchPhase::RoundEnded && r.ticksInPhase >= content.mode.roundEndTicks) {
    // Saturates: an endless run of draws must not wrap the counter to 0.
    if (r.round < UINT8_MAX) ++r.round;
    startRound(s, content, sink);
  }
  if (r.phase == MatchPhase::RoundStarting && r.ticksInPhase >= content.mode.roundStartTicks) {
    r.phase = MatchPhase::Playing;
    r.ticksInPhase = 0;
  }
}

void rulesPostTick(WorldState& s, const MatchContent& content, EventSink& sink) {
  ++s.rules.ticksInPhase;
  if (s.rules.phase != MatchPhase::Playing) return;
  modeBehaviorOf(content).onPlaying(s, content, sink);
}

}  // namespace kfm::sim
