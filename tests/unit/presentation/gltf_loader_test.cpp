// The glTF loader (presentation/gltf_loader.hpp) on the rigged test model
// that tools/assets/gen_test_rig.py writes at build time. KFM_TEST_RIG_PATH
// is set by tests/unit/CMakeLists.txt: the generated file in the build tree
// natively, and the copy embedded at /content/models/test_rig.glb in the
// wasm test module. If the file is absent the file-based cases skip with a
// message instead of failing.
#include "kfm/presentation/gltf_loader.hpp"

#include "kfm/presentation/animation.hpp"
#include "kfm/presentation/math.hpp"
#include "kfm/presentation/skinned_model.hpp"
#include "kfm/presentation/tuning.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>
#include <ios>
#include <string>

#ifndef KFM_TEST_RIG_PATH
#error "KFM_TEST_RIG_PATH must name the generated test_rig.glb"
#endif

using namespace kfm::presentation;
using Catch::Approx;

namespace {

bool rigPresent() {
  const std::ifstream f(KFM_TEST_RIG_PATH, std::ios::binary);
  return f.good();
}

#define REQUIRE_RIG()                                                              \
  do {                                                                             \
    if (!rigPresent()) SKIP("generated test rig not found at " KFM_TEST_RIG_PATH); \
  } while (false)

const SkinnedModel& rig() {
  static const SkinnedModel model = [] {
    SkinnedModel m;
    std::string error;
    if (!loadSkinnedModel(KFM_TEST_RIG_PATH, m, error)) FAIL(error);
    return m;
  }();
  return model;
}

bool nearlyIdentity(const Mat4& a, float eps) {
  const Mat4 id = Mat4::identity();
  for (std::size_t i = 0; i < 16; ++i) {
    if (std::fabs(a.m[i] - id.m[i]) > eps) return false;
  }
  return true;
}

Vec3 jointPosition(const SkinnedModel& m, const Pose& pose, std::size_t joint) {
  SkinMatrices global;
  globalTransforms(m, pose, global);
  return transformPoint(global[joint], {});
}

}  // namespace

TEST_CASE("missing or malformed model files fail cleanly", "[presentation][gltf]") {
  SkinnedModel m;
  std::string error;
  REQUIRE_FALSE(loadSkinnedModel("/nonexistent/kfm/test_rig.glb", m, error));
  REQUIRE_FALSE(error.empty());
  REQUIRE(m.empty());
  error.clear();
  REQUIRE_FALSE(loadSkinnedModel("", m, error));
  REQUIRE_FALSE(error.empty());
  REQUIRE_FALSE(loadSkinnedModel(nullptr, m, error));
}

TEST_CASE("the generated test rig loads", "[presentation][gltf]") {
  REQUIRE_RIG();
  const SkinnedModel& m = rig();
  std::string error;
  INFO(error);
  REQUIRE(validateModel(m, error));

  // Skeleton: 7 joints plus the unweighted hand_r leaf (v0.53), parents
  // first, within the uniform budget.
  REQUIRE(m.joints.size() == 8);
  REQUIRE(m.joints[7].name == "hand_r");
  REQUIRE(m.joints[7].parent == 6);
  REQUIRE(m.joints.size() <= kMaxSkinJoints);
  REQUIRE(m.joints[0].name == "root");
  REQUIRE(m.joints[0].parent == -1);
  for (std::size_t j = 1; j < m.joints.size(); ++j)
    REQUIRE(m.joints[j].parent < static_cast<int>(j));

  // Every clip the state machine plays exists and has a length.
  const ClipBinding b = bindClips(m);
  for (std::size_t c = 0; c < kAnimClipCount; ++c) {
    const auto clip = static_cast<AnimClip>(c);
    INFO(animClipName(clip));
    REQUIRE(b[clip] >= 0);
    REQUIRE(m.clips[static_cast<std::size_t>(b[clip])].duration > 0.0f);
  }

  // Sized like the capsule in radius units: feet on y = 0, kMonkeyHeightOverRadius tall,
  // facing +x (at head height the muzzle sticks out further forward than the back of the head).
  REQUIRE(m.boundsMin.y == Approx(0.0f).margin(1e-4));
  REQUIRE(m.height() == Approx(kMonkeyHeightOverRadius).margin(1e-3));
  float headFront = 0.0f;
  float headBack = 0.0f;
  for (const SkinnedVertex& v : m.vertices) {
    if (v.py < 2.0f) continue;
    headFront = std::fmax(headFront, v.px);
    headBack = std::fmax(headBack, -v.px);
  }
  REQUIRE(headFront > headBack + 0.2f);
  REQUIRE(m.boundsMax.z == Approx(-m.boundsMin.z).margin(1e-4));  // left-right symmetric

  // Mesh: at most 4 normalised influences per vertex, unit normals.
  REQUIRE(m.vertices.size() < 65536);
  for (const SkinnedVertex& v : m.vertices) {
    const float sum = v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3];
    REQUIRE(sum == Approx(1.0f).margin(1e-4));
    REQUIRE(std::sqrt((v.nx * v.nx) + (v.ny * v.ny) + (v.nz * v.nz)) == Approx(1.0f).margin(1e-3));
  }
}

