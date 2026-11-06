#include "kfm/presentation/gltf_loader.hpp"

#include <cgltf.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace kfm::presentation {

namespace {

// Frees the cgltf data on every return path.
class CgltfData {
 public:
  CgltfData() = default;
  CgltfData(const CgltfData&) = delete;
  CgltfData& operator=(const CgltfData&) = delete;
  CgltfData(CgltfData&&) = delete;
  CgltfData& operator=(CgltfData&&) = delete;
  ~CgltfData() {
    if (data_ != nullptr) cgltf_free(data_);
  }
  [[nodiscard]] cgltf_data** out() { return &data_; }
  [[nodiscard]] cgltf_data* get() const { return data_; }

 private:
  cgltf_data* data_{nullptr};
};

[[nodiscard]] const char* resultName(cgltf_result r) {
  switch (r) {
    case cgltf_result_success:
      return "success";
    case cgltf_result_data_too_short:
      return "data too short";
    case cgltf_result_unknown_format:
      return "unknown format";
    case cgltf_result_invalid_json:
      return "invalid JSON";
    case cgltf_result_invalid_gltf:
      return "invalid glTF";
    case cgltf_result_invalid_options:
      return "invalid options";
    case cgltf_result_file_not_found:
      return "file not found";
    case cgltf_result_io_error:
      return "I/O error";
    case cgltf_result_out_of_memory:
      return "out of memory";
    case cgltf_result_legacy_gltf:
      return "legacy glTF (1.0)";
    default:
      return "unknown error";
  }
}

[[nodiscard]] Mat4 matFromFloats(std::span<const float, 16> f) {
  Mat4 m;
  std::ranges::copy(f, m.m.begin());
  return m;
}

struct Loader {
  const cgltf_data& data;
  SkinnedModel& model;
  std::string& error;

  bool fail(std::string message) {
    error = std::move(message);
    return false;
  }

  [[nodiscard]] const cgltf_node* findSkinnedNode() const {
    for (const cgltf_node& node : std::span(data.nodes, data.nodes_count)) {
      if (node.mesh != nullptr && node.skin != nullptr) return &node;
    }
    return nullptr;
  }

  // Skin joint index of `node`, or -1.
  [[nodiscard]] static int jointIndexOf(const cgltf_skin& skin, const cgltf_node* node) {
    const std::span<cgltf_node*> joints(skin.joints, skin.joints_count);
    for (std::size_t j = 0; j < joints.size(); ++j) {
      if (joints[j] == node) return static_cast<int>(j);
    }
    return -1;
  }

  // Fills model.joints in skin order; `order` receives the parents-first
  // permutation (order[newIndex] = skin index) and `remap` its inverse.
  bool loadSkeleton(const cgltf_skin& skin, std::vector<int>& remap) {
    const std::size_t n = skin.joints_count;
    if (n == 0 || n > kMaxSkinJoints) {
      return fail("the skin has " + std::to_string(n) + " joints; 1 to " +
                  std::to_string(kMaxSkinJoints) + " are supported");
    }
    const std::span<cgltf_node*> nodes(skin.joints, n);
    std::vector<Joint> joints(n);
    std::vector<int> skinParent(n, -1);
    for (std::size_t j = 0; j < n; ++j) {
      const cgltf_node& node = *nodes[j];
      if (node.has_matrix != 0) {
        return fail("joint '" + std::string(node.name != nullptr ? node.name : "") +
                    "' uses a matrix transform; joints need translation/rotation/scale");
      }
      Joint& joint = joints[j];
      joint.name = node.name != nullptr ? node.name : "";
      if (node.has_translation != 0)
        joint.translation = {node.translation[0], node.translation[1], node.translation[2]};
      if (node.has_rotation != 0) {
        joint.rotation =
            normalize(Quat{node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]});
      }
      if (node.has_scale != 0) joint.scale = {node.scale[0], node.scale[1], node.scale[2]};
      // The nearest ancestor that is a joint is the parent; the world
      // transform of the non-joint ancestors in between becomes the base.
      const cgltf_node* ancestor = node.parent;
      while (ancestor != nullptr && jointIndexOf(skin, ancestor) < 0)
        ancestor = ancestor->parent;
      if (ancestor != nullptr) {
        skinParent[j] = jointIndexOf(skin, ancestor);
        if (node.parent != ancestor) {
          return fail("joint '" + joint.name + "' has a non-joint node between it and its parent");
        }
      } else if (node.parent != nullptr) {
        std::array<float, 16> world{};
        cgltf_node_transform_world(node.parent, world.data());
        joint.base = matFromFloats(world);
      }
      if (skin.inverse_bind_matrices != nullptr) {
        std::array<float, 16> ibm{};
        if (cgltf_accessor_read_float(skin.inverse_bind_matrices, j, ibm.data(), 16) == 0)
          return fail("cannot read inverse bind matrix " + std::to_string(j));
        joint.inverseBind = matFromFloats(ibm);
      }
    }

