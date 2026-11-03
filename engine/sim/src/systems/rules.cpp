// RulesSystem (architecture §5.3 steps 1 and 10): FFA round flow and scoring.
// M1 stub: keeps the M0 behaviour (first tick spawns the present players and
// starts play); the round/score state machine lands with the parallel rules
// work (contract: rulesPreTick, rulesPostTick).
#include "kfm/core/assert.hpp"
#include "kfm/sim/systems.hpp"

namespace kfm::sim {
namespace {

void spawnPlayer(WorldState& s, const ContentDB& content, PlayerId id, Vec2 pos, EventSink& sink) {
  const EntityId e = s.players.spawnAt(static_cast<uint16_t>(id.value));
  KFM_ASSERT(e.isValid());
  if (!e.isValid()) return;  // slot already occupied: cannot happen from a fresh lobby
  Player& p = s.players.items[e.index];
  p.id = id;
  p.transform.pos = pos;
  p.transform.prevPos = pos;
  p.stats = Stats{content.player.moveSpeed, content.player.maxHp, content.player.ammoMax};
  p.hp = content.player.maxHp;
  p.ammo = content.player.ammoStart;
  for (uint8_t slot = 0; slot < kAbilitySlots; ++slot) {
    p.abilities[slot].defIndex = content.player.abilities[slot];
  }
  sink.push(SimEvent{EventKind::PlayerSpawned, s.tick, id, e, 0, 0});
}

}  // namespace

void rulesPreTick(WorldState& s, const ContentDB& content, EventSink& sink) {
  if (s.rules.phase != MatchPhase::Lobby) return;
  s.rules.phase = MatchPhase::Playing;
  s.rules.ticksInPhase = 0;
  const ArenaDef& arena = content.arena;
  KFM_ASSERT(arena.spawnPointCount > 0);
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const PlayerId id{i};
    if (!maskHas(s.rules.present, id)) continue;
    const Vec2 pos =
        arena.spawnPointCount > 0 ? arena.spawnPoints[i % arena.spawnPointCount] : Vec2::zero();
    spawnPlayer(s, content, id, pos, sink);
  }
  sink.push(SimEvent{EventKind::MatchStarted, s.tick, PlayerId::invalid(), EntityId{}, 0, 0});
}

void rulesPostTick(WorldState& s, const ContentDB& /*content*/, EventSink& /*sink*/) {
  ++s.rules.ticksInPhase;
}

}  // namespace kfm::sim
