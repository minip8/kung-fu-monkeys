// Skinned-model data and the animation layer (presentation/skinned_model.hpp,
// presentation/animation.hpp), on small hand-built models: quaternion math,
// bind-pose skin matrices, keyframe sampling, looping, crossfades, the clip
// state machine and the joint uniform budget.
#include "kfm/presentation/animation.hpp"

#include "kfm/presentation/math.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/presentation/skinned_model.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

using namespace kfm::presentation;
using Catch::Approx;

namespace {

bool nearlyEqual(const Mat4& a, const Mat4& b, float eps = 1e-4f) {
  for (std::size_t i = 0; i < 16; ++i) {
    if (std::fabs(a.m[i] - b.m[i]) > eps) return false;
  }
  return true;
}

bool nearlyEqual(Vec3 a, Vec3 b, float eps = 1e-4f) {
  return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps;
}

SkinnedVertex vertex(Vec3 p, float joint) {
  SkinnedVertex v{};
  v.px = p.x;
  v.py = p.y;
  v.pz = p.z;
  v.ny = 1.0f;
  v.joints = {joint, 0.0f, 0.0f, 0.0f};
  v.weights = {1.0f, 0.0f, 0.0f, 0.0f};
  return v;
}

// Two joints: a root at the origin and a child one unit up. One clip, "bend",
// rotates the child 90° about +z over one second (keys at 0 and 1), and
// "idle" bobs the root's translation.
SkinnedModel twoJointModel() {
  SkinnedModel m;
  m.joints.resize(2);
  m.joints[0].name = "root";
  m.joints[1].name = "child";
  m.joints[1].parent = 0;
  m.joints[1].translation = {0.0f, 1.0f, 0.0f};
  m.joints[1].inverseBind = translate({0.0f, -1.0f, 0.0f});
  m.vertices = {vertex({0.0f, 0.0f, 0.0f}, 0.0f), vertex({1.0f, 1.0f, 0.0f}, 1.0f),
                vertex({0.0f, 2.0f, 0.0f}, 1.0f)};
  m.indices = {0, 1, 2};

  AnimationClip bend;
  bend.name = "bend";
  bend.duration = 1.0f;
  bend.joints.resize(2);
  bend.joints[1].rotation.times = {0.0f, 1.0f};
  bend.joints[1].rotation.values = {Quat{}, quatFromAxisAngle({0.0f, 0.0f, 1.0f}, kPi * 0.5f)};
  m.clips.push_back(bend);

  AnimationClip idle;
  idle.name = "idle";
  idle.duration = 2.0f;
  idle.joints.resize(2);
  idle.joints[0].translation.times = {0.0f, 1.0f, 2.0f};
  idle.joints[0].translation.values = {Vec3{}, Vec3{0.0f, 0.5f, 0.0f}, Vec3{}};
  m.clips.push_back(idle);
  computeBounds(m);
  return m;
}

RenderPlayer playerWith(uint16_t flags, Vec3 ground = {}) {
  RenderPlayer p;
  p.flags = flags;
  p.ground = ground;
  p.position = ground;
  return p;
}

}  // namespace

TEST_CASE("joint budget fits WebGL2 vertex uniforms", "[presentation][animation]") {
  // Skinned VS block: mvp + model (8 vec4s) and 4 vec4s per joint. WebGL2
  // guarantees 256 vertex vec4 uniforms; keep well under half of it.
  STATIC_REQUIRE(8 + (4 * kMaxSkinJoints) <= 128);
  STATIC_REQUIRE(kMaxJointInfluences == 4);
}

