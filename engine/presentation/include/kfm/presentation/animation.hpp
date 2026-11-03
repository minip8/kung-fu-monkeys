// Skeletal animation for skinned characters (architecture §7). Headless and
// allocation-free per frame:
//
//   clip evaluation   sampleClip() samples every joint track of a clip at a
//                     time (looping or clamped) into a Pose of local
//                     transforms; skinMatrices() composes them down the
//                     hierarchy (global = parent global × local) and returns
//                     global × inverse bind per joint, ready for the shader.
//   crossfades        blendPoses() mixes two poses (lerp translations and
//                     scales, nlerp rotations); a clip change fades over
//                     kAnimCrossfadeSeconds.
//   state machine     AnimState (one per player slot, kept by the caller next
//                     to the renderer, never in RenderState) picks the clip
//                     from RenderPlayer flags and ground motion each frame, by
//                     priority: eliminated > hit > dash > throw > run > idle.
//
// The clip set is fixed (AnimClip); a model provides clips by name
// (bindClips) and a missing one plays the rest pose.
#pragma once

#include "kfm/presentation/math.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/presentation/skinned_model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kfm::presentation {

// --- tuning (cosmetic) ---------------------------------------------------------------
// Clip changes blend the outgoing pose into the new one over this long.
inline constexpr float kAnimCrossfadeSeconds = 0.1f;
// A player whose ground position moves faster than this (world units per
// second) runs; the slowest walk is far above it, a stopped player far below.
inline constexpr float kRunSpeedThreshold = 0.5f;
// The throw clip plays at least this long after the Throwing flag rises, so a
// short Startup/Active/Recovery still reads.
inline constexpr float kThrowAnimSeconds = 0.3f;

// --- poses ---------------------------------------------------------------------------

struct JointPose {
  Vec3 translation{};
  Quat rotation{};
  Vec3 scale{1.0f, 1.0f, 1.0f};
};

// Local joint transforms for one model (joints[0..count)).
struct Pose {
  std::array<JointPose, kMaxSkinJoints> joints{};
  uint32_t count{0};
};

using SkinMatrices = std::array<Mat4, kMaxSkinJoints>;

// Time inside a clip: wrapped into [0, duration) when looping, clamped to
// [0, duration] otherwise. A zero-length clip always samples at 0.
[[nodiscard]] float clipTime(float time, float duration, bool loop);

// The model's rest pose (each joint's node transform).
void restPose(const SkinnedModel& model, Pose& out);

// Samples `clip` at `time` (clipTime applied): linear keys interpolate
// (translations lerp, rotations slerp), step keys hold; joints without a track
// keep their rest value.
void sampleClip(const SkinnedModel& model, const AnimationClip& clip, float time, bool loop,
                Pose& out);

// out = a blended toward b by weightB in [0, 1] (0: a, 1: b). out may alias a or b.
void blendPoses(const Pose& a, const Pose& b, float weightB, Pose& out);

// Global (model-space) transform of each joint for `pose`.
void globalTransforms(const SkinnedModel& model, const Pose& pose, SkinMatrices& out);

// Skin matrix per joint: global × inverse bind. Entries past the joint count
// are the identity. The rest pose yields identities.
void skinMatrices(const SkinnedModel& model, const Pose& pose, SkinMatrices& out);

// --- clips and the state machine ------------------------------------------------------

enum class AnimClip : uint8_t { Idle = 0, Run, Dash, Throw, Hit, Eliminated };
inline constexpr std::size_t kAnimClipCount = 6;

// The glTF animation name of each clip: idle, run, dash, throw, hit, eliminated.
[[nodiscard]] std::string_view animClipName(AnimClip clip);
// Idle and run loop; the others play once and hold their last frame.
[[nodiscard]] constexpr bool animClipLoops(AnimClip clip) {
  return clip == AnimClip::Idle || clip == AnimClip::Run;
}

// Model clip index per AnimClip (-1: the model has no such clip).
struct ClipBinding {
  std::array<int16_t, kAnimClipCount> index{-1, -1, -1, -1, -1, -1};
  [[nodiscard]] int16_t operator[](AnimClip clip) const {
    return index[static_cast<std::size_t>(clip)];
  }
};
[[nodiscard]] ClipBinding bindClips(const SkinnedModel& model);

// The clip a player should show: Eliminated > HitStun > Dashing > throwing
// (the Throwing flag or a running throw hold) > moving > idle.
[[nodiscard]] AnimClip selectClip(const RenderPlayer& player, bool moving, bool throwing);

// Per-player animation state. Value-initialise one per slot; reset it (assign
// {}) when the slot leaves the match.
struct AnimState {
  bool initialised{false};
  AnimClip clip{AnimClip::Idle};      // the clip fading in (or playing)
  float time{0.0f};                   // seconds into `clip`
  AnimClip fromClip{AnimClip::Idle};  // the clip fading out
  float fromTime{0.0f};
  float fade{1.0f};  // crossfade progress: 0 = all fromClip, 1 = all clip
  bool moving{false};
  bool wasThrowing{false};
  float throwHold{0.0f};  // seconds of throw clip still owed
  Vec3 lastGround{};
};

// Advances `state` by dtSeconds (>= 0) for `player`. The first update snaps to
// the selected clip without a fade. A clip change, or a new throw while the
// throw clip plays, restarts the new clip at 0 and fades from the old one.
// Motion is the ground (sim) position change since the previous update; a
// jump beyond kSnapDistance (respawn, round reset) does not count as running.
// dtSeconds == 0 (a paused frame) keeps the clip, times and motion as they are.
void updateAnimState(AnimState& state, const RenderPlayer& player, float dtSeconds);

// Skin matrices for `state`: the current clip, blended from the outgoing one
// while the crossfade runs.
void evaluateAnimState(const SkinnedModel& model, const ClipBinding& binding,
                       const AnimState& state, SkinMatrices& out);

// One AnimState per player slot, keyed by RenderPlayer::slot.
using CharacterAnimStates = std::array<AnimState, kMaxRenderPlayers>;

// Updates every slot from `state`: players in it advance, slots absent from
// it are reset.
void updateCharacterAnimation(CharacterAnimStates& states, const RenderState& state,
                              float dtSeconds);

}  // namespace kfm::presentation
