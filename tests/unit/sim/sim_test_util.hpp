// Shared helpers for sim unit and scenario tests: build states by hand,
// script inputs, run ticks and collect events. Tests may allocate; the sim
// may not.
#pragma once

#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/step.hpp"
#include "kfm/sim/world_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace kfm::sim::test {

// Events of several ticks, in emission order.
using EventLog = std::vector<SimEvent>;

// v0.48: every throw def without its charge (a tap at the boomerang's own
// speed, the Startup never held), for tests about the ability machine, flight
// and catch timing rather than charging.
inline void removeThrowCharge(ContentDB& c) {
  for (uint8_t i = 0; i < c.abilityCount && i < kMaxAbilityDefs; ++i) {
    AbilityDef& d = c.abilities[i];
    if (d.kind != AbilityKind::Throw) continue;
    d.speed = Fixed{};
    d.param0 = Fixed{};
    d.param1 = Fixed{};
  }
}

// defaultContent() with removeThrowCharge applied.
[[nodiscard]] inline const ContentDB& unchargedContent() {
  static const ContentDB c = [] {
    ContentDB d = defaultContent();
    removeThrowCharge(d);
    return d;
  }();
  return c;
}

// An input frame for the world's current tick with every present player idle.
[[nodiscard]] inline InputFrame frameFor(const WorldState& s) {
  InputFrame f;
  f.tick = s.tick;
  f.present = s.rules.present;
  return f;
}

// Steps n ticks with the same per-player inputs (frame.tick is rewritten to
// the world's tick each time) and appends every event to log.
inline void runTicks(WorldState& s, const InputFrame& frame, int n, const MatchContent& content,
                     EventLog& log) {
  EventSink sink;
  InputFrame f = frame;
  for (int i = 0; i < n; ++i) {
    f.tick = s.tick;
    sink.clear();
    step(s, f, content, sink);
    REQUIRE(sink.dropped == 0);
    log.insert(log.end(), sink.begin(), sink.end());
  }
}

// A world whose players exist and whose match is in the Playing phase: steps
// idle frames from the lobby until play starts (one tick with the M0 rules,
// the round countdown once RulesSystem implements it).
[[nodiscard]] inline WorldState makePlayingWorld(uint64_t seed, PlayerMask playerMask,
                                                 const ContentDB& content = defaultContent()) {
  WorldState s = initialWorld(MatchConfig{seed, playerMask});
  EventLog log;
  for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i) {
    runTicks(s, frameFor(s), 1, content, log);
  }
  REQUIRE(s.rules.phase == MatchPhase::Playing);
  return s;
}

// The player in a slot, failing the test when there is none. Use this rather
// than dereferencing playerBySlot(): the abort tells the optimiser the
// reference is never null (Release builds with -Wnull-dereference).
inline Player& playerAt(WorldState& s, PlayerId id) {
  Player* p = s.playerBySlot(id);
  if (p == nullptr) {
    FAIL_CHECK("no player in slot " << int{id.value});
    std::abort();
  }
  return *p;
}
inline const Player& playerAt(const WorldState& s, PlayerId id) {
  return playerAt(const_cast<WorldState&>(s), id);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
}

// Teleports a player (pos and prevPos, zero velocity).
inline Player& placePlayer(WorldState& s, PlayerId id, Vec2 pos) {
  Player& p = playerAt(s, id);
  p.transform.pos = pos;
  p.transform.prevPos = pos;
  p.transform.vel = Vec2::zero();
  return p;
}

// Spawns a banana owned by `owner` at pos moving with vel (units/tick).
// damage comes from content.
inline EntityId spawnBanana(WorldState& s, PlayerId owner, Vec2 pos, Vec2 vel,
                            BoomerangPhase phase = BoomerangPhase::Outbound,
                            const ContentDB& content = defaultContent()) {
  const EntityId id = s.boomerangs.spawn();
  REQUIRE(id.isValid());
  Boomerang& b = s.boomerangs.items[id.index];
  b.owner = owner;
  b.phase = phase;
  b.transform.pos = pos;
  b.transform.prevPos = pos;
  b.transform.vel = vel;
  b.damage = content.boomerang.damage;
  return id;
}

