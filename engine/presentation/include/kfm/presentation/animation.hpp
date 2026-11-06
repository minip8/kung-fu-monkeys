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
//                     priority: eliminated > hit > dash > charge > throw > run
//                     > idle. v0.53: the charge clip is a pose scrubbed by the
//                     throw's charge, not played over time, with a procedural
//                     strain tremble once it is full.
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
// v0.53, the charged throw. The throw clip's own wind-up (its first 30 %):
// a release starts the clip charge x this far in, since the charge pose has
// already wound up.
inline constexpr float kThrowWindupSeconds = 0.1f;
// The strain at full charge: it fades in over kStrainRampSeconds once the
// charge reaches kStrainFromCharge, and shakes the spine and the throwing arm
// by these amplitudes (degrees) at two incommensurate frequencies (Hz).
inline constexpr float kStrainFromCharge = 0.999f;
inline constexpr float kStrainRampSeconds = 0.15f;
inline constexpr float kStrainHz = 23.0f;
inline constexpr float kStrainHz2 = 37.0f;
inline constexpr float kStrainArmDegrees = 4.0f;
inline constexpr float kStrainForearmDegrees = 5.0f;
inline constexpr float kStrainSpineDegrees = 1.5f;

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

enum class AnimClip : uint8_t { Idle = 0, Run, Dash, Throw, Hit, Eliminated, Charge };
inline constexpr std::size_t kAnimClipCount = 7;

// The glTF animation name of each clip: idle, run, dash, throw, hit,
// eliminated, charge (v0.53).
[[nodiscard]] std::string_view animClipName(AnimClip clip);
// Idle and run loop; the others play once and hold their last frame.
[[nodiscard]] constexpr bool animClipLoops(AnimClip clip) {
  return clip == AnimClip::Idle || clip == AnimClip::Run;
}

// Model clip index per AnimClip (-1: the model has no such clip), and
// (v0.53) the joints the strain shakes and the hand the held banana sits in,
// found by name (spine, arm_upper_r, arm_lower_r, hand_r; -1 when missing).
struct ClipBinding {
  std::array<int16_t, kAnimClipCount> index{-1, -1, -1, -1, -1, -1, -1};
  int16_t spine{-1};
  int16_t upperArm{-1};
  int16_t forearm{-1};
  int16_t hand{-1};
  [[nodiscard]] int16_t operator[](AnimClip clip) const {
    return index[static_cast<std::size_t>(clip)];
  }
};
[[nodiscard]] ClipBinding bindClips(const SkinnedModel& model);

// The clip a player should show: Eliminated > HitStun > Dashing > Charging
// (v0.53) > throwing (the Throwing flag or a running throw hold) > moving >
// idle.
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
  // v0.53: the charge the charge clip is sampled at (held after the release,
  // so a fade out of it stays put), the strain's weight and its clock, and
  // whether the last update was charging.
  float charge{0.0f};
  float strain{0.0f};
  float strainTime{0.0f};
  bool wasCharging{false};
};

// Advances `state` by dtSeconds (>= 0) for `player`. The first update snaps to
// the selected clip without a fade. A clip change, or a new throw while the
// throw clip plays, restarts the new clip at 0 and fades from the old one.
// Motion is the ground (sim) position change since the previous update; a
// jump beyond kSnapDistance (respawn, round reset) does not count as running.
// dtSeconds == 0 (a paused frame) keeps the clip, times and motion as they are.
void updateAnimState(AnimState& state, const RenderPlayer& player, float dtSeconds);

// The pose for `state`: the current clip, blended from the outgoing one
// while the crossfade runs, with the strain on top (v0.53).
void evaluateAnimPose(const SkinnedModel& model, const ClipBinding& binding, const AnimState& state,
                      Pose& out);

// Skin matrices for `state` (evaluateAnimPose, skinned); `globals`, when
// given, also receives each joint's model-space transform (v0.53: the hand).
void evaluateAnimState(const SkinnedModel& model, const ClipBinding& binding,
                       const AnimState& state, SkinMatrices& out, SkinMatrices* globals = nullptr);

// One AnimState per player slot, keyed by RenderPlayer::slot.
using CharacterAnimStates = std::array<AnimState, kMaxRenderPlayers>;

// Updates every slot from `state`: players in it advance, slots absent from
// it are reset.
void updateCharacterAnimation(CharacterAnimStates& states, const RenderState& state,
                              float dtSeconds);

}  // namespace kfm::presentation
