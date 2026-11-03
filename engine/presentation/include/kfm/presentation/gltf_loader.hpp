// glTF 2.0 loader for skinned characters (architecture §7), built on cgltf.
//
// Loads the first node that has both a mesh and a skin from a .gltf (with its
// .bin) or .glb file into a SkinnedModel:
//   - every triangle primitive of that mesh, merged: POSITION, NORMAL,
//     JOINTS_0, WEIGHTS_0 (any component type glTF allows; weights are
//     renormalised) and the indices (or 0..n-1 for a non-indexed primitive);
//   - the skin's joints, reordered so parents precede children, with their
//     node TRS as the rest pose and the inverse bind matrices (identity when
//     the skin has none); non-joint ancestors of a root joint fold into
//     Joint::base;
//   - every animation as a clip: translation and rotation channels that
//     target a joint, LINEAR or STEP. Scale, morph-weight and non-joint
//     channels are ignored; CUBICSPLINE samplers are rejected.
//
// Limits: at most kMaxSkinJoints joints, 65535 vertices (16-bit indices).
// Joints must use TRS, not a matrix. Failure (a missing file, a malformed
// file, a limit) returns false with a message and leaves `out` empty; it
// never throws (presentation builds without exceptions). Not built in
// KFM_HEADLESS builds (cgltf belongs to the client stack).
#pragma once

#include "kfm/presentation/skinned_model.hpp"

#include <string>

namespace kfm::presentation {

[[nodiscard]] bool loadSkinnedModel(const char* path, SkinnedModel& out, std::string& error);

}  // namespace kfm::presentation
