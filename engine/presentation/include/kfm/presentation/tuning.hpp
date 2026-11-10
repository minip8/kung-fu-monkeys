// Cosmetic tuning for the presentation layer (architecture §6.8, §6.9, §7).
// None of these values reach the sim; they shape how sim state is drawn.
// Lengths are in arena units (1 sim unit = 1 world unit), times in seconds.
#pragma once

#include "kfm/presentation/math.hpp"
#include "kfm/presentation/palette.hpp"

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
// v0.57: a stacked banana pickup (several bananas on one spot) draws each
// extra banana this much higher and turned this much further.
inline constexpr float kPickupStackLift = 0.12f;
inline constexpr float kPickupStackTurn = 0.9f;

// --- arena statics (§7, v0.22) -------------------------------------------------------
// Interior walls are drawn as blocks this tall: above a banana in flight
// (hand height + arc) so a bounce reads as hitting the wall, below the
// camera's framing height so a wall near the far edge never leaves frame.
inline constexpr float kArenaWallHeight = 1.2f;
// A hazard piece's height in RenderState::arenaPieces (v0.22). v0.54: the
// renderer draws hazards in the ground program, flat on the floor, from the
// pieces' footprints only.
inline constexpr float kHazardSlabHeight = 0.008f;
// The pit rim pieces' width (builder.hpp buildArenaPieces). v0.54: the
// renderer no longer draws them (the ground program draws a cliff and an ink
// line along every edge of land); the HUD's arena preview does not either.
inline constexpr float kPitRimWidth = 0.18f;
// Objective zones (v0.33): a translucent floor rectangle, opaque at its rim
// band (the inner fraction of the half-size where the band starts) so the
// edge a centre must cross reads clearly; neutral zones (hill) are white,
// a team's zone (capture) its team colour.
inline constexpr float kZoneOpacity = 0.22f;
inline constexpr float kZoneRimOpacity = 0.75f;
inline constexpr float kZoneRimStart = 0.9f;
// A capture flag (v0.33) is a power-up pickup in its team's colour
// (render_state.hpp kFlagShape); its carrier wears a small copy above the head.
inline constexpr float kFlagCarrySize = 0.32f;  // units
inline constexpr float kFlagCarryLift = 0.35f;  // above the head, units
inline constexpr float kFlagCarrySpinRadPerSec = 2.0f;

// --- cartoon look (§7 "Look", v0.49) -------------------------------------------------
// Ink outlines are inverted hulls: the mesh again, pushed out along its normals
// (boxes: out along each axis, so the corners stay closed) by this many world
// units, back faces only, in ArenaLook::ink.
inline constexpr float kOutlineMonkey = 0.045f;
inline constexpr float kOutlineBanana = 0.022f;
inline constexpr float kOutlineWall = 0.08f;
inline constexpr float kOutlinePowerUp = 0.035f;
// v0.54: the skinned character's hull is drawn with this depth bias (constant
// and slope-scaled, in depth-buffer units, away from the camera): where it
// grazes the character's own surface it stays hidden; silhouettes keep it.
inline constexpr float kOutlineSkinnedDepthBias = 1.0f;
// Two-band cel shading: n·l above this edge is lit, below it the shade band
// (kShadeBand × ArenaLook::shadeTint). The edge is anti-aliased with fwidth.
inline constexpr float kCelBandEdge = 0.12f;
inline constexpr float kShadeBand = 0.62f;
// The floor patterns' scales live in each ArenaLook (palette.hpp). Ripples,
// cloud arcs and wave squiggles sway with presentation time; mud bubbles
// swell and settle.
inline constexpr float kWaterRippleSpeed = 0.5f;  // radians of sway per second
inline constexpr float kMudBubbleHz = 0.35f;

