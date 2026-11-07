// LifecycleSystem (architecture §5.3 step 9, §5.7, §5.8): spawn protection and
// respawns (v0.23), pit falls (v0.57), despawns players tagged Eliminated
// (already reported by InteractionSystem), turns grounded boomerangs into
// banana pickups at their position, and runs the arena's power-up spawners
// (v0.20).
//
// Pit falls run in every match phase, after the respawn timers: a falling
// monkey's fallTicks counts down and at 0 it is eliminated by nobody
// (rules/fall.hpp), so it despawns in the same pass. A fall that starts on
// tick T (fallTicks = PlayerDef::fallTicks = N) ends on tick T + N - 1.
//
// v0.57, bananas never disappear (rules/bananas.hpp): an eliminated monkey's
// bananas still away from it (outbound or returning) drop where they are as
// it despawns (the pit row already dropped a faller's when its fall began),
// so they come down as pickups below like any grounded banana, on the tick
// of the death; the ones in its hand drop too, scattered around it, unless
// it fell down a pit with them. A monkey holding more than its ammoMax (a
// power-up ran out) drops the extra ones the same way. A grounded banana whose disc (the
// banana's contact radius) lies wholly over the pits (discOverPits, the
// monkeys' own test) falls into the hole instead: Sinking, BoomerangFell,
// gone after BoomerangDef::pitSinkTicks; so no pickup ever lies where only a
// fall could reach it.
//
// Respawns are driven by MatchRules' timers, which only modes that respawn
// (deathmatch) set, so the code is the same for every mode: during Playing a
// dead present slot with a pending respawnTicks counts down and respawns at
// 0 (modes/mode_flow.hpp: farthest spawn point from the other monkeys,
// Tag::Invulnerable for respawnInvulnTicks); an alive player's invulnTicks
// counts down and clears the tag at 0. The countdown runs before this tick's
// despawns, so a player eliminated on tick T respawns on tick
// T + respawnTicks and is protected for the interactions of the next
// respawnInvulnTicks ticks.
//
// Spawners (§5.8) fire only while the match phase is Playing, on the ticks
// where ticksInPhase (0 on the first Playing tick) equals firstTicks +
// k × intervalTicks (k >= 0; intervalTicks 0 = only once), in arena order,
// after the bananas. A spawner with maxAlive of its own pickups lying in the
// arena skips that beat. The power-up is drawn from its set with the world
// RNG (one rng.below() per spawn, none otherwise), so an arena without
// spawners never touches the RNG here. The spawner's timeline is a function
// of ticksInPhase, not state of its own; a pickup remembers its spawner.
#include "kfm/sim/systems.hpp"
#include "modes/mode_flow.hpp"
#include "rules/bananas.hpp"
#include "rules/fall.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace kfm::sim {
namespace {

void respawnTimers(WorldState& s, const MatchContent& content, EventSink& sink) {
  MatchRules& r = s.rules;
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const PlayerId id{i};
    if (Player* p = s.playerBySlot(id)) {
      if (r.invulnTicks[i] == 0) continue;
      // A power-up's Invulnerable (Player::grantedTags, §5.8) outlives it.
      if (--r.invulnTicks[i] == 0 && !p->grantedTags.has(Tag::Invulnerable)) {
        p->tags.clear(Tag::Invulnerable);
      }
      continue;
    }
    if (r.respawnTicks[i] == 0) continue;
    if (!maskHas(r.present, id)) {
      r.respawnTicks[i] = 0;  // left while dead: no respawn
      continue;
    }
    if (--r.respawnTicks[i] == 0) respawnPlayer(s, content, id, sink);
  }
}

[[nodiscard]] bool spawnerBeat(const PickupSpawner& sp, uint32_t t) {
  if (t < sp.firstTicks) return false;
  const uint32_t since = t - sp.firstTicks;
  return sp.intervalTicks == 0 ? since == 0 : since % sp.intervalTicks == 0;
}

[[nodiscard]] uint32_t aliveFrom(const WorldState& s, uint8_t spawner) {
  uint32_t n = 0;
  s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& k) {
    if (k.kind == PickupKind::PowerUp && k.spawner == spawner) ++n;
  });
  return n;
}

// The power-ups a spawner may draw: its set (0 = all) within the DB and the
// match's power-up set (MatchConfig::powerUps).
[[nodiscard]] uint32_t candidates(const PickupSpawner& sp, const MatchContent& content) {
  const ContentDB& db = content.db;
  const uint32_t all = db.powerUpCount >= 32 ? UINT32_MAX : (uint32_t{1} << db.powerUpCount) - 1u;
  const uint32_t own = sp.powerUpSet == 0 ? all : (uint32_t{sp.powerUpSet} & all);
  return own & content.powerUps;
}

