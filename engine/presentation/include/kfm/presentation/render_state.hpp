// RenderState (architecture §7): everything the renderer and HUD draw in one
// frame, derived from two sim states, an interpolation alpha, smoothing
// offsets and the event buffer (builder.hpp). Plain floats and enums in
// fixed-capacity arrays: drawing it never touches WorldState, and building it
// never allocates.
//
// Positions are world space (world_basis.hpp): (x, height, z) with z = -sim y.
// Yaws are radians about world +y in the sim's angle convention: rotateY(yaw)
// turns a model's +x (forward) onto the heading. A renderer iterates
// players[0..playerCount), bananas[0..bananaCount), pickups[0..pickupCount),
// shadows[0..shadowCount) and effects[0..effectCount), and reads hud; the
// app plays sounds[0..soundCount) (the cues started this frame).
#pragma once

#include "kfm/presentation/math.hpp"
#include "kfm/presentation/sound.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/content/arena.hpp"
#include "kfm/sim/content/mode.hpp"
#include "kfm/sim/world_state.hpp"

#include <array>
#include <cstdint>

namespace kfm::presentation {

inline constexpr uint16_t kMaxRenderPlayers = sim::kMaxPlayers;
inline constexpr uint16_t kMaxRenderBananas = sim::kMaxBoomerangs;
inline constexpr uint16_t kMaxRenderPickups = sim::kMaxPickups;
inline constexpr uint16_t kMaxRenderShadows =
    kMaxRenderPlayers + kMaxRenderBananas + kMaxRenderPickups;
inline constexpr uint16_t kMaxRenderEffects = 128;
inline constexpr uint16_t kMaxRenderSounds = 32;

inline constexpr int8_t kNoSlot = -1;

// RenderPlayer::flags bits.
enum class PlayerFlag : uint16_t {
  Dashing = 1u << 0,
  PerfectDash = 1u << 1,  // the perfect-dash (catch) window is open
  HitStun = 1u << 2,
  Eliminated = 1u << 3,
  Invulnerable = 1u << 4,
  Shielded = 1u << 5,
  Throwing = 1u << 6,  // the Throw slot is in Startup/Active/Recovery
  HasAmmo = 1u << 7,
  Carrying = 1u << 8,  // v0.33: holds a capture flag (Tag::Carrying)
  Charging = 1u << 9,  // v0.48: the Throw slot's wind-up is a charging one (still in Startup)
  Falling = 1u << 10,  // v0.57: going down a pit (Tag::Falling); RenderPlayer::fall
};
[[nodiscard]] constexpr uint16_t flagBit(PlayerFlag f) {
  return static_cast<uint16_t>(f);
}

// A power-up's placeholder shape (sim::PowerUpVisual::shape, §5.8).
enum class PowerUpShapeView : uint8_t { Orb = 0, Cube, Diamond, Coin };
// A live power-up on a player, for the HUD timers (§5.8).
struct RenderPowerUp {
  uint8_t powerUp{0};     // index into ContentDB::powerUps
  uint8_t colorIndex{0};  // palette.hpp
  PowerUpShapeView shape{PowerUpShapeView::Orb};
  float secondsLeft{0.0f};
  float fraction{0.0f};  // remaining / duration, 1 = just picked up
};

struct RenderPlayer {
  uint8_t slot{0};
  uint8_t colorIndex{0};  // = slot; palette.hpp
  bool visible{true};
  uint16_t flags{0};             // PlayerFlag bits
  Vec3 position{};               // feet, world space, y = cosmetic height (hop, hit lift)
  Vec3 ground{};                 // the sim position on the ground (where the shadow is)
  float height{0.0f};            // = position.y
  float bodyYaw{0.0f};           // follows the move direction, turn-rate limited
  float aimYaw{0.0f};            // follows the aim directly
  Vec3 scale{1.0f, 1.0f, 1.0f};  // model space: x forward (along bodyYaw), y up, z side
  float flash{0.0f};             // 0..1 white flash (hit, catch), fading
  uint8_t ammo{0};
  uint8_t ammoMax{0};
  uint8_t hp{0};
  uint8_t maxHp{0};
  float dashCooldown{0.0f};  // 0 = ready, 1 = just used
  // v0.48, while Charging: the charge so far (0 = a tap, 1 = full). v0.53:
  // it drives the charge pose and, at 1, the strain (animation.hpp).
  float throwCharge{0.0f};
  // v0.57, while Falling: the fall so far (0 = it just went in, 1 = out),
  // interpolated like the position, and the body's pitch about the model's
  // side axis (z; negative tips the nose down). position, scale and bodyYaw
  // already carry the rest of the fall (builder.hpp).
  float fall{0.0f};
  float tilt{0.0f};
  std::array<RenderPowerUp, sim::kMaxModifiers> powerUps{};  // in slot (pickup) order
  uint8_t powerUpCount{0};