// --- the ground program (§7 "Look", v0.54) ---------------------------------------
// The arena plane is one quad reaching this far beyond the arena box on
// every side, so the backdrop fills the view (the camera never sees past it).
inline constexpr float kGroundMargin = 40.0f;
// A wall's soft shadow on the ground: its footprint grown by the spread,
// moved this far toward the camera (sim -y) and blurred over the softness.
inline constexpr float kWallShadowOffset = 0.3f;
inline constexpr float kWallShadowSpread = 0.2f;
inline constexpr float kWallShadowSoftness = 0.3f;
// A wall block's corner radius is at most this share of its shorter half
// side (ArenaLook::wallRadius), so a thin wall stays a slab, not a tube.
inline constexpr float kWallRadiusShare = 0.45f;
// Grid-space modes only (§5.12): a faint line on every cell edge.
inline constexpr float kGridLineWidth = 0.06f;
inline constexpr float kGridAlpha = 0.22f;
// Where boxes of one kind meet at a concave corner (the land round a notch,
// an L of mud), the ground rounds it this much: a fillet takes area from the
// other side (a pit, the floor), so it stays small whatever the look's
// cornerRadius (which rounds convex corners only).
inline constexpr float kGroundFillet = 0.3f;
// The ground's distance fields (ground.hpp GroundField): sampled this many
// times a unit over the arena and this far around it (deeper than any cliff,
// shallows band or ink line reaches), and clamped to this range (half
// floats keep 1/1024 of a unit near the edges that matter).
inline constexpr float kGroundFieldTexelsPerUnit = 12.0f;
inline constexpr float kGroundFieldMargin = 2.5f;
inline constexpr float kGroundFieldRange = 8.0f;
// The arena edge line is at least this wide (a look without ground ink
// still marks where bananas bounce).
inline constexpr float kEdgeLineMinWidth = 0.1f;

// --- locomotion (§7) -----------------------------------------------------------------
// A moving monkey plays the rig's `run` clip (limb cycle, bob and forward
// lean) only when this is on. Off, it holds the idle clip and glides while
// still turning to its heading; dash, charge, throw, hit and eliminated play
// as before. The clip stays in the rig (tools/assets/gen_test_rig.py).
inline constexpr bool kMonkeyRunCycle = false;

// --- facing (§7) ---------------------------------------------------------------------
// Body yaw follows the move direction at most this fast (720°/s: a half turn
// in 0.25 s); aim yaw follows the aim directly.
inline constexpr float kBodyTurnRate = 4.0f * kPi;

// --- charged throw (v0.48) -----------------------------------------------------------
// While a throw charges, the aim bar grows from its resting length to this
// and warms toward gold (v0.53: the monkey winds up and, at full charge,
// strains; animation.hpp holds those numbers).
inline constexpr float kChargeAimLength = 1.6f;

// --- squash and stretch ------------------------------------------------------------
inline constexpr float kDashStretch = 0.18f;  // forward stretch while dashing
inline constexpr float kHitSquash = 0.2f;     // vertical squash at the start of hit-stun