// Spawns a banana pickup lying at pos.
inline EntityId spawnPickup(WorldState& s, Vec2 pos) {
  const EntityId id = s.pickups.spawn();
  REQUIRE(id.isValid());
  Pickup& p = s.pickups.items[id.index];
  p.kind = PickupKind::Banana;
  p.pos = pos;
  return id;
}

// --- power-ups (§5.8) ------------------------------------------------------

[[nodiscard]] inline PowerUpGrant statGrant(StatKind stat, Fixed mul, Fixed add) {
  PowerUpGrant g;
  g.kind = GrantKind::Stat;
  g.stat = StatModifier{stat, mul, add};
  return g;
}
[[nodiscard]] inline PowerUpGrant abilityGrant(uint8_t slot, uint8_t ability) {
  PowerUpGrant g;
  g.kind = GrantKind::AbilityOverride;
  g.slot = slot;
  g.ability = ability;
  return g;
}
[[nodiscard]] inline PowerUpGrant projectileGrant(const ProjectileModifier& m) {
  PowerUpGrant g;
  g.kind = GrantKind::Projectile;
  g.projectile = m;
  return g;
}
[[nodiscard]] inline PowerUpGrant tagGrant(Tag t) {
  PowerUpGrant g;
  g.kind = GrantKind::Tag;
  g.tag = t;
  return g;
}

// Appends a power-up def to `c`; returns its index.
inline uint8_t addPowerUp(ContentDB& c, std::string_view name, uint16_t durationTicks,
                          std::initializer_list<PowerUpGrant> grants) {
  REQUIRE(c.powerUpCount < kMaxPowerUps);
  REQUIRE(grants.size() <= kMaxPowerUpGrants);
  PowerUpDef& d = c.powerUps[c.powerUpCount];
  d = PowerUpDef{};
  d.name = ContentName::of(name);
  d.durationTicks = durationTicks;
  for (const PowerUpGrant& g : grants)
    d.grants[d.grantCount++] = g;
  return c.powerUpCount++;
}

// Puts a ModifierList entry on a player directly (as the pickup rule would).
inline void givePowerUp(Player& p, uint8_t powerUp, uint16_t ticksLeft) {
  REQUIRE(p.modifiers.count < kMaxModifiers);
  p.modifiers.items[p.modifiers.count++] = ActiveModifier{powerUp, ticksLeft};
}

// Spawns a power-up pickup of def `powerUp` lying at pos.
inline EntityId spawnPowerUpPickup(WorldState& s, Vec2 pos, uint8_t powerUp, uint8_t spawner = 0) {
  const EntityId id = s.pickups.spawn();
  REQUIRE(id.isValid());
  Pickup& p = s.pickups.items[id.index];
  p.kind = PickupKind::PowerUp;
  p.pos = pos;
  p.powerUpIndex = powerUp;
  p.spawner = spawner;
  return id;
}

// Counts events of one kind in an EventSink or an EventLog.
template <class Events>
[[nodiscard]] int countEvents(const Events& events, EventKind kind) {
  int n = 0;
  for (const SimEvent& e : events) {
    if (e.kind == kind) ++n;
  }
  return n;
}

// First event of a kind, or nullptr.
template <class Events>
[[nodiscard]] const SimEvent* findEvent(const Events& events, EventKind kind) {
  for (const SimEvent& e : events) {
    if (e.kind == kind) return &e;
  }
  return nullptr;
}

// First event of a kind, failing the test when there is none (see playerAt
// for why this aborts rather than returning a null pointer).
template <class Events>
[[nodiscard]] const SimEvent& firstEvent(const Events& events, EventKind kind) {
  const SimEvent* e = findEvent(events, kind);
  if (e == nullptr) {
    FAIL_CHECK("no event of kind " << static_cast<int>(kind));
    std::abort();
  }
  return *e;
}

}  // namespace kfm::sim::test
