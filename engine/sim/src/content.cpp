// M1 content tables (architecture §5.10): constexpr defs built from integer
// ratios only, and the content hash over their canonical serialized form.
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

  AbilityDef& dash = c.abilities[static_cast<uint8_t>(AbilityKind::Dash)];
  dash.kind = AbilityKind::Dash;
  dash.cooldownTicks = 45;
  dash.startupTicks = 0;
  dash.activeTicks = 12;
  dash.recoveryTicks = 6;
  dash.perfectWindowTicks = 4;
  dash.speed = perSecond(22);
  dash.tagsActive = Tags::of(Tag::Dashing);

  AbilityDef& thr = c.abilities[static_cast<uint8_t>(AbilityKind::Throw)];
  thr.kind = AbilityKind::Throw;
  thr.cooldownTicks = 20;
  thr.startupTicks = 2;
  thr.activeTicks = 1;
  thr.recoveryTicks = 6;

  c.abilityCount = static_cast<uint8_t>(AbilityKind::kCount);

  c.boomerang.radius = Fixed::fromRatio(35, 100);
  c.boomerang.speed = perSecond(16);
  c.boomerang.outboundTicks = 36;
  c.boomerang.maxReturnTicks = 240;
  c.boomerang.turnRateBam = 1200;
  c.boomerang.restitution = Fixed::one();
  c.boomerang.spawnOffset = Fixed::fromRatio(8, 10);
  c.boomerang.damage = 1;
  c.boomerang.pierce = 0;

  c.player.radius = Fixed::half();
  c.player.moveSpeed = perSecond(7);
  c.player.maxHp = 1;
  c.player.ammoMax = 1;
  c.player.ammoStart = 1;
  c.player.hitStunTicks = 6;
  c.player.abilities[kAbilitySlotDash] = static_cast<uint8_t>(AbilityKind::Dash);
  c.player.abilities[kAbilitySlotThrow] = static_cast<uint8_t>(AbilityKind::Throw);

  c.arena = makeArena();

  c.mode.roundsToWin = 3;
  c.mode.roundEndTicks = 120;
  c.mode.roundStartTicks = 60;
  return c;
}

constexpr ContentDB kDefaultContent = makeDefaultContent();

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