// --- pit falls (v0.57) ------------------------------------------------------------------
// A falling monkey (Tag::Falling) is drawn from its fall progress t in [0, 1]
// (PlayerDef::fallTicks, interpolated like the position): over the first
// kFallTeeterFraction it hops up kFallHopHeight (a startled teeter at the
// edge), then drops kFallDepth below the floor as u², u the rest of the fall;
// the floor's depth hides what is under it (the sim starts a fall only with
// the whole body over the pit, so it sinks where it stands). It shrinks to
// kFallEndScale, tips its nose down by kFallTilt
// radians, spins kFallSpinTurns whole turns (as t²) and darkens by
// kFallDarken; its shadow shrinks away with it, and a banana in its hand
// goes down with it.
inline constexpr float kFallTeeterFraction = 0.18f;
inline constexpr float kFallHopHeight = 0.22f;
inline constexpr float kFallDepth = 2.2f;
inline constexpr float kFallEndScale = 0.5f;
inline constexpr float kFallTilt = 0.7f;
inline constexpr float kFallSpinTurns = 0.75f;
inline constexpr float kFallDarken = 0.55f;
inline constexpr float kPitFallSeconds = 0.45f;  // the PitFall ring
// A banana that comes down over a pit (v0.57, Sinking) drops from hand height
// to this far below the floor as t² over BoomerangDef::pitSinkTicks; the
// splash ring where it drops in is this big.
inline constexpr float kBananaSinkDepth = 0.8f;
inline constexpr float kBananaSplashSize = 0.45f;
// The banana economy (v0.57): a banana grown in an empty hand twinkles there
// (PickupSparkle) with a short flash; a dead monkey's banana in flight that
// vanishes with it leaves a small white puff (ThrowPuff) where it was.
inline constexpr float kRegrowSparkleSeconds = 0.5f;
inline constexpr float kRegrowFlashSeconds = 0.15f;
inline constexpr float kBananaVanishSeconds = 0.3f;

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
// M4 effects (v0.19). Dash anticipation: a ring collapsing onto the dasher
// and a flash ahead of it, so a dash whose catch resolves late still reads as
// intentional at the press (§12).
inline constexpr float kDashAnticipationSeconds = 0.12f;
// Trails are sampled from state, not events: a dashing player and a flying
// banana leave one afterimage every interval, each living this long.
inline constexpr float kDashTrailSeconds = 0.18f;
inline constexpr float kDashTrailInterval = 0.025f;
inline constexpr float kBananaTrailSeconds = 0.12f;
inline constexpr float kBananaTrailInterval = 0.03f;
inline constexpr float kPerfectCatchBurstSeconds = 0.45f;  // gold sparks, beside the flash
inline constexpr float kHitSparkSeconds = 0.25f;
inline constexpr float kEliminationPoofSeconds = 0.7f;  // grey smoke puffs drifting up
inline constexpr float kPickupSparkleSeconds = 0.35f;
inline constexpr float kGroundDustSeconds = 0.35f;
inline constexpr float kBounceSparkSeconds = 0.15f;
// Mode effects (v0.23).
inline constexpr float kRespawnBurstSeconds = 0.6f;
inline constexpr float kScorePopSeconds = 0.5f;
inline constexpr float kTimerBannerSeconds = 0.8f;
// Spark and puff counts per burst (each is one billboard draw call).
inline constexpr int kPerfectCatchSparks = 8;
inline constexpr int kHitSparks = 6;
inline constexpr int kEliminationPuffs = 5;
inline constexpr int kPickupTwinkles = 4;
inline constexpr int kDustPuffs = 4;
// Power-ups (v0.20). A power-up pickup is the def's shape in its palette
// colour, hovering higher than a banana with a glowing ground ring; taking it
// bursts a ring and sparks in the taker's colour; a broken shield shatters.
inline constexpr float kPowerUpHeight = 0.45f;
inline constexpr float kPowerUpSize = 0.6f;  // shape size, units (the contact radius is 0.5)
inline constexpr float kPowerUpRingRadius = 0.55f;
inline constexpr float kPowerUpRingPulseHz = 1.2f;
inline constexpr float kPowerUpBurstSeconds = 0.5f;
inline constexpr int kPowerUpSparks = 8;
inline constexpr float kShieldShatterSeconds = 0.4f;
inline constexpr int kShieldShards = 7;
// Abilities (v0.21). A blink bursts at both ends; a slam's ring races out to
// its reach (the effect's size) and shakes the camera; a parry flares at the
// press and clashes in sparks where the banana turned.
inline constexpr float kBlinkFlashSeconds = 0.3f;
inline constexpr float kSlamShockwaveSeconds = 0.45f;
inline constexpr float kShakeOnSlam = 0.25f;
inline constexpr float kParryGuardSeconds = 0.2f;
// v0.35: a call's shout over the caller's head, and its height above the body.
inline constexpr float kCallShoutSeconds = 0.45f;
inline constexpr float kCallShoutHeight = 1.9f;
inline constexpr float kParrySparkSeconds = 0.35f;
inline constexpr int kParrySparks = 8;
// Banners in the HUD pop in: scale 1 + this at the start, settling to 1 over
// the first quarter of the banner's life.
inline constexpr float kBannerPop = 0.35f;

