#include "kfm/sim/content.hpp"

#include "kfm/sim/constants.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace kfm;
using namespace kfm::sim;

TEST_CASE("contentHash is stable and sensitive to every def", "[sim][content]") {
  const ContentDB& c = defaultContent();
  const uint32_t h = contentHash(c);
  CHECK(h == contentHash(c));
  CHECK(h == contentHash(defaultContent()));

  ContentDB copy = c;
  CHECK(contentHash(copy) == h);

  copy.abilities[static_cast<uint8_t>(AbilityKind::Dash)].perfectWindowTicks += 1;
  CHECK(contentHash(copy) != h);

  copy = c;
  copy.boomerang.speed += Fixed::epsilon();
  CHECK(contentHash(copy) != h);

  copy = c;
  copy.arenas[0].spawnPoints[9].y += Fixed::epsilon();
  CHECK(contentHash(copy) != h);

  copy = c;
  copy.pickup.radius += Fixed::epsilon();
  CHECK(contentHash(copy) != h);

  copy = c;
  copy.modes[0].roundsToWin = 5;
  CHECK(contentHash(copy) != h);

  copy = c;
  copy.abilities[static_cast<uint8_t>(AbilityKind::Throw)].tagsActive.set(Tag::Shielded);
  CHECK(contentHash(copy) != h);

  copy = c;
  copy.dashAttack.minAmmo = 0;
  CHECK(contentHash(copy) != h);

  copy = c;
  copy.dashAttack.consumesAmmo = 1;
  CHECK(contentHash(copy) != h);

  copy = c;
  copy.dashAttack.damage = 2;
  CHECK(contentHash(copy) != h);
}

TEST_CASE("default content holds per-tick speeds and the M1 numbers", "[sim][content]") {
  const ContentDB& c = defaultContent();
  const auto perTick = [](int32_t unitsPerSecond) {
    return Fixed::fromRatio(unitsPerSecond, static_cast<int32_t>(kTickHz));
  };
  CHECK(c.player.moveSpeed == perTick(7));
  CHECK(c.boomerang.speed == perTick(16));
  const AbilityDef& dash = c.abilityForSlot(kAbilitySlotDash);
  const AbilityDef& thr = c.abilityForSlot(kAbilitySlotThrow);
  CHECK(dash.kind == AbilityKind::Dash);
  CHECK(thr.kind == AbilityKind::Throw);
  CHECK(dash.speed == perTick(22));
  // Every speed is well below one radius per tick, so swept tests stay sane.
  CHECK(c.player.moveSpeed < c.player.radius);
  CHECK(dash.speed < Fixed::one());
  CHECK(c.boomerang.speed < Fixed::one());

  CHECK(dash.tagsActive.has(Tag::Dashing));
  CHECK(dash.perfectWindowTicks <= dash.activeTicks);
  CHECK(thr.activeTicks >= 1);
  CHECK(c.abilityCount == static_cast<uint8_t>(AbilityKind::kCount));
  CHECK(c.player.ammoStart <= c.player.ammoMax);
  CHECK(c.boomerang.damage >= c.player.maxHp);  // base game: one hit kills
  CHECK(c.modes[0].roundsToWin == 3);
  CHECK(c.pickup.radius == Fixed::half());
  // A fresh banana spawns clear of its thrower.
  CHECK(c.boomerang.spawnOffset > c.player.radius + c.boomerang.radius);
  // Dash attack (§5.6): needs a banana in hand, keeps it, one hit kills.
  CHECK(c.dashAttack.minAmmo == 1);
  CHECK(c.dashAttack.consumesAmmo == 0);
  CHECK(c.dashAttack.damage >= c.player.maxHp);
}

TEST_CASE("arena spawn points are distinct and inside the arena", "[sim][content]") {
  const ContentDB& c = defaultContent();
  const ArenaDef& a = c.arenas[0];
  REQUIRE(a.spawnPointCount == kMaxPlayers);
  const Fixed limitX = a.halfExtents.x - c.player.radius;
  const Fixed limitY = a.halfExtents.y - c.player.radius;
  for (uint8_t i = 0; i < a.spawnPointCount; ++i) {
    const Vec2 p = a.spawnPoints[i];
    CHECK(abs(p.x) < limitX);
    CHECK(abs(p.y) < limitY);
    for (uint8_t j = 0; j < i; ++j) {
      CHECK(distance(p, a.spawnPoints[j]) > c.player.radius * 4);
    }
  }
}
