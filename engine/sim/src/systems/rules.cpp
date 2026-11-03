// RulesSystem (architecture §5.3 steps 1 and 10): the FFA round flow.
//
// Lobby -> (first tick) MatchStarted, round 1 -> RoundStarting (players spawned,
// countdown of roundStartTicks) -> Playing -> (at most one player left)
// RoundEnded (roundEndTicks) -> next round ... -> MatchEnded once a player
// reaches roundsToWin. A phase lasts exactly its tick count: rulesPreTick
// transitions when ticksInPhase reaches it, and a zero-length countdown goes
// straight to Playing in the same tick.
#include "kfm/sim/systems.hpp"

#include <bit>
#include <cstdint>

namespace kfm::sim {
namespace {

// Deterministic facing at spawn: toward the arena centre along x.
[[nodiscard]] Angle facingTowardCentre(Vec2 pos) {
  return pos.x > Fixed::zero() ? Angle::fromBam(Angle::kHalf) : Angle{};
}

void resetPlayer(Player& p, PlayerId id, Vec2 pos, const ContentDB& content) {
  // Keep the previous buttons so a button held across the round boundary does
  // not register as a fresh press.
  const uint8_t prevButtons = p.prevButtons;
  p = Player{};
  p.id = id;
  p.prevButtons = prevButtons;
  p.transform.pos = pos;
  p.transform.prevPos = pos;
  p.transform.facing = facingTowardCentre(pos);
  p.stats = Stats{content.player.moveSpeed, content.player.maxHp, content.player.ammoMax};
  p.hp = content.player.maxHp;
  p.ammo = content.player.ammoStart;
  for (uint8_t slot = 0; slot < kAbilitySlots; ++slot) {
    p.abilities[slot].defIndex = content.player.abilities[slot];
  }
}

void startRound(WorldState& s, const ContentDB& content, EventSink& sink) {
  s.boomerangs.clear();
  s.pickups.clear();

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
}

[[nodiscard]] PlayerId soleSurvivor(const WorldState& s) {
  PlayerId winner = PlayerId::invalid();
  if (s.players.size() != 1) return winner;
  s.players.forEachAlive([&](EntityId /*id*/, const Player& p) { winner = p.id; });
  return winner;
}

}  // namespace

void rulesPreTick(WorldState& s, const ContentDB& content, EventSink& sink) {
  MatchRules& r = s.rules;
  if (r.phase == MatchPhase::Lobby) {
    sink.push(SimEvent{EventKind::MatchStarted, s.tick, PlayerId::invalid(), EntityId{}, 0, 0});
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

void rulesPostTick(WorldState& s, const ContentDB& content, EventSink& sink) {
  MatchRules& r = s.rules;
  ++r.ticksInPhase;
  if (r.phase != MatchPhase::Playing) return;
  if (std::popcount(static_cast<uint32_t>(r.present)) < 2 || s.players.size() > 1) return;

  const PlayerId winner = soleSurvivor(s);
  r.lastRoundWinner = winner;
  r.ticksInPhase = 0;
  r.phase = MatchPhase::RoundEnded;
  sink.push(SimEvent{EventKind::RoundEnded, s.tick, winner, EntityId{}, r.round, 0});
  if (!winner.isValid()) return;  // draw: nobody scores
  uint8_t& score = r.scores[winner.value];
  ++score;
  if (score >= content.mode.roundsToWin) {
    r.phase = MatchPhase::MatchEnded;
    sink.push(SimEvent{EventKind::MatchEnded, s.tick, winner, EntityId{}, 0, 0});
  }
}

}  // namespace kfm::sim