// --- the renderer's look (§7, renderer.cpp) -------------------------------------------
// Direction toward the light: from above, a little left and toward the camera,
// so capsule fronts are lit and their right sides fall into the half band.
inline constexpr Vec3 kToLight{-0.42f, 1.0f, 0.55f};
// Colours the renderer draws with besides the palette and the arena look.
inline constexpr Color kBananaColor{1.0f, 0.86f, 0.16f};   // a banana in hand or in flight
inline constexpr Color kPickupColor{0.98f, 0.80f, 0.12f};  // a banana on the ground
inline constexpr Color kEliminatedColor{0.42f, 0.42f, 0.44f};
inline constexpr float kEliminatedFade = 0.8f;  // how far an eliminated monkey greys out
// v0.57: a monkey going down a pit darkens toward this (by kFallDarken × fall).
inline constexpr Color kFallShade{0.05f, 0.07f, 0.12f};
inline constexpr Color kEffectGold{1.0f, 0.82f, 0.18f};  // catches, scores, the flag
inline constexpr Color kEffectWhite{1.0f, 1.0f, 1.0f};   // neutral effects, tints toward white
inline constexpr Color kHitRed{1.0f, 0.16f, 0.10f};
inline constexpr Color kShieldBlue{0.45f, 0.85f, 1.0f};
inline constexpr Color kSparkColor{1.0f, 0.62f, 0.25f};   // hit sparks
inline constexpr Color kSmokeColor{0.72f, 0.72f, 0.74f};  // elimination poof
inline constexpr Color kDustColor{0.62f, 0.52f, 0.38f};   // ground dust
// Decal layers above the floor (units), each over the one before, plus the
// decal pipelines' depth bias (constant and slope-scaled, toward the camera).
inline constexpr float kZoneLift = 0.006f;    // v0.33: zones lie under the shadows
inline constexpr float kShadowLift = 0.012f;  // blob shadows
inline constexpr float kDecalLift = 0.022f;   // rings and trails sit above the shadows
inline constexpr float kDecalDepthBias = -1.0f;
inline constexpr float kShadowOpacity = 0.55f;  // multiplied by RenderShadow::opacity
// The aim indicator: a bar this long beyond the capsule surface, at this
// share of the capsule height, the player's colour whitened by kAimWhiten;
// it glows kAimGlow, plus kAimChargeGlow × the throw's charge.
inline constexpr float kAimLength = 0.5f;
inline constexpr float kAimThickness = 0.1f;
inline constexpr float kAimHeightFraction = 0.55f;
inline constexpr float kAimWhiten = 0.45f;
inline constexpr float kAimGlow = 0.15f;
inline constexpr float kAimChargeGlow = 0.35f;
// A banana in hand: this scale, and on a capsule (no skinned hand) this far
// along the aim and to the side, × the player radius.
inline constexpr float kHeldBananaScale = 0.75f;
inline constexpr float kHeldBananaForward = 0.55f;
inline constexpr float kHeldBananaSide = 0.85f;
// Bananas glow a little: emissive in hand, in flight and on the ground.
inline constexpr float kHeldBananaGlow = 0.1f;
inline constexpr float kFlyingBananaGlow = 0.15f;
inline constexpr float kPickupBananaGlow = 0.1f;
inline constexpr int kPickupStackShown = 3;  // bananas drawn on one stacked pickup, at most
inline constexpr float kOwnerTint = 0.3f;    // flying bananas lean toward the owner's colour
inline constexpr float kFlagCarryGlow = 0.3f;
// An invulnerable monkey blinks at this rate, flashing this bright; a
// dashing one glows this much.
inline constexpr float kInvulnerableBlinkHz = 8.0f;
inline constexpr float kInvulnerableFlash = 0.35f;
inline constexpr float kDashEmissive = 0.25f;
inline constexpr float kMaxAnimStepSeconds = 0.25f;  // a stalled frame must not skip whole clips
// Power-up pickup shapes (§5.8), × kPowerUpSize: the glow pulses about its
// base by its swing at kPowerUpRingPulseHz; the cube is tipped toward the
// camera; the coin is a capsule this thick stood on its edge.
inline constexpr float kPowerUpGlow = 0.25f;
inline constexpr float kPowerUpGlowSwing = 0.15f;
inline constexpr float kPowerUpCubeScale = 0.75f;
inline constexpr float kPowerUpCubeTilt = 0.35f;  // radians
inline constexpr float kPowerUpDiamondScale = 0.62f;
inline constexpr float kPowerUpCoinThickness = 0.2f;
// The pulsing ring under a power-up pickup: its opacity from min to max.
inline constexpr float kPowerUpRingAlphaMin = 0.45f;
inline constexpr float kPowerUpRingAlphaMax = 0.9f;
inline constexpr float kPowerUpRingInner = 0.75f;

// How the renderer draws one disc of an effect: its radius goes from `from`
// to `to` over the effect's life (units, or × the player radius where the
// constant says so), its opacity starts at `alpha` and fades out with the
// effect, and `inner` shapes the decal (a ring's inner edge, a burst's soft
// core, as a fraction of the radius).
struct EffectDisc {
  float from{0.0f};
  float to{0.0f};
  float alpha{0.0f};
  float inner{0.0f};
};
// State-driven rings round a monkey's feet (× radius; from = to, no fade).
inline constexpr EffectDisc kPerfectDashRing{1.9f, 1.9f, 0.95f, 0.72f};  // the catch window
inline constexpr EffectDisc kShieldRing{1.5f, 1.5f, 0.7f, 0.8f};
// A dashing monkey's stretched trail: this far behind it, this long and wide
// (× radius), its colour whitened by kDashStreakWhiten.
inline constexpr float kDashStreakBehind = 1.4f;
inline constexpr float kDashStreakAlong = 2.2f;
inline constexpr float kDashStreakAcross = 0.85f;
inline constexpr float kDashStreakWhiten = 0.4f;
inline constexpr float kDashStreakAlpha = 0.5f;
inline constexpr float kDashStreakInner = 0.2f;
// Event effects, floor rings (alpha-blended).
inline constexpr EffectDisc kDashRingDisc{0.9f, 2.4f, 0.8f, 0.78f};         // × radius
inline constexpr EffectDisc kPerfectCatchRing{1.0f, 4.0f, 1.0f, 0.82f};     // × radius
inline constexpr EffectDisc kEliminationRing{1.0f, 6.0f, 0.9f, 0.85f};      // × radius
inline constexpr EffectDisc kDashAnticipationRing{2.6f, 1.0f, 0.9f, 0.8f};  // × radius, eased
inline constexpr EffectDisc kPitFallRing{2.4f, 0.4f, 0.85f, 0.8f};          // × the faller, eased
inline constexpr EffectDisc kPitFallInnerRing{1.6f, 0.2f, 0.6f, 0.75f};     // × the faller, eased
inline constexpr float kPitFallInnerLead =
    1.3f;  // the inner ring runs this much faster, a beat behind
