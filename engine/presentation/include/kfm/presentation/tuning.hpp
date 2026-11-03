// Cosmetic tuning for the presentation layer (architecture §6.8, §6.9, §7).
// None of these values reach the sim; they shape how sim state is drawn.
// Lengths are in arena units (1 sim unit = 1 world unit), times in seconds.
#pragma once

#include "kfm/presentation/math.hpp"

namespace kfm::presentation {

// --- interpolation (§6.8) -------------------------------------------------------
// An entity whose position moved further than this between the two rendered
// ticks snaps to the newer tick instead of sliding (respawn, round reset,
// teleport). The fastest legitimate motion is a dash at 22/60 ≈ 0.37 units/tick.
inline constexpr float kSnapDistance = 2.0f;

// --- correction smoothing (§6.9) -------------------------------------------------
inline constexpr float kSmoothingTau = 0.12f;          // remote entities: exp(-t / tau)
inline constexpr float kLocalSmoothingTau = 0.03f;     // locally controlled players: brief blend
inline constexpr float kSmoothingSnapDistance = 1.5f;  // larger corrections snap
inline constexpr float kSmoothingEpsilon = 1e-3f;      // offsets below this are cleared

// --- cosmetic height (§7) -----------------------------------------------------------
// Monkey placeholder: a capsule standing on the ground, height = radius × this.
inline constexpr float kMonkeyHeightOverRadius = 3.2f;
// Dash hop: H × sin(pi u) over the Active phase, u = (phaseTicks + 1) / (activeTicks + 1).
inline constexpr float kDashHopHeight = 0.35f;
// Hit-stun lift: H × sin(pi u), u = 1 - hitStunTicks / (PlayerDef::hitStunTicks + 1).
inline constexpr float kHitLiftHeight = 0.4f;
// A banana in flight is carried at hand height plus an arc that is zero at the
// throw and back at the owner: rising as sin over the outbound phase, falling
// with the remaining distance to the owner while returning.
inline constexpr float kBananaHandHeight = 0.6f;
inline constexpr float kBananaArcPeak = 0.5f;
// Pickups (grounded bananas) hover and spin slowly so they read as collectable.
inline constexpr float kPickupHeight = 0.15f;
inline constexpr float kPickupBobHeight = 0.08f;
inline constexpr float kPickupBobHz = 0.8f;
inline constexpr float kPickupSpinRadPerSec = 1.5f;

// --- facing (§7) ---------------------------------------------------------------------
// Body yaw follows the move direction at most this fast (720°/s: a half turn
// in 0.25 s); aim yaw follows the aim directly.
inline constexpr float kBodyTurnRate = 4.0f * kPi;

// --- squash and stretch ------------------------------------------------------------
inline constexpr float kDashStretch = 0.18f;  // forward stretch while dashing
inline constexpr float kHitSquash = 0.2f;     // vertical squash at the start of hit-stun

// --- effects ---------------------------------------------------------------------------
inline constexpr float kDashRingSeconds = 0.25f;
inline constexpr float kPerfectCatchFlashSeconds = 0.4f;
inline constexpr float kAutoCatchFlashSeconds = 0.2f;
inline constexpr float kHitFlashSeconds = 0.2f;
inline constexpr float kThrowPuffSeconds = 0.25f;
inline constexpr float kEliminationBurstSeconds = 0.6f;
inline constexpr float kRoundStartBannerSeconds = 1.0f;
inline constexpr float kRoundEndBannerSeconds = 1.5f;
inline constexpr float kMatchEndBannerSeconds = 3.0f;
inline constexpr float kEffectBodyHeight = 0.8f;     // flashes, puffs, bursts
inline constexpr float kEffectGroundHeight = 0.02f;  // rings, just above the floor

// --- screen shake ------------------------------------------------------------------
inline constexpr float kShakeOnHit = 0.12f;  // world units of amplitude added
inline constexpr float kShakeOnElimination = 0.3f;
inline constexpr float kShakeOnPerfectCatch = 0.08f;
inline constexpr float kShakeMax = 0.5f;
inline constexpr float kShakeTau = 0.15f;  // exponential decay time constant
inline constexpr float kShakeHz = 23.0f;

}  // namespace kfm::presentation
