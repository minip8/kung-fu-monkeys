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

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <utility>

namespace kfm::sim {
namespace {

// v0.32: an arena with several spawn groups spawns each team in its own
// group (spawnRangeOf): one world RNG offset per group, drawn in group order,
// then the group's present slots in slot order take its points from that
// offset on, wrapping. One group, or a free-for-all mode (v0.91), spreads the
// players instead (spawnSpread).
void spawnByGroups(WorldState& s, const MatchContent& content, EventSink& sink) {
  const ArenaDef& arena = content.arena;
  std::array<uint32_t, kMaxPlayers> offset{};
  std::array<uint32_t, kMaxPlayers> rank{};
  for (uint8_t g = 0; g < arena.spawnGroups && g < kMaxPlayers; ++g) {
    const SpawnRange r = spawnGroupRange(arena, g);
    offset[g] = r.end > r.begin ? s.rng.below(r.end - r.begin) : 0;
  }
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const PlayerId id{i};
    if (!maskHas(s.rules.present, id)) continue;
    const SpawnRange r = spawnRangeOf(content, id);
    const uint8_t g = static_cast<uint8_t>(content.teamOf(id) % arena.spawnGroups);
    const uint32_t size = r.end - r.begin;
    const EntityId e = s.players.isAliveIndex(i) ? s.players.idAt(i) : s.players.spawnAt(i);
    const Vec2 pos =
        size > 0 ? arena.spawnPoints[r.begin + ((rank[g] + offset[g]) % size)] : Vec2::zero();
    ++rank[g];
    resetPlayer(s.players.items[e.index], id, pos, content);
    sink.push(SimEvent{EventKind::PlayerSpawned, s.tick, id, e, 0, 0});
  }
}

// v0.91: one spawn list (a free-for-all, or an arena with one group). The n
// present slots start on the n points that lie farthest apart
// (spreadSpawnSet), each round dealt over them afresh: the world Rng shuffles
// the points (Fisher-Yates, n - 1 draws) and the present slots in slot order
// take them in that order. With more present slots than points, the M1 rule:
// one Rng offset, slots in order from it, wrapping.
void spawnSpread(WorldState& s, const MatchContent& content, EventSink& sink) {
  const ArenaDef& arena = content.arena;
  const uint32_t points = std::min<uint32_t>(arena.spawnPointCount, kMaxPlayers);
  const auto present = static_cast<uint32_t>(std::popcount(static_cast<uint32_t>(s.rules.present)));
  std::array<uint8_t, kMaxPlayers> order{};
  uint32_t dealt = 0;
  uint32_t offset = 0;
  if (present <= points) {
    const uint16_t set = spreadSpawnSet(arena, present);
    for (uint32_t k = 0; k < points; ++k) {
      if ((set & (1u << k)) != 0) order[dealt++] = static_cast<uint8_t>(k);
    }
    for (uint32_t k = dealt; k > 1; --k) {
      std::swap(order[k - 1], order[s.rng.below(k)]);
    }
  } else if (points > 0) {
    offset = s.rng.below(points);
  }
  uint32_t rank = 0;
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const PlayerId id{i};
    if (!maskHas(s.rules.present, id)) continue;
    const EntityId e = s.players.isAliveIndex(i) ? s.players.idAt(i) : s.players.spawnAt(i);
    Vec2 pos = Vec2::zero();
    if (rank < dealt) {
      pos = arena.spawnPoints[order[rank]];
    } else if (points > 0) {
      pos = arena.spawnPoints[(rank + offset) % points];
    }
    ++rank;
    resetPlayer(s.players.items[e.index], id, pos, content);
    sink.push(SimEvent{EventKind::PlayerSpawned, s.tick, id, e, 0, 0});
  }
}

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

  if (spawnsByGroup(content)) {
    spawnByGroups(s, content, sink);
  } else {
    spawnSpread(s, content, sink);
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