inline constexpr EffectDisc kPowerUpBurstRing{kPowerUpRingRadius, 2.2f, 0.9f, 0.8f};  // eased
inline constexpr EffectDisc kShieldShatterRing{1.5f, 2.8f, 0.9f, 0.85f};  // × radius, eased
inline constexpr EffectDisc kRespawnRing{3.0f, 1.0f, 0.9f, 0.8f};         // × radius, eased
inline constexpr EffectDisc kScorePopRing{1.2f, 3.2f, 0.9f, 0.85f};       // × radius, eased
inline constexpr EffectDisc kSlamRing{0.3f, 1.0f, 0.9f, 0.9f};            // × the reach, eased
inline constexpr EffectDisc kParryGuardRing{1.0f, 1.8f, 0.8f, 0.85f};     // × radius, eased
// Event effects, billboards facing the camera (alpha-blended).
inline constexpr EffectDisc kEliminationFlash{0.8f, 2.6f, 0.85f, 0.35f};
inline constexpr EffectDisc kHitFlashDisc{0.6f, 1.4f, 0.9f, 0.3f};
inline constexpr EffectDisc kDashTrailDisc{1.0f, 0.4f, 0.45f, 0.3f};  // × radius
inline constexpr EffectDisc kRespawnGlow{1.6f, 0.6f, 0.6f, 0.35f};    // × radius
inline constexpr float kRespawnGlowRiseFrom = 0.2f;  // the glow's height over its life, units
inline constexpr float kRespawnGlowRiseTo = 1.6f;
// Event effects, additive billboards.
inline constexpr EffectDisc kCallShoutRing{0.25f, 0.7f, 1.0f, 0.7f};  // eased
inline constexpr float kCallShoutRingPerSize = 0.3f;  // the ring's end grows with the call's range
inline constexpr EffectDisc kCallShoutCore{0.1f, 0.35f, 1.0f, 0.2f};  // eased
inline constexpr EffectDisc kPerfectCatchGlow{0.6f, 1.8f, 1.0f, 0.25f};
inline constexpr EffectDisc kPerfectCatchCore{0.3f, 0.8f, 1.0f, 0.1f};
inline constexpr EffectDisc kAutoCatchDisc{0.4f, 0.9f, 0.7f, 0.2f};
inline constexpr EffectDisc kThrowPuffDisc{0.2f, 0.55f, 0.55f, 0.1f};
inline constexpr EffectDisc kDashAnticipationFlash{0.5f, 0.2f, 0.8f, 0.1f};
// The flash is this far ahead of the dasher (× radius), at this share of
// kEffectBodyHeight.
inline constexpr float kDashAnticipationAhead = 1.3f;
inline constexpr float kDashAnticipationFlashHeight = 0.6f;
inline constexpr EffectDisc kBananaTrailDisc{0.16f, 0.04f, 0.6f, 0.1f};
inline constexpr EffectDisc kBounceSparkDisc{0.15f, 0.45f, 0.8f, 0.1f};
inline constexpr EffectDisc kShieldShatterFlash{0.7f, 1.5f, 0.8f, 0.3f};
inline constexpr EffectDisc kBlinkFlashDisc{1.1f, 0.2f, 0.9f, 0.3f};  // eased
inline constexpr EffectDisc kParrySparkFlash{0.5f, 1.2f, 0.9f, 0.2f};
// An effect tinted with its player's colour is whitened this much.
inline constexpr float kDashAnticipationWhiten = 0.5f;
inline constexpr float kPitFallWhiten = 0.55f;
inline constexpr float kDashTrailWhiten = 0.3f;
inline constexpr float kRespawnWhiten = 0.35f;
inline constexpr float kSlamWhiten = 0.4f;
inline constexpr float kParryGuardWhiten = 0.6f;
inline constexpr float kBlinkWhiten = 0.5f;
inline constexpr float kCallShoutWhiten = 0.3f;
inline constexpr float kPowerUpSparkWhiten = 0.35f;
inline constexpr float kPickupTwinkleWhiten = 0.4f;  // gold toward white
inline constexpr float kShieldShardWhiten = 0.4f;    // shield blue toward white
inline constexpr float kParrySparkWhiten = 0.6f;     // gold toward white