TEST_CASE("quaternion helpers", "[presentation][animation]") {
  const Quat q = quatFromAxisAngle({0.0f, 1.0f, 0.0f}, kPi * 0.5f);
  // A quarter turn about +y maps +x onto -z, like rotateY.
  REQUIRE(nearlyEqual(transformDirection(quatToMat4(q), {1.0f, 0.0f, 0.0f}), {0.0f, 0.0f, -1.0f}));
  REQUIRE(nearlyEqual(quatToMat4(q), rotateY(kPi * 0.5f)));

  const Quat a{};
  const Quat half = slerp(a, q, 0.5f);
  REQUIRE(nearlyEqual(quatToMat4(half), rotateY(kPi * 0.25f)));
  // Shortest arc: -q is the same rotation.
  const Quat negQ{-q.x, -q.y, -q.z, -q.w};
  REQUIRE(nearlyEqual(quatToMat4(slerp(a, negQ, 0.5f)), rotateY(kPi * 0.25f)));
  REQUIRE(nearlyEqual(quatToMat4(nlerp(a, negQ, 0.5f)), rotateY(kPi * 0.25f)));

  const Mat4 trs = composeTrs({1.0f, 2.0f, 3.0f}, q, {2.0f, 2.0f, 2.0f});
  REQUIRE(nearlyEqual(
      trs, translate({1.0f, 2.0f, 3.0f}) * rotateY(kPi * 0.5f) * scale({2.0f, 2.0f, 2.0f})));
}

TEST_CASE("hand-built model validates", "[presentation][animation]") {
  const SkinnedModel m = twoJointModel();
  std::string error;
  REQUIRE(validateModel(m, error));
  REQUIRE(m.findClip("bend") == 0);
  REQUIRE(m.findClip("missing") == -1);
  REQUIRE(m.height() == Approx(2.0f));

  SkinnedModel bad = m;
  bad.vertices[1].weights = {0.5f, 0.0f, 0.0f, 0.0f};
  REQUIRE_FALSE(validateModel(bad, error));
  bad = m;
  bad.joints[1].parent = 1;
  REQUIRE_FALSE(validateModel(bad, error));
  bad = m;
  bad.joints.resize(kMaxSkinJoints + 1);
  REQUIRE_FALSE(validateModel(bad, error));
  bad = m;
  bad.indices.push_back(7);
  REQUIRE_FALSE(validateModel(bad, error));
}

TEST_CASE("bind pose gives identity skin matrices", "[presentation][animation]") {
  const SkinnedModel m = twoJointModel();
  Pose pose;
  restPose(m, pose);
  REQUIRE(pose.count == 2);
  SkinMatrices skin;
  skinMatrices(m, pose, skin);
  for (const Mat4& s : skin)
    REQUIRE(nearlyEqual(s, Mat4::identity()));
}

TEST_CASE("a joint rotation keyframe produces the expected skin matrix",
          "[presentation][animation]") {
  const SkinnedModel m = twoJointModel();
  Pose pose;
  SkinMatrices skin;

  sampleClip(m, m.clips[0], 1.0f, false, pose);
  skinMatrices(m, pose, skin);
  REQUIRE(nearlyEqual(skin[0], Mat4::identity()));
  // Child rotated 90° about +z around its bind position (0, 1, 0).
  const Mat4 expected = translate({0.0f, 1.0f, 0.0f}) *
                        quatToMat4(quatFromAxisAngle({0.0f, 0.0f, 1.0f}, kPi * 0.5f)) *
                        translate({0.0f, -1.0f, 0.0f});
  REQUIRE(nearlyEqual(skin[1], expected));
  // The vertex at the child's tip (0, 2, 0) swings onto (-1, 1, 0).
  REQUIRE(nearlyEqual(transformPoint(skin[1], {0.0f, 2.0f, 0.0f}), {-1.0f, 1.0f, 0.0f}));

  // Halfway: 45° (slerp between the keys).
  sampleClip(m, m.clips[0], 0.5f, false, pose);
  skinMatrices(m, pose, skin);
  const float h = std::sqrt(0.5f);
  REQUIRE(nearlyEqual(transformPoint(skin[1], {0.0f, 2.0f, 0.0f}), {-h, 1.0f + h, 0.0f}));

  // Step keys hold the earlier value.
  SkinnedModel stepped = m;
  stepped.clips[0].joints[1].rotation.interpolation = KeyInterpolation::Step;
  sampleClip(stepped, stepped.clips[0], 0.9f, false, pose);
  skinMatrices(stepped, pose, skin);
  REQUIRE(nearlyEqual(skin[1], Mat4::identity()));

  // A translation channel moves the root and, through the hierarchy, the child.
  sampleClip(m, m.clips[1], 1.0f, true, pose);
  skinMatrices(m, pose, skin);
  REQUIRE(nearlyEqual(skin[0], translate({0.0f, 0.5f, 0.0f})));
  REQUIRE(nearlyEqual(skin[1], translate({0.0f, 0.5f, 0.0f})));
}

