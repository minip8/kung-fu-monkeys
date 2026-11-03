// Scripted input generator: test and tooling scaffolding, not an IAgent
// (architecture §8, §9). It drives the replay recorder (`kfm_replay
// record-scripted`), the bench, the determinism scenarios and later the
// netsim with a deterministic, varied match, so it lives in engine/sim where
// all of them can share it. Like the sim it is fixed-point and allocation-free;
// unlike a system it never writes WorldState. Its only side effect is on the
// Rng the caller passes in, which must be separate from WorldState::rng.
//
// Behaviour per alive player, each tick:
//   - wander: keep the current stick direction (read back from the player's
//     decoded intent), turn to a random one about once a second, and steer
//     back toward the interior near a wall; moveMag 255;
//   - with no ammo, walk toward the nearest banana pickup;
//   - aim at the nearest other alive player (with a little jitter);
//   - throw about twice a second while ammo > 0 and the Throw slot is ready;
//   - dash now and then, and often toward a non-own banana in flight that is
//     close and approaching (a catch attempt), pressing at a random sub-tick;
//   - now and then, while holding enough ammo for a dash attack, dash at the
//     nearest other player when it is close (§5.6);
//   - a button is only pressed when it was up on the previous tick, so every
//     press is a real edge (a press is held for one tick, then released).
#pragma once

#include "kfm/core/fixed.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/core/types.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/world_state.hpp"

#include <cstdint>

namespace kfm::sim {

// Defaults are tuned so a 10-player match ends its first round within ~20 s
// with every gameplay event present (scripted_match_scenario.cpp).
struct ScriptedInputConfig {
  uint16_t turnOneIn{60};         // per-tick chance 1/N of a new wander direction (~1 s)
  uint16_t throwOneIn{30};        // per-tick chance 1/N of a throw when able
  uint16_t dashOneIn{180};        // per-tick chance 1/N of a random dash when able
  uint8_t dodgePer256{32};        // per-tick chance of dashing at an incoming banana
  uint16_t dashAttackOneIn{240};  // per-tick chance 1/N of a dash at a close enemy
  Fixed dashAttackRadius{Fixed::fromInt(3)};  // how close that enemy must be
  Fixed threatRadius{Fixed::fromInt(2)};      // how close an incoming banana must be
  Fixed pickupRadius{Fixed::fromInt(40)};     // how far an empty-handed player looks for ammo
  Fixed wallMargin{Fixed::fromInt(2)};        // turn back when this close to a wall
  uint8_t aimJitter{2};                       // ± 1/256 turns of random aim error
};

// The input for one player this tick. Returns an idle input for a player
// that is not alive.
[[nodiscard]] PlayerInput scriptedInput(Rng& rng, const WorldState& s, const MatchContent& content,
                                        PlayerId id, const ScriptedInputConfig& config = {});

// A whole frame for s.tick: present = s.rules.present, inputs for every
// present slot in slot order (so the Rng is consumed in a fixed order).
[[nodiscard]] InputFrame scriptedFrame(Rng& rng, const WorldState& s, const MatchContent& content,
                                       const ScriptedInputConfig& config = {});

}  // namespace kfm::sim