TEST_CASE("the test rig's bind pose skins to identity", "[presentation][gltf]") {
  REQUIRE_RIG();
  const SkinnedModel& m = rig();
  Pose pose;
  restPose(m, pose);
  SkinMatrices skin;
  skinMatrices(m, pose, skin);
  for (const Mat4& s : skin)
    REQUIRE(nearlyIdentity(s, 1e-4f));
}

TEST_CASE("the test rig's clips move the skeleton as intended", "[presentation][gltf]") {
  REQUIRE_RIG();
  const SkinnedModel& m = rig();
  const ClipBinding b = bindClips(m);
  const auto clipOf = [&](AnimClip c) -> const AnimationClip& {
    return m.clips[static_cast<std::size_t>(b[c])];
  };
  constexpr std::size_t kHead = 2;
  constexpr std::size_t kLowerArmL = 4;
  Pose rest;
  restPose(m, rest);
  const Vec3 headRest = jointPosition(m, rest, kHead);

  // Run swings the arms: two times a quarter cycle apart differ.
  Pose a;
  Pose c;
  const AnimationClip& run = clipOf(AnimClip::Run);
  sampleClip(m, run, run.duration * 0.25f, true, a);
  sampleClip(m, run, run.duration * 0.75f, true, c);
  REQUIRE(std::fabs(jointPosition(m, a, kLowerArmL).x - jointPosition(m, c, kLowerArmL).x) > 0.3f);
  // ... and loops: the end equals the start.
  sampleClip(m, run, 0.0f, true, a);
  sampleClip(m, run, run.duration - 1e-4f, true, c);
  REQUIRE(jointPosition(m, a, kLowerArmL).x ==
          Approx(jointPosition(m, c, kLowerArmL).x).margin(1e-2));

  // Dash leans forward (+x): the head moves ahead of its rest position.
  sampleClip(m, clipOf(AnimClip::Dash), 10.0f, false, a);
  REQUIRE(jointPosition(m, a, kHead).x > headRest.x + 0.5f);

  // Eliminated slumps: the head ends forward and lower.
  sampleClip(m, clipOf(AnimClip::Eliminated), 10.0f, false, a);
  const Vec3 slumped = jointPosition(m, a, kHead);
  REQUIRE(slumped.x > headRest.x + 0.5f);
  REQUIRE(slumped.y < headRest.y - 0.3f);

  // Hit recoils backward (-x).
  const AnimationClip& hit = clipOf(AnimClip::Hit);
  sampleClip(m, hit, hit.duration * 0.25f, false, a);
  REQUIRE(jointPosition(m, a, kHead).x < headRest.x - 0.3f);

  // v0.53: the charge pose raises the banana hand up and back, further the
  // fuller the charge; at 0 it is the rest pose.
  REQUIRE(b.hand == 7);
  REQUIRE(b.upperArm == 5);
  REQUIRE(b.forearm == 6);
  REQUIRE(b.spine == 1);
  const AnimationClip& charge = clipOf(AnimClip::Charge);
  const Vec3 handRest = jointPosition(m, rest, 7);
  sampleClip(m, charge, 0.0f, false, a);
  REQUIRE(jointPosition(m, a, 7).y == Approx(handRest.y).margin(1e-3));
  sampleClip(m, charge, charge.duration * 0.5f, false, a);
  sampleClip(m, charge, charge.duration, false, c);
  const Vec3 handHalf = jointPosition(m, a, 7);
  const Vec3 handFull = jointPosition(m, c, 7);
  INFO("hand rest " << handRest.x << "," << handRest.y << " half " << handHalf.x << ","
                    << handHalf.y << " full " << handFull.x << "," << handFull.y);
  REQUIRE(handHalf.y > handRest.y + 0.3f);
  REQUIRE(handFull.y > handHalf.y);
  REQUIRE(handFull.y > headRest.y - 0.5f);  // up by the head
  REQUIRE(handFull.x < handRest.x - 0.3f);  // and behind
}