TEST_CASE("clip time loops or clamps", "[presentation][animation]") {
  REQUIRE(clipTime(0.25f, 1.0f, true) == Approx(0.25f));
  REQUIRE(clipTime(1.25f, 1.0f, true) == Approx(0.25f));
  REQUIRE(clipTime(3.75f, 1.0f, true) == Approx(0.75f));
  REQUIRE(clipTime(-0.25f, 1.0f, true) == Approx(0.75f));
  REQUIRE(clipTime(1.25f, 1.0f, false) == Approx(1.0f));
  REQUIRE(clipTime(-1.0f, 1.0f, false) == Approx(0.0f));
  REQUIRE(clipTime(5.0f, 0.0f, true) == 0.0f);

  // A looping sample one period later is the same pose.
  const SkinnedModel m = twoJointModel();
  Pose a;
  Pose b;
  sampleClip(m, m.clips[1], 0.6f, true, a);
  sampleClip(m, m.clips[1], 2.6f, true, b);
  REQUIRE(nearlyEqual(a.joints[0].translation, b.joints[0].translation));
  // Clamped: past the end holds the last key.
  sampleClip(m, m.clips[0], 7.0f, false, a);
  sampleClip(m, m.clips[0], 1.0f, false, b);
  REQUIRE(a.joints[1].rotation.z == Approx(b.joints[1].rotation.z));
}

TEST_CASE("blending poses", "[presentation][animation]") {
  const SkinnedModel m = twoJointModel();
  Pose rest;
  Pose bent;
  restPose(m, rest);
  sampleClip(m, m.clips[0], 1.0f, false, bent);
  Pose out;
  blendPoses(rest, bent, 0.0f, out);
  REQUIRE(nearlyEqual(quatToMat4(out.joints[1].rotation), Mat4::identity()));
  blendPoses(rest, bent, 1.0f, out);
  REQUIRE(nearlyEqual(quatToMat4(out.joints[1].rotation), quatToMat4(bent.joints[1].rotation)));
  blendPoses(rest, bent, 0.5f, out);
  // nlerp halfway between 0° and 90° about z is 45°.
  REQUIRE(nearlyEqual(quatToMat4(out.joints[1].rotation),
                      quatToMat4(quatFromAxisAngle({0.0f, 0.0f, 1.0f}, kPi * 0.25f))));
  // Aliasing the output with an input is allowed.
  Pose alias = rest;
  blendPoses(alias, bent, 1.0f, alias);
  REQUIRE(nearlyEqual(quatToMat4(alias.joints[1].rotation), quatToMat4(bent.joints[1].rotation)));
}

TEST_CASE("clip binding by name", "[presentation][animation]") {
  const SkinnedModel m = twoJointModel();
  const ClipBinding b = bindClips(m);
  REQUIRE(b[AnimClip::Idle] == 1);
  REQUIRE(b[AnimClip::Run] == -1);
  REQUIRE(animClipName(AnimClip::Eliminated) == "eliminated");
  REQUIRE(animClipLoops(AnimClip::Idle));
  REQUIRE(animClipLoops(AnimClip::Run));
  REQUIRE_FALSE(animClipLoops(AnimClip::Dash));
  REQUIRE_FALSE(animClipLoops(AnimClip::Eliminated));

  // A missing clip plays the rest pose.
  AnimState s;
  s.initialised = true;
  s.clip = AnimClip::Run;
  SkinMatrices skin;
  evaluateAnimState(m, b, s, skin);
  REQUIRE(nearlyEqual(skin[1], Mat4::identity()));
}