    // Parents first: repeatedly place the joints whose parent is placed.
    remap.assign(n, -1);
    std::vector<int> order;
    order.reserve(n);
    while (order.size() < n) {
      const std::size_t before = order.size();
      for (std::size_t j = 0; j < n; ++j) {
        if (remap[j] >= 0) continue;
        const int p = skinParent[j];
        if (p < 0 || remap[static_cast<std::size_t>(p)] >= 0) {
          remap[j] = static_cast<int>(order.size());
          order.push_back(static_cast<int>(j));
        }
      }
      if (order.size() == before) return fail("the joint hierarchy has a cycle");
    }
    model.joints.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      const auto src = static_cast<std::size_t>(order[i]);
      model.joints[i] = std::move(joints[src]);
      const int p = skinParent[src];
      model.joints[i].parent =
          static_cast<int16_t>(p < 0 ? -1 : remap[static_cast<std::size_t>(p)]);
    }
    return true;
  }

  bool loadPrimitive(const cgltf_primitive& prim, const std::vector<int>& remap) {
    if (prim.type != cgltf_primitive_type_triangles)
      return fail("only triangle-list primitives are supported");
    const cgltf_accessor* position = nullptr;
    const cgltf_accessor* normal = nullptr;
    const cgltf_accessor* joints = nullptr;
    const cgltf_accessor* weights = nullptr;
    const cgltf_accessor* color = nullptr;  // optional (v0.49)
    for (const cgltf_attribute& a : std::span(prim.attributes, prim.attributes_count)) {
      if (a.type == cgltf_attribute_type_position) position = a.data;
      if (a.type == cgltf_attribute_type_normal) normal = a.data;
      if (a.type == cgltf_attribute_type_joints && a.index == 0) joints = a.data;
      if (a.type == cgltf_attribute_type_weights && a.index == 0) weights = a.data;
      if (a.type == cgltf_attribute_type_color && a.index == 0) color = a.data;
    }
    if (position == nullptr || normal == nullptr || joints == nullptr || weights == nullptr)
      return fail("a primitive lacks POSITION, NORMAL, JOINTS_0 or WEIGHTS_0");
    const std::size_t count = position->count;
    if (normal->count != count || joints->count != count || weights->count != count ||
        (color != nullptr && color->count != count))
      return fail("a primitive's attributes have different counts");
    // COLOR_0 is VEC3 or VEC4 (glTF 2.0 §3.7.2.1); a VEC3 colour has alpha 1.
    if (color != nullptr && color->type != cgltf_type_vec3 && color->type != cgltf_type_vec4)
      return fail("COLOR_0 must be VEC3 or VEC4");
    const cgltf_size colorComponents = color != nullptr && color->type == cgltf_type_vec3 ? 3 : 4;
    const std::size_t base = model.vertices.size();
    if (base + count > 65535) return fail("the mesh has more than 65535 vertices");

    for (std::size_t i = 0; i < count; ++i) {
      std::array<float, 3> p{};
      std::array<float, 3> nrm{};
      std::array<cgltf_uint, 4> j{};
      std::array<float, 4> w{};
      if (cgltf_accessor_read_float(position, i, p.data(), 3) == 0 ||
          cgltf_accessor_read_float(normal, i, nrm.data(), 3) == 0 ||
          cgltf_accessor_read_uint(joints, i, j.data(), 4) == 0 ||
          cgltf_accessor_read_float(weights, i, w.data(), 4) == 0) {
        return fail("cannot read vertex " + std::to_string(i));
      }
      SkinnedVertex v{};
      if (color != nullptr) {
        std::array<float, 4> c{1.0f, 1.0f, 1.0f, 1.0f};
        if (cgltf_accessor_read_float(color, i, c.data(), colorComponents) == 0)
          return fail("cannot read the colour of vertex " + std::to_string(i));
        for (std::size_t k = 0; k < 4; ++k)
          v.color[k] = std::clamp(c[k], 0.0f, 1.0f);
      }
      v.px = p[0];
      v.py = p[1];
      v.pz = p[2];
      const Vec3 n = normalize(Vec3{nrm[0], nrm[1], nrm[2]});
      v.nx = n.x;
      v.ny = n.y;
      v.nz = n.z;
      float sum = 0.0f;
      for (std::size_t k = 0; k < kMaxJointInfluences; ++k) {
        const float wk = std::max(w[k], 0.0f);
        if (wk > 0.0f && j[k] >= remap.size())
          return fail("vertex " + std::to_string(i) + " references joint " + std::to_string(j[k]));
        v.joints[k] = j[k] < remap.size() ? static_cast<float>(remap[j[k]]) : 0.0f;
        v.weights[k] = wk;
        sum += wk;
      }
      if (sum > 0.0f) {
        for (float& wk : v.weights)
          wk /= sum;
      } else {
        v.weights = {1.0f, 0.0f, 0.0f, 0.0f};  // unweighted: follow the first joint
        v.joints = {0.0f, 0.0f, 0.0f, 0.0f};
      }
      model.vertices.push_back(v);
    }
    if (prim.indices != nullptr) {
      for (std::size_t i = 0; i < prim.indices->count; ++i) {
        const cgltf_size index = cgltf_accessor_read_index(prim.indices, i);
        if (index >= count) return fail("index " + std::to_string(index) + " is out of range");
        model.indices.push_back(static_cast<uint16_t>(base + index));
      }
    } else {
      for (std::size_t i = 0; i < count; ++i)
        model.indices.push_back(static_cast<uint16_t>(base + i));
    }
    return true;
  }

  template <typename T, std::size_t N>
  bool readTrack(const cgltf_animation_sampler& sampler, Track<T>& track, const char* clip) {
    const cgltf_accessor* in = sampler.input;
    const cgltf_accessor* outAcc = sampler.output;
    if (in == nullptr || outAcc == nullptr || in->count == 0 || outAcc->count != in->count)
      return fail(std::string("clip '") + clip + "' has a malformed sampler");
    track.interpolation = sampler.interpolation == cgltf_interpolation_type_step
                              ? KeyInterpolation::Step
                              : KeyInterpolation::Linear;
    track.times.resize(in->count);
    track.values.resize(in->count);
    for (std::size_t k = 0; k < in->count; ++k) {
      std::array<float, N> v{};
      if (cgltf_accessor_read_float(in, k, &track.times[k], 1) == 0 ||
          cgltf_accessor_read_float(outAcc, k, v.data(), N) == 0) {
        return fail(std::string("clip '") + clip + "' has an unreadable key");
      }
      if constexpr (N == 4) {
        track.values[k] = normalize(Quat{v[0], v[1], v[2], v[3]});
      } else {
        track.values[k] = Vec3{v[0], v[1], v[2]};
      }
    }
    return true;
  }

  bool loadAnimations(const cgltf_skin& skin, const std::vector<int>& remap) {
    const std::span<const cgltf_animation> animations(data.animations, data.animations_count);
    for (std::size_t a = 0; a < animations.size(); ++a) {
      const cgltf_animation& anim = animations[a];
      AnimationClip clip;
      clip.name = anim.name != nullptr ? anim.name : "animation" + std::to_string(a);
      clip.joints.resize(model.joints.size());
      for (const cgltf_animation_channel& ch : std::span(anim.channels, anim.channels_count)) {
        if (ch.sampler == nullptr || ch.target_node == nullptr) continue;
        const int skinIndex = jointIndexOf(skin, ch.target_node);
        if (skinIndex < 0) continue;
        if (ch.sampler->interpolation == cgltf_interpolation_type_cubic_spline)
          return fail("clip '" + clip.name + "' uses CUBICSPLINE keys; LINEAR or STEP only");
        JointTrack& jt =
            clip.joints[static_cast<std::size_t>(remap[static_cast<std::size_t>(skinIndex)])];
        bool ok = true;
        if (ch.target_path == cgltf_animation_path_type_translation) {
          ok = readTrack<Vec3, 3>(*ch.sampler, jt.translation, clip.name.c_str());
          if (ok) clip.duration = std::max(clip.duration, jt.translation.times.back());
        } else if (ch.target_path == cgltf_animation_path_type_rotation) {
          ok = readTrack<Quat, 4>(*ch.sampler, jt.rotation, clip.name.c_str());
          if (ok) clip.duration = std::max(clip.duration, jt.rotation.times.back());
        }
        if (!ok) return false;
      }
      model.clips.push_back(std::move(clip));
    }
    return true;
  }

  bool load() {
    const cgltf_node* node = findSkinnedNode();
    if (node == nullptr) return fail("no node has both a mesh and a skin");
    std::vector<int> remap;
    if (!loadSkeleton(*node->skin, remap)) return false;
    for (const cgltf_primitive& prim :
         std::span(node->mesh->primitives, node->mesh->primitives_count)) {
      if (!loadPrimitive(prim, remap)) return false;
    }
    if (!loadAnimations(*node->skin, remap)) return false;
    computeBounds(model);
    return validateModel(model, error);
  }
};

}  // namespace

bool loadSkinnedModel(const char* path, SkinnedModel& out, std::string& error) {
  out = SkinnedModel{};
  if (path == nullptr || *path == '\0') {
    error = "no model path given";
    return false;
  }
  const cgltf_options options{};
  CgltfData data;
  cgltf_result r = cgltf_parse_file(&options, path, data.out());
  if (r == cgltf_result_success) r = cgltf_load_buffers(&options, data.get(), path);
  if (r == cgltf_result_success) r = cgltf_validate(data.get());
  if (r != cgltf_result_success) {
    error = std::string("cannot load '") + path + "': " + resultName(r);
    return false;
  }
  Loader loader{.data = *data.get(), .model = out, .error = error};
  if (!loader.load()) {
    error = std::string("'") + path + "': " + error;
    out = SkinnedModel{};
    return false;
  }
  return true;
}

}  // namespace kfm::presentation