// Index of the n-th set bit of `bits` (n < popcount(bits)).
[[nodiscard]] uint8_t nthSetBit(uint32_t bits, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    bits &= bits - 1u;  // drop the lowest set bit
  return static_cast<uint8_t>(std::countr_zero(bits));
}

void runSpawners(WorldState& s, const MatchContent& content, EventSink& sink) {
  if (s.rules.phase != MatchPhase::Playing) return;
  const ArenaDef& arena = content.arena;
  const uint32_t t = s.rules.ticksInPhase;
  for (uint8_t i = 0; i < arena.pickupSpawnerCount && i < kMaxPickupSpawners; ++i) {
    const PickupSpawner& sp = arena.pickupSpawners[i];
    if (!spawnerBeat(sp, t) || aliveFrom(s, i) >= sp.maxAlive) continue;
    const uint32_t set = candidates(sp, content);
    if (set == 0) continue;
    const EntityId id = s.pickups.spawn();
    if (!id.isValid()) return;  // pickup pool full: no spawner fires this tick
    const uint8_t def = nthSetBit(set, s.rng.below(static_cast<uint32_t>(std::popcount(set))));
    Pickup& pickup = s.pickups.items[id.index];
    pickup.kind = PickupKind::PowerUp;
    pickup.pos = sp.pos;
    pickup.powerUpIndex = def;
    pickup.spawner = i;
    sink.push(SimEvent{EventKind::PickupSpawned, s.tick, PlayerId::invalid(), id,
                       static_cast<uint32_t>(PickupKind::PowerUp), def});
  }
}

}  // namespace

void lifecycleSystem(WorldState& s, const MatchContent& content, EventSink& sink) {
  if (s.rules.phase == MatchPhase::Playing) respawnTimers(s, content, sink);

  s.players.forEachAlive([&](EntityId /*id*/, Player& p) {
    if (!p.tags.has(Tag::Falling) || p.tags.has(Tag::Eliminated)) return;
    if (p.fallTicks > 0) --p.fallTicks;
    if (p.fallTicks == 0) eliminateFallen(s, content, p, sink);
  });

  s.players.forEachAlive([&](EntityId id, Player& p) {
    if (!p.tags.has(Tag::Eliminated)) {
      // v0.57: more held than ammoMax allows (a power-up ran out): the
      // extra ones drop around the monkey.
      if (p.ammo > p.stats.ammoMax) {
        dropHeldBananas(s, content, p, static_cast<uint8_t>(p.ammo - p.stats.ammoMax), sink);
      }
      return;
    }
    s.rules.invulnTicks[id.index] = 0;
    // v0.57: its bananas come down where they are, the held ones scattered
    // around it, unless it fell down a pit holding them.
    dropBananasOf(s, p.id);
    if (!p.tags.has(Tag::Falling)) dropHeldBananas(s, content, p, p.ammo, sink);
    s.players.despawn(id);
  });

  // Iterates boomerangs and spawns pickups: a different pool, so the
  // forEachAlive no-spawn rule holds.
  const BoomerangDef& bananaDef = content.db.boomerang;
  s.boomerangs.forEachAlive([&](EntityId id, Boomerang& b) {
    // v0.57: a banana that has sunk into a pit is gone.
    if (b.phase == BoomerangPhase::Sinking &&
        b.ticksInPhase >= std::max<uint16_t>(bananaDef.pitSinkTicks, 1)) {
      s.boomerangs.despawn(id);
      return;
    }
    if (b.phase != BoomerangPhase::Grounded) return;
    const Vec2 pos = b.transform.pos;
    // v0.57: come down wholly over a pit, it drops into the hole.
    if (discOverPits(content.arena, pos, bananaDef.radius * b.radiusMul)) {
      b.phase = BoomerangPhase::Sinking;
      b.ticksInPhase = 0;
      b.transform.vel = Vec2::zero();
      sink.push(SimEvent{EventKind::BoomerangFell, s.tick, b.owner, id, 0, 0});
      return;
    }
    s.boomerangs.despawn(id);
    sink.push(SimEvent{EventKind::BoomerangGrounded, s.tick, PlayerId::invalid(), id, 0, 0});
    // v0.57: a full pickup pool stacks it on the nearest banana pickup.
    layBananaPickup(s, pos, sink);
  });

  runSpawners(s, content, sink);
}

}  // namespace kfm::sim
