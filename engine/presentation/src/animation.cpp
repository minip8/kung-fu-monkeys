#include "kfm/presentation/animation.hpp"

#include "kfm/presentation/tuning.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <utility>

namespace kfm::presentation {

namespace {

// Index of the key segment [k, k + 1] holding t and the blend inside it.
struct KeySpan {
  std::size_t k0{0};
  std::size_t k1{0};
  float u{0.0f};
};

[[nodiscard]] KeySpan findSpan(const std::vector<float>& times, float t, KeyInterpolation interp) {
  const std::size_t n = times.size();
  if (n == 1 || t <= times.front()) return {};
  if (t >= times.back()) return {n - 1, n - 1, 0.0f};
  // First key strictly after t; t is inside (times[hi - 1], times[hi]].
  const auto it = std::ranges::upper_bound(times, t);
  const auto hi = static_cast<std::size_t>(std::distance(times.begin(), it));
  const std::size_t lo = hi - 1;
  if (interp == KeyInterpolation::Step) return {lo, lo, 0.0f};
  const float span = times[hi] - times[lo];
  const float u = span > 0.0f ? (t - times[lo]) / span : 0.0f;
  return {lo, hi, clamp01(u)};
}

[[nodiscard]] Vec3 sampleTrack(const Track<Vec3>& track, float t) {
  const KeySpan s = findSpan(track.times, t, track.interpolation);
  return lerp(track.values[s.k0], track.values[s.k1], s.u);
}

[[nodiscard]] Quat sampleTrack(const Track<Quat>& track, float t) {
  const KeySpan s = findSpan(track.times, t, track.interpolation);
  if (s.k0 == s.k1) return normalize(track.values[s.k0]);
  return slerp(track.values[s.k0], track.values[s.k1], s.u);
}

constexpr std::array<std::string_view, kAnimClipCount> kClipNames = {
    "idle", "run", "dash", "throw", "hit", "eliminated", "charge"};

// The charge clip is scrubbed: sampled at charge x its duration, whatever
// time it has been playing (v0.53).
void sampleBound(const SkinnedModel& model, const ClipBinding& binding, AnimClip clip, float time,
                 float charge, Pose& out) {
  const int16_t index = binding[clip];
  if (index < 0 || static_cast<std::size_t>(index) >= model.clips.size()) {
    restPose(model, out);
    return;
  }
  const AnimationClip& c = model.clips[static_cast<std::size_t>(index)];
  const float t = clip == AnimClip::Charge ? clamp01(charge) * c.duration : time;
  sampleClip(model, c, t, animClipLoops(clip), out);
}

[[nodiscard]] int16_t jointNamed(const SkinnedModel& model, std::string_view name) {
  for (std::size_t j = 0; j < model.joints.size() && j < kMaxSkinJoints; ++j) {
    if (model.joints[j].name == name) return static_cast<int16_t>(j);
  }
  return -1;
}

// v0.53: the strain, a small shake about the model's z axis (the arm's
// cocking axis) and, for the spine, its x axis.
void addTremble(Pose& pose, int16_t joint, Vec3 axis, float degrees) {
  if (joint < 0 || std::cmp_greater_equal(joint, pose.count)) return;
  JointPose& jp = pose.joints[static_cast<std::size_t>(joint)];
  jp.rotation = normalize(jp.rotation * quatFromAxisAngle(axis, degrees * (kPi / 180.0f)));
}

void applyStrain(const ClipBinding& binding, const AnimState& state, Pose& pose) {
  if (!(state.strain > 0.0f)) return;
  const float t = state.strainTime;
  const float wave =
      std::sin(kTwoPi * kStrainHz * t) + (0.5f * std::sin((kTwoPi * kStrainHz2 * t) + 1.3f));
  const float w = state.strain * wave;
  addTremble(pose, binding.upperArm, Vec3{0.0f, 0.0f, 1.0f}, kStrainArmDegrees * w);
  addTremble(pose, binding.forearm, Vec3{0.0f, 0.0f, 1.0f}, -kStrainForearmDegrees * w);
  addTremble(pose, binding.spine, Vec3{1.0f, 0.0f, 0.0f}, kStrainSpineDegrees * w);
}

// Joints that both the model and the pose provide, capped at the skin budget.
[[nodiscard]] std::size_t jointCount(const SkinnedModel& model, const Pose& pose) {
  return std::ranges::min({model.joints.size(), static_cast<std::size_t>(pose.count),
                           static_cast<std::size_t>(kMaxSkinJoints)});
}

}  // namespace

float clipTime(float time, float duration, bool loop) {
  if (!(duration > 0.0f)) return 0.0f;
  if (loop) {
    const float wrapped = std::fmod(time, duration);
    return wrapped < 0.0f ? wrapped + duration : wrapped;
  }
  return std::clamp(time, 0.0f, duration);
}

void restPose(const SkinnedModel& model, Pose& out) {
  const std::size_t n = std::min<std::size_t>(model.joints.size(), kMaxSkinJoints);
  out.count = static_cast<uint32_t>(n);
  for (std::size_t j = 0; j < n; ++j) {
    const Joint& joint = model.joints[j];
    out.joints[j] = {
        .translation = joint.translation, .rotation = joint.rotation, .scale = joint.scale};
  }
}

void sampleClip(const SkinnedModel& model, const AnimationClip& clip, float time, bool loop,
                Pose& out) {
  restPose(model, out);
  const float t = clipTime(time, clip.duration, loop);
  const std::size_t n = std::min<std::size_t>(out.count, clip.joints.size());
  for (std::size_t j = 0; j < n; ++j) {
    const JointTrack& track = clip.joints[j];
    if (!track.translation.empty()) out.joints[j].translation = sampleTrack(track.translation, t);
    if (!track.rotation.empty()) out.joints[j].rotation = sampleTrack(track.rotation, t);
  }
}

void blendPoses(const Pose& a, const Pose& b, float weightB, Pose& out) {
  const float w = clamp01(weightB);
  const uint32_t n = std::min(a.count, b.count);
  for (uint32_t j = 0; j < n; ++j) {
    const JointPose& pa = a.joints[j];
    const JointPose& pb = b.joints[j];
    out.joints[j] = {.translation = lerp(pa.translation, pb.translation, w),
                     .rotation = nlerp(pa.rotation, pb.rotation, w),
                     .scale = lerp(pa.scale, pb.scale, w)};
  }
  out.count = n;
}

void globalTransforms(const SkinnedModel& model, const Pose& pose, SkinMatrices& out) {
  out.fill(Mat4::identity());
  const std::size_t n = jointCount(model, pose);
  for (std::size_t j = 0; j < n; ++j) {
    const JointPose& p = pose.joints[j];
    const Mat4 local = composeTrs(p.translation, p.rotation, p.scale);
    const int parent = model.joints[j].parent;
    // Parents precede children (validateModel), so out[parent] is final.
    out[j] =
        parent >= 0 ? out[static_cast<std::size_t>(parent)] * local : model.joints[j].base * local;
  }
}

void skinMatrices(const SkinnedModel& model, const Pose& pose, SkinMatrices& out) {
  globalTransforms(model, pose, out);
  const std::size_t n = jointCount(model, pose);
  for (std::size_t j = 0; j < n; ++j)
    out[j] = out[j] * model.joints[j].inverseBind;
}

std::string_view animClipName(AnimClip clip) {
  return kClipNames[static_cast<std::size_t>(clip)];
}

ClipBinding bindClips(const SkinnedModel& model) {
  ClipBinding b;
  for (std::size_t c = 0; c < kAnimClipCount; ++c)
    b.index[c] = static_cast<int16_t>(model.findClip(kClipNames[c]));
  b.spine = jointNamed(model, "spine");
  b.upperArm = jointNamed(model, "arm_upper_r");
  b.forearm = jointNamed(model, "arm_lower_r");
  b.hand = jointNamed(model, "hand_r");
  return b;
}

AnimClip selectClip(const RenderPlayer& player, bool moving, bool throwing) {
  if (player.has(PlayerFlag::Eliminated)) return AnimClip::Eliminated;
  if (player.has(PlayerFlag::HitStun)) return AnimClip::Hit;
  if (player.has(PlayerFlag::Dashing)) return AnimClip::Dash;
  if (player.has(PlayerFlag::Charging)) return AnimClip::Charge;
  if (throwing) return AnimClip::Throw;
  return moving ? AnimClip::Run : AnimClip::Idle;
}

void updateAnimState(AnimState& state, const RenderPlayer& player, float dtSeconds) {
  const bool throwFlag = player.has(PlayerFlag::Throwing);
  if (!state.initialised) {
    state = AnimState{};
    state.initialised = true;
    state.lastGround = player.ground;
    state.wasThrowing = throwFlag;
    state.throwHold = throwFlag ? kThrowAnimSeconds : 0.0f;
    state.wasCharging = player.has(PlayerFlag::Charging);
    state.charge = state.wasCharging ? player.throwCharge : 0.0f;
    state.clip = selectClip(player, false, throwFlag);
    state.fromClip = state.clip;
    return;
  }
  const float dt = std::max(dtSeconds, 0.0f);
  if (dt <= 0.0f) return;

  const Vec3 delta = player.ground - state.lastGround;
  const float moved = std::sqrt((delta.x * delta.x) + (delta.z * delta.z));
  state.moving = moved < kSnapDistance && moved > kRunSpeedThreshold * dt;
  state.lastGround = player.ground;

  const bool throwStarted = throwFlag && !state.wasThrowing;
  state.wasThrowing = throwFlag;
  state.throwHold = throwStarted ? kThrowAnimSeconds : std::max(state.throwHold - dt, 0.0f);

  // v0.53: while charging the pose follows the charge; the release owes a
  // whole throw clip (the press's hold may have run out during the charge).
  const bool charging = player.has(PlayerFlag::Charging);
  const bool released = state.wasCharging && !charging;
  state.wasCharging = charging;
  if (charging) state.charge = clamp01(player.throwCharge);
  if (released) state.throwHold = kThrowAnimSeconds;
  const float strainTarget = charging && player.throwCharge >= kStrainFromCharge ? 1.0f : 0.0f;
  const float ramp = dt / kStrainRampSeconds;
  state.strain = strainTarget > state.strain ? std::min(state.strain + ramp, strainTarget)
                                             : std::max(state.strain - ramp, strainTarget);
  state.strainTime = state.strain > 0.0f ? state.strainTime + dt : 0.0f;

  state.time += dt;
  state.fromTime += dt;
  state.fade = std::min(state.fade + (dt / kAnimCrossfadeSeconds), 1.0f);

  const AnimClip target = selectClip(player, state.moving, throwFlag || state.throwHold > 0.0f);
  if (target != state.clip || (throwStarted && target == AnimClip::Throw)) {
    // A release goes into the throw past as much of its wind-up as the
    // charge pose already did.
    const bool fromCharge = state.clip == AnimClip::Charge && target == AnimClip::Throw;
    state.fromClip = state.clip;
    state.fromTime = state.time;
    state.clip = target;
    state.time = fromCharge ? kThrowWindupSeconds * state.charge : 0.0f;
    state.fade = 0.0f;
  }
}

void evaluateAnimPose(const SkinnedModel& model, const ClipBinding& binding, const AnimState& state,
                      Pose& out) {
  sampleBound(model, binding, state.clip, state.time, state.charge, out);
  if (state.fade < 1.0f) {
    Pose from;
    sampleBound(model, binding, state.fromClip, state.fromTime, state.charge, from);
    blendPoses(from, out, state.fade, out);
  }
  applyStrain(binding, state, out);
}

void evaluateAnimState(const SkinnedModel& model, const ClipBinding& binding,
                       const AnimState& state, SkinMatrices& out, SkinMatrices* globals) {
  Pose current;
  evaluateAnimPose(model, binding, state, current);
  if (globals != nullptr) {
    globalTransforms(model, current, *globals);
    const std::size_t n = jointCount(model, current);
    for (std::size_t j = 0; j < n; ++j)
      out[j] = (*globals)[j] * model.joints[j].inverseBind;
    for (std::size_t j = n; j < out.size(); ++j)
      out[j] = Mat4::identity();
    return;
  }
  skinMatrices(model, current, out);
}

void updateCharacterAnimation(CharacterAnimStates& states, const RenderState& state,
                              float dtSeconds) {
  std::array<bool, kMaxRenderPlayers> seen{};
  for (uint16_t i = 0; i < state.playerCount; ++i) {
    const RenderPlayer& p = state.players[i];
    if (p.slot >= kMaxRenderPlayers || !p.visible) continue;
    updateAnimState(states[p.slot], p, dtSeconds);
    seen[p.slot] = true;
  }
  for (std::size_t s = 0; s < kMaxRenderPlayers; ++s) {
    if (!seen[s]) states[s] = AnimState{};
  }
}

}  // namespace kfm::presentation
