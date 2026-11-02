// Tick pipeline (architecture §5.3). M0 implements the skeleton: match
// start spawns the present players; the remaining systems are stubs in the
// documented order so M1 can fill them in without reordering anything.
#include "kfm/sim/step.hpp"

#include "kfm/core/assert.hpp"
#include "kfm/sim/visit.hpp"

namespace kfm::sim {
namespace {

// Spawn area for M0: a square of half-extent kSpawnHalfExtent around the origin.
constexpr Fixed kSpawnHalfExtent = Fixed::fromInt(8);

void spawnPlayer(WorldState& s, PlayerId id, EventSink& sink) {
  const EntityId e = s.players.spawnAt(static_cast<uint16_t>(id.value));
  KFM_ASSERT(e.isValid());
  if (!e.isValid()) return;  // slot already occupied: cannot happen from a fresh lobby
  Player& p = s.players.items[e.index];
  p.id = id;
  p.transform.pos = Vec2{s.rng.between(-kSpawnHalfExtent, kSpawnHalfExtent),
                         s.rng.between(-kSpawnHalfExtent, kSpawnHalfExtent)};
  p.transform.vel = Vec2::zero();
  p.transform.facing = s.rng.angle();
  sink.push(SimEvent{EventKind::PlayerSpawned, s.tick, id, e, 0, 0});
}

// 1. RulesSystem::preTick: round start/end, spawn waves, pickup spawners.
void rulesPreTick(WorldState& s, EventSink& sink) {
  if (s.rules.phase == MatchPhase::Lobby) {
    s.rules.phase = MatchPhase::Playing;
    s.rules.ticksInPhase = 0;
    for (uint8_t i = 0; i < kMaxPlayers; ++i) {
      const PlayerId id{i};
      if (maskHas(s.rules.present, id)) spawnPlayer(s, id, sink);
    }
    sink.push(SimEvent{EventKind::MatchStarted, s.tick, PlayerId::invalid(), EntityId{}, 0, 0});
  }
}

// 2. InputSystem: decode PlayerInput into intent, press-edge detection. (M1)
void inputSystem(WorldState& s, const InputFrame& in) {
  s.players.forEachAlive([&](EntityId, Player& p) {
    const PlayerInput& pi = in.inputs[p.id.value];
    if (pi.moveDir != 0) p.transform.facing = Angle::fromInput8(pi.moveDir);
  });
}

// 3. AbilitySystem (M1)
// 4. MovementSystem: intent × stats → velocity; integrate; arena clamp. (M1)
void movementSystem(WorldState& s) {
  s.players.forEachAlive([](EntityId, Player& p) { p.transform.pos += p.transform.vel; });
}

// 5. ProjectileSystem (M1)
// 6. CollisionSystem (M1)
// 7. InteractionSystem (M1)
// 8. ModifierSystem (M4)
// 9. LifecycleSystem (M1)

// 10. RulesSystem::postTick: win conditions, scoring.
void rulesPostTick(WorldState& s) {
  ++s.rules.ticksInPhase;
}

}  // namespace

void step(WorldState& s, const InputFrame& in, EventSink& sink) {
  KFM_ASSERT(in.tick == s.tick);
  rulesPreTick(s, sink);
  inputSystem(s, in);
  movementSystem(s);
  rulesPostTick(s);
  ++s.tick;
}

uint32_t checksum(const WorldState& s) {
  return checksumOf(s);
}

}  // namespace kfm::sim