TEST_CASE("clip selection priority", "[presentation][animation]") {
  const auto f = [](PlayerFlag flag) { return flagBit(flag); };
  REQUIRE(selectClip(playerWith(0), false, false) == AnimClip::Idle);
  REQUIRE(selectClip(playerWith(0), true, false) == AnimClip::Run);
  REQUIRE(selectClip(playerWith(0), true, true) == AnimClip::Throw);
  REQUIRE(selectClip(playerWith(f(PlayerFlag::Dashing)), true, true) == AnimClip::Dash);
  REQUIRE(selectClip(playerWith(f(PlayerFlag::Dashing) | f(PlayerFlag::HitStun)), true, true) ==
          AnimClip::Hit);
  REQUIRE(selectClip(playerWith(f(PlayerFlag::HitStun) | f(PlayerFlag::Eliminated)), true, true) ==
          AnimClip::Eliminated);
  // Flags that do not drive a clip.
  REQUIRE(selectClip(playerWith(f(PlayerFlag::HasAmmo) | f(PlayerFlag::Invulnerable) |
                                f(PlayerFlag::Shielded) | f(PlayerFlag::PerfectDash)),
                     false, false) == AnimClip::Idle);
}

TEST_CASE("state machine transitions on flags and motion", "[presentation][animation]") {
  constexpr float kDt = 1.0f / 60.0f;
  AnimState s;
  RenderPlayer p = playerWith(0, {1.0f, 0.0f, 1.0f});

  // First update snaps without a fade.
  updateAnimState(s, p, kDt);
  REQUIRE(s.initialised);
  REQUIRE(s.clip == AnimClip::Idle);
  REQUIRE(s.fade == 1.0f);

  // Standing still stays idle; time advances.
  updateAnimState(s, p, kDt);
  REQUIRE(s.clip == AnimClip::Idle);
  REQUIRE(s.time == Approx(kDt));

  // Moving 0.1 units per frame (6 u/s) runs.
  p.ground.x += 0.1f;
  updateAnimState(s, p, kDt);
  REQUIRE(s.clip == AnimClip::Run);
  REQUIRE(s.fromClip == AnimClip::Idle);
  REQUIRE(s.time == 0.0f);
  REQUIRE(s.fade == 0.0f);

  // Creeping below the threshold is idle again.
  p.ground.x += 0.001f;
  updateAnimState(s, p, kDt);
  REQUIRE(s.clip == AnimClip::Idle);

  // A teleport (round reset) is not running.
  p.ground.x += 10.0f;
  updateAnimState(s, p, kDt);
  REQUIRE(s.clip == AnimClip::Idle);

  // Each flag, by priority.
  p.flags = flagBit(PlayerFlag::Dashing);
  updateAnimState(s, p, kDt);
  REQUIRE(s.clip == AnimClip::Dash);
  p.flags = flagBit(PlayerFlag::HitStun);
  updateAnimState(s, p, kDt);
  REQUIRE(s.clip == AnimClip::Hit);
  p.flags = flagBit(PlayerFlag::Eliminated);
  updateAnimState(s, p, kDt);
  REQUIRE(s.clip == AnimClip::Eliminated);
  p.flags = 0;
  updateAnimState(s, p, kDt);
  REQUIRE(s.clip == AnimClip::Idle);

  // A paused frame (dt 0) changes nothing.
  const AnimState before = s;
  p.flags = flagBit(PlayerFlag::Dashing);
  updateAnimState(s, p, 0.0f);
  REQUIRE(s.clip == before.clip);
  REQUIRE(s.time == before.time);
}

