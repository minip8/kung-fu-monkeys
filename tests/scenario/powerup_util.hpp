// The power-up scenarios' content (architecture §5.8): defaultContent() plus
// the shipped throw_fast ability and the five M4 power-ups. The defs mirror
// content/abilities.toml and content/powerups.toml (the sim tests never
// depend on the loader); kfm_content_tests' mirror test checks they still
// equal the TOML.
#pragma once

#include "scenario_util.hpp"

#include <cstdint>

namespace kfm::sim::test {

// Indices of the shipped power-ups, in powerups.toml order.
inline constexpr uint8_t kSpeed = 0;
inline constexpr uint8_t kBig = 1;
inline constexpr uint8_t kTriple = 2;
inline constexpr uint8_t kShield = 3;
inline constexpr uint8_t kFastHands = 4;

[[nodiscard]] inline ContentDB powerUpContent() {
  ContentDB c = defaultContent();
  AbilityDef fast = c.abilities[c.player.abilities[kAbilitySlotThrow]];
  fast.name = ContentName::of("throw_fast");
  fast.cooldownTicks = 8;
  fast.startupTicks = 1;
  fast.activeTicks = 1;
  fast.recoveryTicks = 3;
  fast.speed = (fast.param1 - fast.param0) / 20;  // charge_ticks = 20: twice the throw's rate
  const uint8_t fastIndex = c.abilityCount++;
  c.abilities[fastIndex] = fast;

  ProjectileModifier big;
  big.radiusMul = Fixed::fromInt(2);
  big.speedMul = Fixed::fromRatio(85, 100);
  const auto looks = [&c](uint8_t index, uint8_t color, PowerUpShape shape) {
    c.powerUps[index].visual = PowerUpVisual{color, shape};  // presentation only, but hashed
  };
  looks(addPowerUp(c, "speed_boost", 480,
                   {statGrant(StatKind::MoveSpeed, Fixed::fromRatio(3, 2), Fixed::zero())}),
        6, PowerUpShape::Diamond);
  looks(addPowerUp(c, "big_banana", 600, {projectileGrant(big)}), 2, PowerUpShape::Orb);
  looks(addPowerUp(c, "triple_banana", 900,
                   {statGrant(StatKind::AmmoMax, Fixed::one(), Fixed::fromInt(2))}),
        4, PowerUpShape::Coin);
  looks(addPowerUp(c, "shield", 900, {tagGrant(Tag::Shielded)}), 1, PowerUpShape::Orb);
  looks(addPowerUp(c, "fast_hands", 600, {abilityGrant(kAbilitySlotThrow, fastIndex)}), 3,
        PowerUpShape::Cube);
  return c;
}

}  // namespace kfm::sim::test
