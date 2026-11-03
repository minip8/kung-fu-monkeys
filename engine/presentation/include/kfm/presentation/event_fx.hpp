// Event -> effect and event -> sound mapping (architecture §5.9, §7
// "Effects", "Audio"). buildRenderState() calls applyEventFx() once per new
// SimEvent: it starts the visual effects, flashes and shake, and cues the
// sound, so pictures and sounds come from the same event stream.
//
// Rollback (§5.9): the builder only ever sees each event once. Locally that
// is the per-tick sink; online it is EventJournal::drain(), which drops
// events of an abandoned timeline that have not played and never hands out a
// re-simulated duplicate. An effect or a sound is therefore never
// re-triggered by re-simulation, and a retracted event that had not been
// drained spawns nothing. One that had already played stays played.
#pragma once

#include "kfm/presentation/builder.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/presentation/sound.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/world_state.hpp"

#include <optional>

namespace kfm::presentation {

// The sound an event plays (gain 1, pan 0: the builder pans positioned ones),
// or nothing. The table:
//   PlayerDashed -> DashSwish          BoomerangThrown -> ThrowWhoosh
//   BoomerangCaught Perfect -> CatchPerfect, Auto -> CatchAuto
//   PlayerHit, PlayerDashHit -> Hit    PlayerEliminated -> Elimination
//   PickupTaken -> Pickup              BoomerangGrounded -> BananaGround
//   BoomerangBounced -> WallBounce     RoundEnded -> RoundEnd
//   MatchEnded -> MatchEnd
// CountIn and RoundGo follow the HUD countdown (builder), UiClick the app.
[[nodiscard]] std::optional<SoundCue> soundForEvent(const sim::SimEvent& e);

// Light stereo position of a sim-plane x: kPanWidth × x / arena half width.
[[nodiscard]] float soundPan(float simX, float arenaHalfWidth);

// Adds a cue to out.sounds, merging a repeat of the same id this frame
// (kSoundStackGain, kSoundStackMax). Drops it when the list is full.
void pushSound(RenderState& out, const SoundCue& cue);

// Adds an effect; when all kMaxRenderEffects are live, replaces the one
// nearest expiry.
void addEffect(PresentationState& ps, const RenderEffect& e);

// Scale of a HUD banner (RoundStartBanner, RoundEndBanner, MatchEndBanner):
// 1 + kBannerPop when its effect starts, easing to 1 over the first quarter
// of its life; 1 when no such effect is live.
[[nodiscard]] float bannerScale(const RenderState& state, EffectKind banner);

// Effects, flashes, shake and the sound of one event.
void applyEventFx(PresentationState& ps, const sim::WorldState& prev, const sim::WorldState& curr,
                  const sim::SimEvent& e, const sim::MatchContent& content, RenderState& out);

}  // namespace kfm::presentation