TEST_CASE("throw clip holds after a short Throwing flag", "[presentation][animation]") {
  constexpr float kDt = 1.0f / 60.0f;
  AnimState s;
  RenderPlayer p = playerWith(0);
  updateAnimState(s, p, kDt);

  p.flags = flagBit(PlayerFlag::Throwing);
  updateAnimState(s, p, kDt);
  REQUIRE(s.clip == AnimClip::Throw);
  // The flag drops after two frames; the clip keeps playing for the hold.
  updateAnimState(s, p, kDt);
  p.flags = 0;
  float elapsed = 2.0f * kDt;
  while (elapsed + kDt < kThrowAnimSeconds) {
    updateAnimState(s, p, kDt);
    elapsed += kDt;
    REQUIRE(s.clip == AnimClip::Throw);
  }
  updateAnimState(s, p, kDt);
  updateAnimState(s, p, kDt);
  REQUIRE(s.clip == AnimClip::Idle);

  // A second throw while the throw clip still plays restarts it.
  p.flags = flagBit(PlayerFlag::Throwing);
  updateAnimState(s, p, kDt);
  for (int i = 0; i < 5; ++i)
    updateAnimState(s, p, kDt);
  p.flags = 0;
  updateAnimState(s, p, kDt);
  REQUIRE(s.clip == AnimClip::Throw);
  REQUIRE(s.time > 0.0f);
  p.flags = flagBit(PlayerFlag::Throwing);
  updateAnimState(s, p, kDt);
  REQUIRE(s.clip == AnimClip::Throw);
  REQUIRE(s.time == 0.0f);
  REQUIRE(s.fade == 0.0f);
}

TEST_CASE("crossfade weights", "[presentation][animation]") {
  const SkinnedModel m = twoJointModel();
  SkinnedModel model = m;
  // Name the bend clip "dash" so the state machine can play it.
  model.clips[0].name = "dash";
  const ClipBinding binding = bindClips(model);

  AnimState s;
  RenderPlayer p = playerWith(0);
  updateAnimState(s, p, 0.01f);
  p.flags = flagBit(PlayerFlag::Dashing);
  updateAnimState(s, p, 0.01f);
  REQUIRE(s.clip == AnimClip::Dash);
  REQUIRE(s.fade == 0.0f);

  // Fully the outgoing (idle) pose at the switch...
  SkinMatrices skin;
  evaluateAnimState(model, binding, s, skin);
  Pose idle;
  sampleClip(model, model.clips[1], s.fromTime, true, idle);
  SkinMatrices idleSkin;
  skinMatrices(model, idle, idleSkin);
  REQUIRE(nearlyEqual(skin[1], idleSkin[1]));

  // ... half way after half the crossfade, done after all of it.
  updateAnimState(s, p, kAnimCrossfadeSeconds * 0.5f);
  REQUIRE(s.fade == Approx(0.5f));
  updateAnimState(s, p, kAnimCrossfadeSeconds * 0.5f);
  REQUIRE(s.fade == Approx(1.0f));
  updateAnimState(s, p, 1.0f);
  REQUIRE(s.fade == 1.0f);

  // Mid-fade the pose is the blend of the two clips.
  AnimState mid = s;
  mid.fromClip = AnimClip::Idle;
  mid.fromTime = 0.0f;
  mid.time = 1.0f;  // dash clip at its end: 90°
  mid.fade = 0.5f;
  evaluateAnimState(model, binding, mid, skin);
  Pose rest;
  Pose bent;
  Pose half;
  sampleClip(model, model.clips[1], 0.0f, true, rest);
  sampleClip(model, model.clips[0], 1.0f, false, bent);
  blendPoses(rest, bent, 0.5f, half);
  SkinMatrices expected;
  skinMatrices(model, half, expected);
  REQUIRE(nearlyEqual(skin[1], expected[1]));
}

TEST_CASE("character animation is keyed by slot", "[presentation][animation]") {
  CharacterAnimStates states{};
  RenderState rs;
  rs.playerCount = 2;
  rs.players[0].slot = 3;
  rs.players[1].slot = 7;
  rs.players[1].flags = flagBit(PlayerFlag::Eliminated);
  updateCharacterAnimation(states, rs, 1.0f / 60.0f);
  REQUIRE(states[3].initialised);
  REQUIRE(states[7].initialised);
  REQUIRE(states[7].clip == AnimClip::Eliminated);
  REQUIRE_FALSE(states[0].initialised);

  // A slot that leaves is reset.
  rs.playerCount = 1;
  updateCharacterAnimation(states, rs, 1.0f / 60.0f);
  REQUIRE(states[3].initialised);
  REQUIRE_FALSE(states[7].initialised);
}