  [[nodiscard]] constexpr bool has(PlayerFlag f) const { return (flags & flagBit(f)) != 0; }
};

// v0.57: Sinking, a banana that came down over a pit dropping into the hole.
enum class BananaPhase : uint8_t { Outbound = 0, Returning, Grounded, Sinking };

struct RenderBanana {
  Vec3 position{};  // world space, y = hand height + arc
  Vec3 ground{};    // the sim position (shadow, hitbox)
  float height{0.0f};
  float headingYaw{0.0f};  // flight direction
  float spin{0.0f};        // cosmetic spin about world +y, radians
  BananaPhase phase{BananaPhase::Outbound};
  int8_t ownerSlot{kNoSlot};
};

// v0.33: Flag, a capture team's flag, drawn like a power-up in its team's
// colour with this shape.
enum class PickupKindView : uint8_t { Banana = 0, PowerUp, Flag };
inline constexpr PowerUpShapeView kFlagShape = PowerUpShapeView::Diamond;

struct RenderPickup {
  Vec3 position{};  // world space, hovering slightly
  Vec3 ground{};
  float spin{0.0f};
  float radius{0.0f};  // sim contact radius
  uint8_t count{1};    // v0.57, Banana: bananas stacked here (drawn as up to three)
  PickupKindView kind{PickupKindView::Banana};
  // PowerUp: the def's look (palette.hpp index, shape) and its index.
  // Flag (v0.33): the team's colour (team t is palette colour t), shape
  // kFlagShape, powerUp 0.
  uint8_t colorIndex{0};
  PowerUpShapeView shape{PowerUpShapeView::Orb};
  uint8_t powerUp{0};
};

enum class ShadowOwner : uint8_t { Player = 0, Banana, Pickup };

// A blob shadow at the true sim position: it marks the hitbox (§7). Every
// player, banana and pickup casts one; opacity falls with height.
struct RenderShadow {
  Vec3 ground{};  // y = 0
  float radius{0.0f};
  float opacity{1.0f};
  ShadowOwner owner{ShadowOwner::Player};
};

enum class EffectKind : uint8_t {
  DashRing = 0,       // anticipation ring at the dash start
  PerfectCatchFlash,  // dash-catch (CatchHow::Perfect)
  AutoCatchFlash,     // a returning banana caught by its owner
  HitFlash,
  ThrowPuff,
  EliminationBurst,
  RoundStartBanner,  // HUD: "Round N"
  RoundEndBanner,    // HUD: winner of the round (slot) or a draw (kNoSlot)
  MatchEndBanner,    // HUD: match winner (slot)
  // v0.19 (M4 audio/VFX):
  DashAnticipation,   // ring collapsing onto the dasher + flash ahead (direction)
  DashTrail,          // afterimage of a dashing player (sampled while Dashing)
  BananaTrail,        // short trail behind a flying banana (slot = owner)
  PerfectCatchBurst,  // gold sparks radiating from a dash-catch
  HitSpark,           // sparks at the victim (direction: away from the attacker, if known)
  EliminationPoof,    // grey smoke puffs drifting up
  PickupSparkle,      // twinkles where a pickup was taken
  GroundDust,         // dust where a banana landed
  BounceSpark,        // a banana bounced off a wall
  // v0.23 (modes):
  RespawnBurst,  // a column of light where a monkey respawns (slot colour)
  ScorePop,      // gold ring at a monkey that scored an elimination (deathmatch)
  TimerBanner,   // HUD: the mode clock pops at 10, 3, 2, 1 seconds left
  // v0.20 (power-ups):
  PowerUpBurst,   // ring and sparks where a power-up was taken (slot = taker)
  ShieldShatter,  // a shield absorbed a hit (slot = the shielded player)
  // v0.21 (abilities):
  BlinkFlash,     // a burst where a blink left and where it landed (slot colour)
  SlamShockwave,  // a ring racing out to the slam's reach (size = reach, units)
  ParryGuard,     // the parry stance flaring up at the press (slot colour)
  ParrySpark,     // a bright clash where a banana was parried (direction: its new heading)
  // v0.35 (the squad contest):
  CallShout,  // a call: rings popping over the caller's head (slot colour; size = symbol / 255)
  // v0.57 (pit falls):
  PitFall,  // a ring closing in where a monkey went down a pit (slot colour)
  // v0.80 (control):
  ZoneCaptured,  // a ring racing out over a zone a team just took (slot = the team, its
                 // palette colour; size = the zone's larger half extent, units)
};

struct RenderEffect {
  EffectKind kind{EffectKind::DashRing};
  int8_t slot{kNoSlot};   // player concerned, if any
  Vec3 position{};        // world space (unused by banners)
  Vec2f direction{};      // sim-plane direction where it matters (dash, throw); else zero
  float remaining{0.0f};  // seconds
  float duration{0.0f};   // seconds; progress = 1 - remaining / duration
  float size{0.0f};       // world size where it matters (SlamShockwave: the reach); else 0

