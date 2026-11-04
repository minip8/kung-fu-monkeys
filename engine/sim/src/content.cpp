// Default content tables (architecture §5.10): constexpr defs built from
// integer ratios only, and the content hash over their canonical serialized
// form. They mirror content/*.toml exactly (tests/unit/content asserts the
// loaded TOML hashes the same), and stay as the sim tests' fixture.
#include "kfm/sim/content.hpp"

#include "kfm/core/assert.hpp"
#include "kfm/core/bitstream.hpp"
#include "kfm/core/hash.hpp"
#include "kfm/sim/visit.hpp"

#include <array>
#include <cstddef>

namespace kfm::sim {
namespace {

// Units per second -> units per tick.
constexpr Fixed perSecond(int32_t unitsPerSecond) {
  return Fixed::fromRatio(unitsPerSecond, static_cast<int32_t>(kTickHz));
}

constexpr ArenaDef makeArena() {
  ArenaDef a;
  a.name = ContentName::of("dojo");
  a.halfExtents = Vec2{Fixed::fromInt(16), Fixed::fromInt(9)};
  // Ten points on an ellipse (radii 12 × 6) in steps of 36 degrees, listed in an
  // order that spreads the first few slots across the arena (2 players face
  // each other across the centre).
  constexpr std::array<int32_t, kMaxPlayers> kOrder = {0, 5, 2, 7, 4, 9, 6, 1, 8, 3};
  const Fixed rx = Fixed::fromInt(12);
  const Fixed ry = Fixed::fromInt(6);
  for (uint32_t i = 0; i < kMaxPlayers; ++i) {
    const Angle angle = Angle::fromDegrees(kOrder[i] * 36);
    a.spawnPoints[i] = Vec2{cos(angle) * rx, sin(angle) * ry};
  }
  a.spawnPointCount = kMaxPlayers;
  return a;
}

constexpr ContentDB makeDefaultContent() {
  ContentDB c;
  c.abilities[0].name = ContentName::of("none");

  AbilityDef& dash = c.abilities[static_cast<uint8_t>(AbilityKind::Dash)];
  dash.name = ContentName::of("dash");
  dash.kind = AbilityKind::Dash;
  dash.cooldownTicks = 45;
  dash.startupTicks = 0;
  dash.activeTicks = 12;
  dash.recoveryTicks = 6;
  dash.perfectWindowTicks = 4;
  dash.speed = perSecond(22);
  dash.tagsActive = Tags::of(Tag::Dashing);

  AbilityDef& thr = c.abilities[static_cast<uint8_t>(AbilityKind::Throw)];
  thr.name = ContentName::of("throw");
  thr.kind = AbilityKind::Throw;
  thr.cooldownTicks = 20;
  thr.startupTicks = 2;
  thr.activeTicks = 1;
  thr.recoveryTicks = 6;

  c.abilityCount = 3;  // none, dash, throw

  c.boomerang.radius = Fixed::fromRatio(35, 100);
  c.boomerang.speed = perSecond(16);
  c.boomerang.outboundTicks = 36;
  c.boomerang.maxReturnTicks = 240;
  c.boomerang.turnRateBam = 1200;
  c.boomerang.restitution = Fixed::one();
  // Clear of the thrower's contact radius (0.5 + 0.35): no own contact on the spawn tick.
  c.boomerang.spawnOffset = Fixed::one();
  c.boomerang.damage = 1;

  c.pickup.radius = Fixed::half();

  c.player.radius = Fixed::half();
  c.player.moveSpeed = perSecond(7);
  c.player.maxHp = 1;
  c.player.ammoMax = 1;
  c.player.ammoStart = 1;
  c.player.hitStunTicks = 6;
  c.player.abilities[kAbilitySlotDash] = static_cast<uint8_t>(AbilityKind::Dash);
  c.player.abilities[kAbilitySlotThrow] = static_cast<uint8_t>(AbilityKind::Throw);
  c.loadouts[0].name = ContentName::of(kBaseLoadoutName);
  c.loadouts[0].abilities = c.player.abilities;
  c.loadoutCount = 1;

  c.arenas[0] = makeArena();
  c.arenaCount = 1;

  ModeDef& rounds = c.modes[0];
  rounds.name = ContentName::of("rounds");
  rounds.roundsToWin = 3;
  rounds.roundEndTicks = 120;
  rounds.roundStartTicks = 60;
  c.modeCount = 1;

  c.dashAttack.damage = 1;
  c.dashAttack.minAmmo = 1;
  c.dashAttack.consumesAmmo = 0;
  return c;
}

constexpr ContentDB kDefaultContent = makeDefaultContent();

// Preconditions the systems rely on rather than re-check every tick. M4's
// loader validates the same properties at load time.
constexpr const AbilityDef& kDashDef = kDefaultContent.abilityForSlot(kAbilitySlotDash);
static_assert(kDashDef.kind == AbilityKind::Dash &&
                  kDefaultContent.abilityForSlot(kAbilitySlotThrow).kind == AbilityKind::Throw,
              "the base loadout slots hold Dash and Throw (AbilitySystem button mapping)");
static_assert(kDashDef.startupTicks == 0,
              "PerfectDash is granted from the press tick (§5.5): the dash has no startup");
static_assert(kDashDef.activeTicks > kDashDef.perfectWindowTicks,
              "the perfect window, plus its sub-tick tail, fits inside the active phase");
static_assert(kDefaultContent.arenas[0].halfExtents.x <= Fixed::fromInt(1024) &&
                  kDefaultContent.arenas[0].halfExtents.y <= Fixed::fromInt(1024),
              "the swept narrowphase needs coordinates within ±1024 (collision_internal.hpp)");
static_assert(kDefaultContent.boomerang.spawnOffset >
                  kDefaultContent.player.radius + kDefaultContent.boomerang.radius,
              "a thrown banana spawns clear of its thrower (§5.7)");
static_assert(kDefaultContent.dashAttack.minAmmo >= 1,
              "the default dash attack needs a banana in hand (§5.6); minAmmo = 0 would make "
              "every dash an attack");
static_assert(kDefaultContent.dashAttack.consumesAmmo <= 1, "consumesAmmo is a 0/1 switch");

}  // namespace

const ContentDB& defaultContent() {
  return kDefaultContent;
}

uint32_t contentHash(const ContentDB& content) {
  // Every field is serialized at its own width with no padding, so the
  // canonical form never exceeds the in-memory size.
  std::array<std::byte, sizeof(ContentDB)> buf{};
  BitWriter w(buf);
  const bool ok = writeTo(w, content);
  KFM_CHECK(ok);
  return xxh32(w.written());
}

}  // namespace kfm::sim
