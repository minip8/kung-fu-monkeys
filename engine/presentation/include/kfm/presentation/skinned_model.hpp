// A skinned character model in CPU memory (architecture §7): a triangle mesh
// with up to four joint influences per vertex, a joint hierarchy with rest
// transforms and inverse bind matrices, and keyframed animation clips.
//
// Everything is plain vectors filled once at load time (gltf_loader.hpp, or
// built by hand in tests); nothing here allocates per frame. The animation
// layer (animation.hpp) samples clips into poses and turns them into skin
// matrices; the renderer uploads the mesh once and the skin matrices per draw.
//
// Model space follows the world basis (world_basis.hpp): +y up, +x forward,
// feet on y = 0. Joints are ordered so that every parent precedes its
// children (parent < index), which the loader guarantees and validateModel()
// checks, so one forward pass computes global transforms.
#pragma once

#include "kfm/presentation/math.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kfm::presentation {

// Joint budget per skinned model. The skinned vertex shader takes the skin
// matrices as a uniform array of 4 vec4s per joint, so 16 joints are 64 vec4s
// (72 with the mvp and model matrices): far under the 256 vertex-shader vec4
// uniforms every WebGL2 implementation guarantees (GL_MAX_VERTEX_UNIFORM_VECTORS).
inline constexpr uint32_t kMaxSkinJoints = 16;
inline constexpr uint32_t kMaxJointInfluences = 4;

// --- quaternions ----------------------------------------------------------------
// Unit quaternions (x, y, z, w) for rotations, glTF's component order.

struct Quat {
  float x{0.0f};
  float y{0.0f};
  float z{0.0f};
  float w{1.0f};
  constexpr bool operator==(const Quat&) const = default;
};

[[nodiscard]] constexpr float dot(Quat a, Quat b) {
  return (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
}

// Hamilton product: rotates by b first, then by a.
[[nodiscard]] constexpr Quat operator*(Quat a, Quat b) {
  return {(a.w * b.x) + (a.x * b.w) + (a.y * b.z) - (a.z * b.y),
          (a.w * b.y) - (a.x * b.z) + (a.y * b.w) + (a.z * b.x),
          (a.w * b.z) + (a.x * b.y) - (a.y * b.x) + (a.z * b.w),
          (a.w * b.w) - (a.x * b.x) - (a.y * b.y) - (a.z * b.z)};
}

// Unit quaternion along q; a zero quaternion becomes the identity.
[[nodiscard]] inline Quat normalize(Quat q) {
  const float len = std::sqrt(dot(q, q));
  if (len <= 0.0f) return Quat{};
  const float inv = 1.0f / len;
  return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

// Rotation by `radians` about `axis` (need not be unit length; non-zero).
[[nodiscard]] Quat quatFromAxisAngle(Vec3 axis, float radians);

// Normalised linear interpolation along the shorter arc (t in [0, 1]).
[[nodiscard]] Quat nlerp(Quat a, Quat b, float t);
// Spherical linear interpolation along the shorter arc (t in [0, 1]); falls
// back to nlerp when the keys are nearly equal.
[[nodiscard]] Quat slerp(Quat a, Quat b, float t);

// Rotation matrix of a unit quaternion.
[[nodiscard]] Mat4 quatToMat4(Quat q);

// T * R * S, the glTF node transform.
[[nodiscard]] Mat4 composeTrs(Vec3 translation, Quat rotation, Vec3 scale);

// --- model ------------------------------------------------------------------------

// 72 bytes, tightly packed: position, normal, joint indices (as floats, so the
// shader takes a plain vec4 on every backend), weights (sum 1) and colour.
// v0.49: the colour is COLOR_0 (white when the file has none): rgb is the
// part's own colour, alpha its player-colour mask; the renderer draws
// mix(rgb, player colour, alpha), so a model without COLOR_0 is all player
// colour, as before.
struct SkinnedVertex {
  float px{0.0f};
  float py{0.0f};
  float pz{0.0f};
  float nx{0.0f};
  float ny{0.0f};
  float nz{0.0f};
  std::array<float, kMaxJointInfluences> joints{};
  std::array<float, kMaxJointInfluences> weights{};
  std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
};
static_assert(sizeof(SkinnedVertex) == 72);

struct Joint {
  std::string name;
  int16_t parent{-1};  // < own index; -1 for a root
  // Rest (bind-time node) transform, local to the parent joint.
  Vec3 translation{};
  Quat rotation{};
  Vec3 scale{1.0f, 1.0f, 1.0f};
  Mat4 inverseBind{Mat4::identity()};
  // Roots only: the world transform of non-joint ancestor nodes (an armature
  // node, say), applied before the joint's own transform. Identity otherwise.
  Mat4 base{Mat4::identity()};
};

enum class KeyInterpolation : uint8_t { Linear = 0, Step };

// One animated property of one joint: key times (seconds, increasing) and
// values. An empty track leaves the joint's rest value.
template <typename T>
struct Track {
  std::vector<float> times;
  std::vector<T> values;
  KeyInterpolation interpolation{KeyInterpolation::Linear};
  [[nodiscard]] bool empty() const { return times.empty(); }
};

struct JointTrack {
  Track<Vec3> translation;
  Track<Quat> rotation;
};

struct AnimationClip {
  std::string name;
  float duration{0.0f};            // seconds: the last key time over all tracks
  std::vector<JointTrack> joints;  // one per model joint
};

struct SkinnedModel {
  std::vector<SkinnedVertex> vertices;
  std::vector<uint16_t> indices;  // triangle list
  std::vector<Joint> joints;
  std::vector<AnimationClip> clips;
  Vec3 boundsMin{};  // bind pose, model space
  Vec3 boundsMax{};

  [[nodiscard]] bool empty() const { return vertices.empty() || joints.empty(); }
  // Bind-pose height above the feet (the renderer scales it to the player).
  [[nodiscard]] float height() const { return boundsMax.y; }
  // Index of the clip called `name`, or -1.
  [[nodiscard]] int findClip(std::string_view name) const;
};

// Recomputes boundsMin/boundsMax from the vertices.
void computeBounds(SkinnedModel& model);

// Checks the invariants the animation layer and the renderer rely on: 1 to
// kMaxSkinJoints joints with parent < index, a non-empty triangle list whose
// indices are in range, joint indices in range, weights that are
// non-negative and sum to 1 (within 1e-3), clip tracks sized to the joint
// count with matching times/values and increasing times. Returns false and
// sets `error` on the first violation.
[[nodiscard]] bool validateModel(const SkinnedModel& model, std::string& error);

}  // namespace kfm::presentation
