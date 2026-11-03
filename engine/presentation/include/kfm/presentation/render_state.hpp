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
// shadows[0..shadowCount) and effects[0..effectCount), and reads hud.
#pragma once

#include "kfm/presentation/math.hpp"
#include "kfm/sim/constants.hpp"

#include <array>
#include <cstdint>

namespace kfm::presentation {

inline constexpr uint16_t kMaxRenderPlayers = sim::kMaxPlayers;
inline constexpr uint16_t kMaxRenderBananas = sim::kMaxBoomerangs;
inline constexpr uint16_t kMaxRenderPickups = sim::kMaxPickups;
inline constexpr uint16_t kMaxRenderShadows =
    kMaxRenderPlayers + kMaxRenderBananas + kMaxRenderPickups;
inline constexpr uint16_t kMaxRenderEffects = 64;

inline constexpr int8_t kNoSlot = -1;

// RenderPlayer::flags bits.
enum class PlayerFlag : uint8_t {
  Dashing = 1u << 0,
  PerfectDash = 1u << 1,  // the perfect-dash (catch) window is open
  HitStun = 1u << 2,
  Eliminated = 1u << 3,
  Invulnerable = 1u << 4,
  Shielded = 1u << 5,
  Throwing = 1u << 6,  // the Throw slot is in Startup/Active/Recovery
  HasAmmo = 1u << 7,
};
[[nodiscard]] constexpr uint16_t flagBit(PlayerFlag f) {
  return static_cast<uint16_t>(f);
}

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

  [[nodiscard]] constexpr bool has(PlayerFlag f) const { return (flags & flagBit(f)) != 0; }
};

enum class BananaPhase : uint8_t { Outbound = 0, Returning, Grounded };

struct RenderBanana {
  Vec3 position{};  // world space, y = hand height + arc
  Vec3 ground{};    // the sim position (shadow, hitbox)
  float height{0.0f};
  float headingYaw{0.0f};  // flight direction
  float spin{0.0f};        // cosmetic spin about world +y, radians
  BananaPhase phase{BananaPhase::Outbound};
  int8_t ownerSlot{kNoSlot};
};

enum class PickupKindView : uint8_t { Banana = 0 };

struct RenderPickup {
  Vec3 position{};  // world space, hovering slightly
  Vec3 ground{};
  float spin{0.0f};
  float radius{0.0f};  // sim contact radius
  PickupKindView kind{PickupKindView::Banana};
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
};

struct RenderEffect {
  EffectKind kind{EffectKind::DashRing};
  int8_t slot{kNoSlot};   // player concerned, if any
  Vec3 position{};        // world space (unused by banners)
  Vec2f direction{};      // sim-plane direction where it matters (dash, throw); else zero
  float remaining{0.0f};  // seconds
  float duration{0.0f};   // seconds; progress = 1 - remaining / duration

  [[nodiscard]] constexpr float progress() const {
    return duration > 0.0f ? clamp01(1.0f - (remaining / duration)) : 1.0f;
  }
};

enum class HudPhase : uint8_t { Lobby = 0, RoundStarting, Playing, RoundEnded, MatchEnded };

struct RenderHud {
  HudPhase phase{HudPhase::Lobby};
  uint32_t ticksInPhase{0};
  float countdownSeconds{0.0f};  // RoundStarting: until play starts; else 0
  float roundEndSeconds{0.0f};   // RoundEnded: until the next round; else 0
  uint8_t round{0};
  uint8_t roundsToWin{0};
  uint16_t presentMask{0};  // bit i: slot i is in the match
  std::array<uint8_t, sim::kMaxPlayers> scores{};
  int8_t lastRoundWinner{kNoSlot};  // kNoSlot before the first round end or after a draw
  int8_t matchWinner{kNoSlot};      // set in MatchEnded
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
  float playerRadius{0.0f};
  float playerHeight{0.0f};
  float bananaRadius{0.0f};
  float time{0.0f};  // seconds of presentation time (accumulated dt)
  RenderHud hud{};
};

// Skeleton shim: the placeholder apps/client/main.cpp still calls this. Delete
// it once the platform work unit replaces that file.
[[nodiscard]] inline int placeholder() {
  return 1;
}

}  // namespace kfm::presentation