// A burst of billboards flying out from an effect (the counts are above):
// how far they travel, their radius at the start and the share of it lost
// by the end (negative: they grow), how high they rise, the cone they fill
// round the effect's direction (radians; 0 or no direction: the full
// circle) and their opacity.
struct SparkLook {
  float reach{0.0f};
  float size{0.0f};
  float shrink{0.0f};
  float rise{0.0f};
  float spread{0.0f};
  float alpha{0.0f};
};
inline constexpr SparkLook kEliminationPuffLook{0.9f, 0.3f, -1.0f, 0.8f, 0.0f, 0.6f};
inline constexpr SparkLook kDustPuffLook{0.6f, 0.16f, -1.2f, 0.15f, 0.0f, 0.5f};
inline constexpr SparkLook kPerfectCatchSparkLook{1.8f, 0.14f, 0.5f, 0.3f, 0.0f, 1.0f};
inline constexpr SparkLook kHitSparkLook{1.2f, 0.12f, 0.6f, 0.2f, 1.6f, 1.0f};
inline constexpr SparkLook kPickupTwinkleLook{0.35f, 0.12f, 0.7f, 0.6f, 0.0f, 1.0f};
inline constexpr SparkLook kPowerUpSparkLook{1.1f, 0.14f, 0.6f, 0.7f, 0.0f, 1.0f};
inline constexpr SparkLook kShieldShardLook{1.0f, 0.12f, 0.5f, 0.3f, 0.0f, 1.0f};
inline constexpr SparkLook kParrySparkLook{1.3f, 0.13f, 0.5f, 0.2f, 1.2f, 1.0f};
// Every spark's soft core; every other spark travels this share of the
// reach (so a burst is not a perfect circle); a full-circle burst starts
// at this many radians × (slot + 2), so overlapping bursts differ.
inline constexpr float kSparkInner = 0.15f;
inline constexpr float kSparkAltReach = 0.75f;
inline constexpr float kSparkSlotTurn = 0.61f;

// --- audio (v0.19) -----------------------------------------------------------------
inline constexpr int kAudioSampleRate = 48000;  // the stream's rate; SDL converts
inline constexpr int kMixerVoices = 16;
// The device queue is topped up to this much audio each frame: the sound
// latency, and the stall a frame can survive without a gap.
inline constexpr float kAudioQueueSeconds = 0.05f;
inline constexpr float kDefaultMasterVolume = 0.8f;  // --volume 80
// Positioned sounds pan by world x over the arena half width, scaled by this
// (a light pan: nothing fully leaves one ear).
inline constexpr float kPanWidth = 0.6f;
// The same sound cued twice in one frame plays once, louder by this per
// extra cue, up to kSoundStackMax × its gain (identical clips started on the
// same sample would only double in amplitude and clip).
inline constexpr float kSoundStackGain = 0.25f;
inline constexpr float kSoundStackMax = 1.75f;

// --- screen shake ------------------------------------------------------------------
inline constexpr float kShakeOnHit = 0.12f;  // world units of amplitude added
inline constexpr float kShakeOnElimination = 0.3f;
inline constexpr float kShakeOnPerfectCatch = 0.08f;
inline constexpr float kShakeMax = 0.5f;
inline constexpr float kShakeTau = 0.15f;  // exponential decay time constant
inline constexpr float kShakeHz = 23.0f;
// The shake stops being drawn once it moves the arena centre by less than this
// many framebuffer pixels (visibleShakeOffset, camera.hpp): below a pixel a
// 23 Hz offset is no longer seen as motion, only as every hard edge (ink
// lines, the rim, grid lines) toggling between two pixels, which reads as
// flicker for the second the exponential tail would otherwise last.
inline constexpr float kShakeMinPixels = 1.0f;

}  // namespace kfm::presentation
