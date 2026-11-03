#include "kfm/presentation/math.hpp"

#include <cmath>

namespace kfm::presentation {

Mat4 inverse(const Mat4& a) {
  const std::array<float, 16>& m = a.m;
  std::array<float, 16> inv{};
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
           m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
           m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
           m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
            m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
           m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
           m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
           m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
            m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
           m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
           m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
            m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
            m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
           m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
           m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
            m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
            m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

  const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (det == 0.0f || !std::isfinite(det)) return Mat4::identity();
  const float invDet = 1.0f / det;
  Mat4 r;
  for (std::size_t i = 0; i < 16; ++i)
    r.m[i] = inv[i] * invDet;
  return r;
}

Mat4 perspective(float fovY, float aspect, float zNear, float zFar, ClipDepth depth) {
  const float f = 1.0f / std::tan(fovY * 0.5f);
  Mat4 r;
  r.at(0, 0) = f / aspect;
  r.at(1, 1) = f;
  r.at(3, 2) = -1.0f;
  if (depth == ClipDepth::NegativeOneToOne) {
    r.at(2, 2) = (zFar + zNear) / (zNear - zFar);
    r.at(2, 3) = (2.0f * zFar * zNear) / (zNear - zFar);
  } else {
    r.at(2, 2) = zFar / (zNear - zFar);
    r.at(2, 3) = (zNear * zFar) / (zNear - zFar);
  }
  return r;
}

Mat4 orthographic(float left, float right, float bottom, float top, float zNear, float zFar,
                  ClipDepth depth) {
  Mat4 r = Mat4::identity();
  r.at(0, 0) = 2.0f / (right - left);
  r.at(1, 1) = 2.0f / (top - bottom);
  r.at(0, 3) = -(right + left) / (right - left);
  r.at(1, 3) = -(top + bottom) / (top - bottom);
  if (depth == ClipDepth::NegativeOneToOne) {
    r.at(2, 2) = -2.0f / (zFar - zNear);
    r.at(2, 3) = -(zFar + zNear) / (zFar - zNear);
  } else {
    r.at(2, 2) = -1.0f / (zFar - zNear);
    r.at(2, 3) = -zNear / (zFar - zNear);
  }
  return r;
}

Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
  const Vec3 f = normalize(target - eye);
  const Vec3 s = normalize(cross(f, up));
  const Vec3 u = cross(s, f);
  Mat4 r = Mat4::identity();
  r.at(0, 0) = s.x;
  r.at(0, 1) = s.y;
  r.at(0, 2) = s.z;
  r.at(1, 0) = u.x;
  r.at(1, 1) = u.y;
  r.at(1, 2) = u.z;
  r.at(2, 0) = -f.x;
  r.at(2, 1) = -f.y;
  r.at(2, 2) = -f.z;
  r.at(0, 3) = -dot(s, eye);
  r.at(1, 3) = -dot(u, eye);
  r.at(2, 3) = dot(f, eye);
  return r;
}

}  // namespace kfm::presentation