  [[nodiscard]] constexpr float progress() const {
    return duration > 0.0f ? clamp01(1.0f - (remaining / duration)) : 1.0f;
  }
};

// Arena statics (v0.22): the interior walls, then the hazards, in content
// order, then the pit rims. Boxes in world space: `center` is the box centre
// on the ground (y = 0), `size` its world extents (x, y = the drawn height,
// z). Built from content, so constant for a match.
enum class ArenaPieceKind : uint8_t {
  Wall = 0,  // solid block, drawn kArenaWallHeight tall
  Pit,       // near-black floor slab
  PitRim,    // a strip inside a pit along an edge it shares with the floor
  Slow,      // mud patch
};

struct RenderArenaPiece {
  ArenaPieceKind kind{ArenaPieceKind::Wall};
  Vec3 center{};
  Vec3 size{};
};

// An objective zone (v0.33): a translucent floor rectangle in world space,
// tinted with its team's colour when it is a team's (capture: zone t is team
// t's; control, v0.80: its owner's, from modeData), neutral otherwise (hill,
// an unowned control zone). Team t is palette colour t.
struct RenderZone {
  Vec3 center{};  // on the ground, y = 0
  Vec3 size{};    // world extents (x, 0, z)
  int8_t team{kNoSlot};
  // v0.80, control: the team taking the zone (kNoSlot: no capture under way)
  // and its progress toward ModeDef::captureTicks, in [0, 1).
  int8_t challenger{kNoSlot};
  float capture{0.0f};
};

// Rim strips are capped; a pit layout needing more draws without the excess.
inline constexpr uint16_t kMaxRenderPitRims = 64;
inline constexpr uint16_t kMaxRenderArenaPieces =
    sim::kMaxArenaWalls + sim::kMaxArenaHazards + kMaxRenderPitRims;

enum class HudPhase : uint8_t { Lobby = 0, RoundStarting, Playing, RoundEnded, MatchEnded };

struct RenderHud {
  HudPhase phase{HudPhase::Lobby};
  uint32_t ticksInPhase{0};
  float countdownSeconds{0.0f};  // RoundStarting: until play starts; else 0
  float roundEndSeconds{0.0f};   // RoundEnded: until the next round; else 0
  uint8_t round{0};
  uint8_t roundsToWin{0};
  uint16_t presentMask{0};                        // bit i: slot i is in the match
  std::array<int8_t, sim::kMaxPlayers> scores{};  // signed since v0.57
  int8_t lastRoundWinner{kNoSlot};  // kNoSlot before the first round end or after a draw
  int8_t matchWinner{kNoSlot};      // set in MatchEnded
  // v0.23 (modes): what the scoreboard shows. scores are round wins in
  // rounds/survival and eliminations in deathmatch.
  sim::ModeKind mode{sim::ModeKind::Rounds};
  uint8_t scoreToWin{0};     // deathmatch
  bool hasClock{false};      // the mode has a time limit
  float clockSeconds{0.0f};  // time left on it (full during the countdown, 0 after)
  uint8_t aliveCount{0};     // monkeys still in play
  std::array<uint8_t, sim::kMaxPlayers> deaths{};
  std::array<float, sim::kMaxPlayers> respawnSeconds{};  // > 0: dead, back in this long
  // v0.32 (teams): the mode's team count (0 = free-for-all), every slot's
  // team (MatchConfig::teams) and every team's score (the sum of its slots').
  uint8_t teamCount{0};
  std::array<uint8_t, sim::kMaxPlayers> teams{};
  std::array<int16_t, sim::kMaxPlayers> teamScores{};
};

struct RenderState {
  std::array<RenderPlayer, kMaxRenderPlayers> players{};
  uint16_t playerCount{0};
  std::array<RenderBanana, kMaxRenderBananas> bananas{};
  uint16_t bananaCount{0};
  std::array<RenderPickup, kMaxRenderPickups> pickups{};
  uint16_t pickupCount{0};
  std::array<RenderShadow, kMaxRenderShadows> shadows{};
  uint16_t shadowCount{0};
  std::array<RenderEffect, kMaxRenderEffects> effects{};
  uint16_t effectCount{0};

  float shakeAmplitude{0.0f};  // world units
  Vec3 shakeOffset{};          // add to the camera eye and target this frame
  Vec2f arenaHalfExtents{};    // sim units, for the floor and wall rim
  std::array<RenderArenaPiece, kMaxRenderArenaPieces> arenaPieces{};  // interior walls, hazards
  uint16_t arenaPieceCount{0};
  std::array<RenderZone, sim::kMaxArenaZones> zones{};  // v0.33, in content order
  uint8_t zoneCount{0};
  // v0.54: the cell size whose edges the floor draws (the arena's cells under
  // a grid-space mode, §5.12); 0 draws no grid.
  float floorGridCell{0.0f};
  float playerRadius{0.0f};
  float playerHeight{0.0f};
  float bananaRadius{0.0f};
  float time{0.0f};  // seconds of presentation time (accumulated dt)
  RenderHud hud{};
  // Sounds started this frame (event_fx.hpp); same-id cues are merged.
  std::array<SoundCue, kMaxRenderSounds> sounds{};
  uint16_t soundCount{0};
};

}  // namespace kfm::presentation
