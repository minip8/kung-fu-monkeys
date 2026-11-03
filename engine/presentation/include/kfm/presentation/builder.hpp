// Builds the per-frame RenderState from sim state (architecture §6.8, §6.9, §7).
// Presentation reads sim state and never writes it.
//
// Per frame the client calls
//   buildRenderState(ps, prev, curr, alpha, newEvents, content, dtSeconds, out)
// where
//   - prev, curr: the two newest sim ticks (curr.tick = prev.tick + 1 when
//     interpolating); pass the same state twice to render one tick as is;
//   - alpha in [0, 1]: 0 draws prev, 1 draws curr (the browser renders the
//     newest tick directly with alpha = 1, §6.8);
//   - newEvents: the SimEvents of every tick stepped since the previous call,
//     each exactly once (empty on frames that stepped nothing). Effects and
//     flashes are triggered from them;
//   - dtSeconds: wall time since the previous call; drives smoothing decay,
//     the body turn rate and effect timers.
//
// Interpolation: an entity present in both states with the same EntityId
// (index and generation) is lerped by alpha; one that is new, or that moved
// further than kSnapDistance (respawn, teleport), snaps to curr.
//
// Smoothing (§6.9): after a rollback, onRollback() records
// visualOffset = oldRenderPos - newSimPos per entity (keyed by pool index and
// generation); the offset decays as exp(-dt / tau) every frame and is added to
// the drawn model position. Shadows stay on the true (interpolated) sim
// position, since they mark the hitbox. M2 never rolls back; M3 plugs in here.
#pragma once

#include "kfm/presentation/math.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/presentation/tuning.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/world_state.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace kfm::presentation {

// Per-entity presentation memory, keyed by pool index + generation.
struct EntityTrack {
  bool live{false};
  uint16_t generation{0};
  Vec2f lastDrawn{};  // sim-plane position drawn last frame, offset included
  Vec2f visualOffset{};
  float tau{kSmoothingTau};
};

struct PlayerTrack {
  EntityTrack motion{};
  float bodyYaw{0.0f};
  float bodyTarget{0.0f};
  float flashRemaining{0.0f};
  float flashDuration{0.0f};
};

struct BananaTrack {
  EntityTrack motion{};
  float returnStartDistance{-1.0f};  // owner distance when first seen Returning; < 0 = unset
};

// Everything presentation remembers between frames. Trivially copyable,
// fixed size; value-initialise one per view and keep it for the match.
struct PresentationState {
  std::array<PlayerTrack, sim::kMaxPlayers> players{};
  std::array<BananaTrack, sim::kMaxBoomerangs> bananas{};
  std::array<EntityTrack, sim::kMaxPickups> pickups{};
  std::array<RenderEffect, kMaxRenderEffects> effects{};
  uint16_t effectCount{0};
  float shake{0.0f};  // amplitude, world units
  float time{0.0f};   // accumulated dtSeconds
};

void buildRenderState(PresentationState& ps, const sim::WorldState& prev,
                      const sim::WorldState& curr, float alpha,
                      std::span<const sim::SimEvent> newEvents, const sim::MatchContent& match,
                      float dtSeconds, RenderState& out);

[[nodiscard]] inline RenderState buildRenderState(
    PresentationState& ps, const sim::WorldState& prev, const sim::WorldState& curr, float alpha,
    std::span<const sim::SimEvent> newEvents, const sim::MatchContent& content, float dtSeconds) {
  RenderState out;
  buildRenderState(ps, prev, curr, alpha, newEvents, content, dtSeconds, out);
  return out;
}

// Call after a rollback replaced the timeline, before the next build, with
// the new (prev, curr, alpha) the next frame will draw. For every entity that
// survives with the same id, visualOffset becomes the last drawn position
// minus its new interpolated sim position, so the model keeps its place and
// eases onto the corrected path; errors above kSmoothingSnapDistance snap.
// Players in localPlayers blend with kLocalSmoothingTau instead of
// kSmoothingTau.
void onRollback(PresentationState& ps, const sim::WorldState& newPrev,
                const sim::WorldState& newCurr, float alpha, sim::PlayerMask localPlayers = 0);

// --- cosmetic curves (exposed for tests and tools) ----------------------------

// Dash hop over the Active phase of the Dash slot; 0 outside it.
[[nodiscard]] float dashHopHeight(const sim::Player& p, const sim::ContentDB& content);
// Lift during HitStun; 0 without it.
[[nodiscard]] float hitLiftHeight(const sim::Player& p, const sim::ContentDB& content);
// Total cosmetic height of a player's feet.
[[nodiscard]] float playerHeight(const sim::Player& p, const sim::ContentDB& content);

// The arc part of a banana's height (zero at the throw and back at the owner).
// Outbound: kBananaArcPeak × sin(pi/2 × ticksInPhase / outboundTicks).
// Returning: kBananaArcPeak × sin(pi/2 × clamp(distanceToOwner / returnStartDistance)).
// Grounded: 0.
[[nodiscard]] float bananaArcHeight(const sim::Boomerang& b, float distanceToOwner,
                                    float returnStartDistance, const sim::ContentDB& content);

}  // namespace kfm::presentation
