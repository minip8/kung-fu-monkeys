#include "kfm/presentation/skinned_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace kfm::presentation {

Quat quatFromAxisAngle(Vec3 axis, float radians) {
  const Vec3 a = normalize(axis);
  const float s = std::sin(radians * 0.5f);
  return {a.x * s, a.y * s, a.z * s, std::cos(radians * 0.5f)};
}

Quat nlerp(Quat a, Quat b, float t) {
  if (dot(a, b) < 0.0f) b = {-b.x, -b.y, -b.z, -b.w};
  return normalize({lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t), lerp(a.w, b.w, t)});
}

Quat slerp(Quat a, Quat b, float t) {
  float cosTheta = dot(a, b);
  if (cosTheta < 0.0f) {
    b = {-b.x, -b.y, -b.z, -b.w};
    cosTheta = -cosTheta;
  }
  if (cosTheta > 0.9995f) return nlerp(a, b, t);
  const float theta = std::acos(std::min(cosTheta, 1.0f));
  const float sinTheta = std::sin(theta);
  const float wa = std::sin((1.0f - t) * theta) / sinTheta;
  const float wb = std::sin(t * theta) / sinTheta;
  return normalize({(a.x * wa) + (b.x * wb), (a.y * wa) + (b.y * wb), (a.z * wa) + (b.z * wb),
                    (a.w * wa) + (b.w * wb)});
}

Mat4 quatToMat4(Quat q) {
  const float xx = q.x * q.x;
  const float yy = q.y * q.y;
  const float zz = q.z * q.z;
  const float xy = q.x * q.y;
  const float xz = q.x * q.z;
  const float yz = q.y * q.z;
  const float wx = q.w * q.x;
  const float wy = q.w * q.y;
  const float wz = q.w * q.z;
  Mat4 r = Mat4::identity();
  r.at(0, 0) = 1.0f - (2.0f * (yy + zz));
  r.at(0, 1) = 2.0f * (xy - wz);
  r.at(0, 2) = 2.0f * (xz + wy);
  r.at(1, 0) = 2.0f * (xy + wz);
  r.at(1, 1) = 1.0f - (2.0f * (xx + zz));
  r.at(1, 2) = 2.0f * (yz - wx);
  r.at(2, 0) = 2.0f * (xz - wy);
  r.at(2, 1) = 2.0f * (yz + wx);
  r.at(2, 2) = 1.0f - (2.0f * (xx + yy));
  return r;
}

Mat4 composeTrs(Vec3 translation, Quat rotation, Vec3 scaleFactor) {
  Mat4 r = quatToMat4(rotation);
  for (int row = 0; row < 3; ++row) {
    r.at(row, 0) *= scaleFactor.x;
    r.at(row, 1) *= scaleFactor.y;
    r.at(row, 2) *= scaleFactor.z;
  }
  r.at(0, 3) = translation.x;
  r.at(1, 3) = translation.y;
  r.at(2, 3) = translation.z;
  return r;
}

int SkinnedModel::findClip(std::string_view name) const {
  for (std::size_t i = 0; i < clips.size(); ++i) {
    if (clips[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

void computeBounds(SkinnedModel& model) {
  if (model.vertices.empty()) {
    model.boundsMin = {};
    model.boundsMax = {};
    return;
  }
  Vec3 lo{model.vertices[0].px, model.vertices[0].py, model.vertices[0].pz};
  Vec3 hi = lo;
  for (const SkinnedVertex& v : model.vertices) {
    lo = {std::min(lo.x, v.px), std::min(lo.y, v.py), std::min(lo.z, v.pz)};
    hi = {std::max(hi.x, v.px), std::max(hi.y, v.py), std::max(hi.z, v.pz)};
  }
  model.boundsMin = lo;
  model.boundsMax = hi;
}

namespace {

template <typename T>
[[nodiscard]] bool validTrack(const Track<T>& track) {
  if (track.times.size() != track.values.size()) return false;
  for (std::size_t k = 1; k < track.times.size(); ++k) {
    if (!(track.times[k] >= track.times[k - 1])) return false;
  }
  return true;
}

}  // namespace

bool validateModel(const SkinnedModel& model, std::string& error) {
  const std::size_t jointCount = model.joints.size();
  if (jointCount == 0 || jointCount > kMaxSkinJoints) {
    error = "the skeleton has " + std::to_string(jointCount) + " joints; 1 to " +
            std::to_string(kMaxSkinJoints) + " are supported";
    return false;
  }
  for (std::size_t j = 0; j < jointCount; ++j) {
    const int parent = model.joints[j].parent;
    if (parent < -1 || std::cmp_greater_equal(parent, j)) {
      error = "joint " + std::to_string(j) + " has parent " + std::to_string(parent) +
              "; parents must precede their children";
      return false;
    }
  }
  if (model.vertices.empty() || model.indices.empty() || model.indices.size() % 3 != 0) {
    error = "the mesh has no triangles";
    return false;
  }
  for (const uint16_t i : model.indices) {
    if (i >= model.vertices.size()) {
      error = "index " + std::to_string(i) + " is out of range";
      return false;
    }
  }
  for (std::size_t v = 0; v < model.vertices.size(); ++v) {
    const SkinnedVertex& vert = model.vertices[v];
    float sum = 0.0f;
    for (std::size_t k = 0; k < kMaxJointInfluences; ++k) {
      const float w = vert.weights[k];
      const float j = vert.joints[k];
      if (w < 0.0f || j < 0.0f || j >= static_cast<float>(jointCount) || j != std::floor(j)) {
        error = "vertex " + std::to_string(v) + " has a bad joint influence";
        return false;
      }
      sum += w;
    }
    if (std::fabs(sum - 1.0f) > 1e-3f) {
      error = "vertex " + std::to_string(v) + " has weights summing to " + std::to_string(sum);
      return false;
    }
  }
  for (const AnimationClip& clip : model.clips) {
    if (clip.joints.size() != jointCount) {
      error = "clip '" + clip.name + "' does not have one track set per joint";
      return false;
    }
    for (const JointTrack& t : clip.joints) {
      if (!validTrack(t.translation) || !validTrack(t.rotation)) {
        error = "clip '" + clip.name + "' has a malformed track";
        return false;
      }
    }
  }
  return true;
}

}  // namespace kfm::presentation
